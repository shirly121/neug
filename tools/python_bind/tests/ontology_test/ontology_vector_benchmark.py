#!/usr/bin/env python3
"""Mixed read-only ontology vector-search throughput benchmark."""

import argparse
import json
import logging
import random
import statistics
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from pathlib import Path

from neug import Session
from neug.database import Database

CLIENTS = 4
DIMENSION = 512
DEFAULT_WEIGHTS = {
    "vector_only": 50,
    "vector_then_graph": 50,
    "scalar_then_vector": 0,
    "graph_then_vector": 0,
}


@dataclass
class WorkloadStats:
    ok: int
    errors: int
    latencies_ms: list


def array_literal(values):
    return "[" + ",".join(format(float(value), ".9g") for value in values) + "]"


def percentile(values, percentage):
    if not values:
        return None
    ordered = sorted(values)
    index = max(0, min(len(ordered) - 1, int(len(ordered) * percentage) - 1))
    return ordered[index]


def load_database(data_dir, db_path):
    entity_path = data_dir / "entity.jsonl"
    product_path = data_dir / "product.jsonl"
    rel_ep_path = data_dir / "rel_ep.jsonl"
    for path in (entity_path, product_path, rel_ep_path):
        if not path.is_file():
            raise FileNotFoundError(f"ontology data file is unavailable: {path}")

    db = Database(db_path=str(db_path), mode="w")
    conn = db.connect()
    conn.execute("LOAD hybrid_search;")
    conn.execute(
        "CREATE NODE TABLE Entity("
        "uid STRING PRIMARY KEY, name STRING, description STRING, "
        "entity_type STRING, product STRING, authority INT64, kg_id STRING, "
        "embedding FLOAT[512]);"
    )
    conn.execute(
        "CREATE NODE TABLE Product("
        "name STRING PRIMARY KEY, uid STRING, description STRING);"
    )
    conn.execute(
        "CREATE REL TABLE rel_ep("
        "FROM Entity TO Product, rel_type STRING, content STRING);"
    )
    conn.execute(
        f'COPY Entity FROM (LOAD FROM "{entity_path}" '
        "RETURN uid, name, description, entity_type, product, authority, "
        "kg_id, CAST(embedding, 'FLOAT[512]'));"
    )
    conn.execute(
        f'COPY Product FROM (LOAD FROM "{product_path}" '
        "RETURN name, uid, description);"
    )
    if rel_ep_path.stat().st_size:
        conn.execute(
            f'COPY rel_ep FROM (LOAD FROM "{rel_ep_path}" '
            "RETURN source, target, rel_type, content) "
            '(from="Entity", to="Product");'
        )
    conn.execute(
        "CREATE INDEX entity_embedding_hnsw ON Entity USING HNSW (embedding) "
        "WITH (metric = 'ip', m = 16, ef_construction = 200);"
    )
    conn.close()
    db.close()


def load_query_vectors(data_dir, pool_size, seed):
    vectors = []
    with (data_dir / "entity.jsonl").open(encoding="utf-8") as stream:
        for line in stream:
            embedding = json.loads(line)["embedding"]
            if len(embedding) != DIMENSION:
                raise ValueError(f"expected {DIMENSION}-dimensional embedding")
            vectors.append(embedding)
    if not vectors:
        raise ValueError("entity.jsonl contains no query vectors")
    rng = random.Random(seed)
    selected = rng.sample(vectors, min(pool_size, len(vectors)))
    return [array_literal(vector) for vector in selected]


def build_query_pool(vector_literals, topk):
    queries = {name: [] for name in DEFAULT_WEIGHTS}
    for vector in vector_literals:
        queries["vector_only"].append(
            "MATCH (n:Entity) "
            "RETURN n.uid, n.name, n.description, n.entity_type, "
            "n.product, n.authority, "
            f"vector_distance_ip(n.embedding, {vector}) AS score "
            f"ORDER BY score DESC LIMIT {topk};"
        )
        queries["vector_then_graph"].append(
            "MATCH (n:Entity) "
            f"WITH n, vector_distance_ip(n.embedding, {vector}) AS score "
            f"ORDER BY score DESC LIMIT {topk} "
            "MATCH (n)-[r:rel_ep]->(p:Product) "
            "RETURN n.uid, n.name, score, p.name, r.rel_type, r.content "
            "ORDER BY score DESC;"
        )
        queries["scalar_then_vector"].append(
            "MATCH (n:Entity) "
            "WHERE n.product = 'MaxCompute' AND n.authority >= 2 "
            "RETURN n.uid, n.name, n.description, n.authority, "
            f"vector_distance_ip(n.embedding, {vector}) AS score "
            f"ORDER BY score DESC LIMIT {topk};"
        )
        queries["graph_then_vector"].append(
            "MATCH (n:Entity)-[:rel_ep]->"
            "(p:Product {name: 'MaxCompute'}) "
            "WITH n RETURN n.uid, n.name, n.description, "
            f"vector_distance_ip(n.embedding, {vector}) AS score "
            f"ORDER BY score DESC LIMIT {topk};"
        )
    return queries


def parse_weights(value):
    weights = {}
    for item in value.split(","):
        name, weight = item.split("=", 1)
        if name not in DEFAULT_WEIGHTS:
            raise ValueError(f"unknown workload: {name}")
        weights[name] = int(weight)
    if (
        set(weights) != set(DEFAULT_WEIGHTS)
        or any(weight < 0 for weight in weights.values())
        or not any(weights.values())
    ):
        raise ValueError(
            "weights must contain every workload, be non-negative, "
            "and include at least one positive value"
        )
    return weights


def validate_warmup_result(name, rows, topk):
    if not rows:
        raise AssertionError(f"{name} returned no rows during warmup")

    score_column = {
        "vector_only": 6,
        "vector_then_graph": 2,
        "scalar_then_vector": 4,
        "graph_then_vector": 3,
    }[name]
    scores = [row[score_column] for row in rows]
    if any(left < right for left, right in zip(scores, scores[1:])):
        raise AssertionError(f"{name} results are not ordered by score")

    if name in {"vector_only", "scalar_then_vector", "graph_then_vector"}:
        if len(rows) != topk:
            raise AssertionError(f"{name} returned {len(rows)} rows, expected {topk}")
        uids = [row[0] for row in rows]
        if len(uids) != len(set(uids)):
            raise AssertionError(f"{name} returned duplicate entity UIDs")


def run_benchmark(
    db_path, queries, weights, duration, warmup, server_threads, port, seed, topk
):
    db = Database(db_path=str(db_path))
    connection = db.connect()
    connection.execute("LOAD hybrid_search;")
    connection.close()
    uri = db.serve(
        host="localhost",
        port=port,
        blocking=False,
        thread_num=server_threads,
    )
    barrier = threading.Barrier(CLIENTS + 1)
    timing = {}
    stats = {name: WorkloadStats(ok=0, errors=0, latencies_ms=[]) for name in weights}
    error_samples = []
    lock = threading.Lock()
    names = list(weights)
    selection_weights = [weights[name] for name in names]

    def worker(worker_id):
        rng = random.Random(seed + worker_id)
        session = Session(endpoint=uri, timeout="30s", num_threads=1)
        barrier.wait()
        while time.perf_counter() < timing["stop_at"]:
            name = rng.choices(names, weights=selection_weights, k=1)[0]
            query = rng.choice(queries[name])
            started = time.perf_counter()
            try:
                rows = list(session.execute(query, access_mode="read"))
                elapsed_ms = (time.perf_counter() - started) * 1000
                if started < timing["measure_at"]:
                    validate_warmup_result(name, rows, topk)
                else:
                    with lock:
                        stats[name].ok += 1
                        stats[name].latencies_ms.append(elapsed_ms)
            except Exception as error:
                if started < timing["measure_at"]:
                    raise RuntimeError(
                        f"warmup correctness validation failed for {name}"
                    ) from error
                with lock:
                    stats[name].errors += 1
                    if len(error_samples) < 10:
                        error_samples.append(f"{name}: {error}")
        session.close()

    try:
        with ThreadPoolExecutor(max_workers=CLIENTS) as executor:
            futures = [
                executor.submit(worker, worker_id) for worker_id in range(CLIENTS)
            ]
            now = time.perf_counter()
            timing["measure_at"] = now + warmup
            timing["stop_at"] = timing["measure_at"] + duration
            barrier.wait()
            for future in futures:
                future.result()
    finally:
        db.stop_serving()
        db.close()

    return stats, error_samples


def print_results(stats, duration, weights):
    all_latencies = []
    total_ok = 0
    total_errors = 0
    print("\nMixed workload results")
    print("| Workload | Weight | OK | Errors | QPS | P50 ms | P95 ms | P99 ms |")
    print("|---|---:|---:|---:|---:|---:|---:|---:|")
    for name in weights:
        item = stats[name]
        total_ok += item.ok
        total_errors += item.errors
        all_latencies.extend(item.latencies_ms)
        p50 = (
            f"{statistics.median(item.latencies_ms):.2f}" if item.latencies_ms else "-"
        )
        p95 = f"{percentile(item.latencies_ms, 0.95):.2f}" if item.latencies_ms else "-"
        p99 = f"{percentile(item.latencies_ms, 0.99):.2f}" if item.latencies_ms else "-"
        print(
            f"| {name} | {weights[name]} | {item.ok} | {item.errors} | "
            f"{item.ok / duration:.2f} | {p50} | {p95} | {p99} |"
        )
    print(
        f"\nclients={CLIENTS}, total_qps={total_ok / duration:.2f}, "
        f"ok={total_ok}, errors={total_errors}, "
        f"p50={statistics.median(all_latencies):.2f}ms, "
        f"p95={percentile(all_latencies, 0.95):.2f}ms, "
        f"p99={percentile(all_latencies, 0.99):.2f}ms"
    )


def main():
    logging.getLogger("neug.session").setLevel(logging.WARNING)

    parser = argparse.ArgumentParser()
    parser.add_argument("--data-dir", type=Path, required=True)
    parser.add_argument("--db-path", type=Path, required=True)
    parser.add_argument("--skip-load", action="store_true")
    parser.add_argument("--duration", type=int, default=60)
    parser.add_argument("--warmup", type=int, default=10)
    parser.add_argument("--server-threads", type=int, default=4)
    parser.add_argument("--port", type=int, default=10000)
    parser.add_argument("--topk", type=int, default=20)
    parser.add_argument("--query-pool-size", type=int, default=100)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument(
        "--weights",
        default=",".join(
            f"{name}={weight}" for name, weight in DEFAULT_WEIGHTS.items()
        ),
    )
    args = parser.parse_args()

    if not args.skip_load:
        load_database(args.data_dir, args.db_path)
    elif not args.db_path.exists():
        raise FileNotFoundError(f"database is unavailable: {args.db_path}")

    weights = parse_weights(args.weights)
    vectors = load_query_vectors(args.data_dir, args.query_pool_size, args.seed)
    queries = build_query_pool(vectors, args.topk)
    stats, errors = run_benchmark(
        args.db_path,
        queries,
        weights,
        args.duration,
        args.warmup,
        args.server_threads,
        args.port,
        args.seed,
        args.topk,
    )
    print_results(stats, args.duration, weights)
    if errors:
        print("\nError samples:")
        for error in errors:
            print(f"  {error}")
    return 1 if any(item.errors for item in stats.values()) else 0


if __name__ == "__main__":
    raise SystemExit(main())

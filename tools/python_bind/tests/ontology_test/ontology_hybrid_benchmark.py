#!/usr/bin/env python3
"""Mixed ontology vector and full-text-search throughput benchmark."""

import argparse
import logging
import random
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

from ontology_fts_benchmark import build_query_pool as build_fts_query_pool
from ontology_fts_benchmark import load_query_terms
from ontology_fts_benchmark import validate_warmup_result
from ontology_vector_benchmark import CLIENTS
from ontology_vector_benchmark import WorkloadStats
from ontology_vector_benchmark import build_query_pool as build_vector_query_pool
from ontology_vector_benchmark import load_query_vectors
from ontology_vector_benchmark import print_results
from ontology_vector_benchmark import \
    validate_warmup_result as validate_vector_warmup_result

from neug import Session
from neug.database import Database

DEFAULT_WEIGHTS = {
    "vector_only": 25,
    "vector_then_graph": 25,
    "fts_only": 25,
    "fts_then_graph": 25,
}


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
    conn.execute(
        "CREATE INDEX entity_description_fts "
        "ON Entity USING SQLITE_FTS (description);"
    )
    conn.close()
    db.close()


def build_query_pool(data_dir, pool_size, seed, topk):
    vectors = load_query_vectors(data_dir, pool_size, seed)
    terms = load_query_terms(data_dir, pool_size, seed)
    vector_queries = build_vector_query_pool(vectors, topk)
    fts_queries = build_fts_query_pool(terms, topk)
    return {
        "vector_only": vector_queries["vector_only"],
        "vector_then_graph": vector_queries["vector_then_graph"],
        "fts_only": fts_queries["fts_only"],
        "fts_then_graph": fts_queries["fts_then_graph"],
    }


def validate_result(name, rows, topk):
    if name.startswith("vector_"):
        validate_vector_warmup_result(name, rows, topk)
    else:
        validate_warmup_result(name, rows, topk)


def run_benchmark(db_path, queries, duration, warmup, server_threads, port, seed, topk):
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
    stats = {
        name: WorkloadStats(ok=0, errors=0, latencies_ms=[]) for name in DEFAULT_WEIGHTS
    }
    error_samples = []
    lock = threading.Lock()
    names = list(DEFAULT_WEIGHTS)
    selection_weights = [DEFAULT_WEIGHTS[name] for name in names]

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
                    validate_result(name, rows, topk)
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


def main():
    logging.getLogger("neug.session").setLevel(logging.WARNING)

    parser = argparse.ArgumentParser()
    parser.add_argument("--data-dir", type=Path, required=True)
    parser.add_argument("--db-path", type=Path, required=True)
    parser.add_argument("--skip-load", action="store_true")
    parser.add_argument("--duration", type=int, default=300)
    parser.add_argument("--warmup", type=int, default=10)
    parser.add_argument("--server-threads", type=int, default=4)
    parser.add_argument("--port", type=int, default=10000)
    parser.add_argument("--topk", type=int, default=20)
    parser.add_argument("--query-pool-size", type=int, default=100)
    parser.add_argument("--seed", type=int, default=42)
    args = parser.parse_args()

    if not args.skip_load:
        load_database(args.data_dir, args.db_path)
    elif not args.db_path.exists():
        raise FileNotFoundError(f"database is unavailable: {args.db_path}")

    queries = build_query_pool(
        args.data_dir,
        args.query_pool_size,
        args.seed,
        args.topk,
    )
    stats, errors = run_benchmark(
        args.db_path,
        queries,
        args.duration,
        args.warmup,
        args.server_threads,
        args.port,
        args.seed,
        args.topk,
    )
    print_results(stats, args.duration, DEFAULT_WEIGHTS)
    if errors:
        print("\nError samples:")
        for error in errors:
            print(f"  {error}")
    return 1 if any(item.errors for item in stats.values()) else 0


if __name__ == "__main__":
    raise SystemExit(main())

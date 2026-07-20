#!/usr/bin/env python3
"""Add BGE embeddings to Entity records for the ontology HNSW test."""

import argparse
import json
from pathlib import Path

from sentence_transformers import SentenceTransformer

DEFAULT_MODEL = "BAAI/bge-small-zh-v1.5"
DEFAULT_QUERY = "Flink如何写入MaxCompute"
SPLIT_FILENAMES = {
    "Product": "product.jsonl",
    "rel_ee": "rel_ee.jsonl",
    "rel_ep": "rel_ep.jsonl",
    "rel_pe": "rel_pe.jsonl",
    "rel_pp": "rel_pp.jsonl",
}


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path, help="Source unified_graph.jsonl")
    parser.add_argument(
        "output",
        type=Path,
        help="Import-ready Entity JSONL (use <data-dir>/entity.jsonl)",
    )
    parser.add_argument(
        "--query-output",
        type=Path,
        help="Query vector JSON (default: <output>.query.json)",
    )
    parser.add_argument("--model", default=DEFAULT_MODEL)
    parser.add_argument("--query", default=DEFAULT_QUERY)
    parser.add_argument("--batch-size", type=int, default=64)
    parser.add_argument(
        "--split-only",
        action="store_true",
        help="Split Product and relationship files without regenerating Entity embeddings",
    )
    return parser.parse_args()


def load_entities(path):
    entities = []
    with path.open(encoding="utf-8") as stream:
        for line in stream:
            value = json.loads(line)
            if value.get("type") == "node" and value.get("node_type") == "Entity":
                entities.append(value)
    return entities


def split_non_entity_records(path, output_dir):
    entity_ids = set()
    products = {}
    edges = []
    with path.open(encoding="utf-8") as stream:
        for line in stream:
            value = json.loads(line)
            if value.get("type") == "node":
                if value.get("node_type") == "Entity":
                    entity_ids.add(value["id"])
                elif value.get("node_type") == "Product":
                    products[value["id"]] = value
            elif value.get("type") == "edge":
                edges.append(value)

    output_dir.mkdir(parents=True, exist_ok=True)
    output_paths = {
        name: output_dir / filename for name, filename in SPLIT_FILENAMES.items()
    }
    counts = {name: 0 for name in output_paths}
    streams = {
        name: output_path.open("w", encoding="utf-8")
        for name, output_path in output_paths.items()
    }
    try:
        for product in products.values():
            record = {
                "name": product["name"],
                "uid": product["id"],
                "description": product.get("description", ""),
            }
            streams["Product"].write(
                json.dumps(record, ensure_ascii=False, separators=(",", ":")) + "\n"
            )
            counts["Product"] += 1

        for edge in edges:
            source_is_entity = edge["source"] in entity_ids
            target_is_entity = edge["target"] in entity_ids
            source_is_product = edge["source"] in products
            target_is_product = edge["target"] in products
            if not (source_is_entity or source_is_product) or not (
                target_is_entity or target_is_product
            ):
                raise RuntimeError(
                    f"edge references an unknown node: {edge['source']} -> {edge['target']}"
                )

            relation_name = (
                "rel_"
                + ("e" if source_is_entity else "p")
                + ("e" if target_is_entity else "p")
            )
            record = {
                "source": (
                    edge["source"]
                    if source_is_entity
                    else products[edge["source"]]["name"]
                ),
                "target": (
                    edge["target"]
                    if target_is_entity
                    else products[edge["target"]]["name"]
                ),
                "rel_type": edge["relation_type"],
                "content": edge.get("content", ""),
            }
            streams[relation_name].write(
                json.dumps(record, ensure_ascii=False, separators=(",", ":")) + "\n"
            )
            counts[relation_name] += 1
    finally:
        for stream in streams.values():
            stream.close()

    for name, output_path in output_paths.items():
        print(f"Wrote {counts[name]} {name} records to {output_path}")


def main():
    args = parse_args()
    query_output = args.query_output or Path(f"{args.output}.query.json")
    split_non_entity_records(args.input, args.output.parent)
    if args.split_only:
        return
    entities = load_entities(args.input)
    if not entities:
        raise RuntimeError(f"no Entity records found in {args.input}")

    model = SentenceTransformer(args.model)
    texts = [f"{entity['name']} {entity['description']}" for entity in entities]
    embeddings = model.encode(
        texts,
        batch_size=args.batch_size,
        normalize_embeddings=True,
        show_progress_bar=True,
    )
    query_embedding = model.encode(
        [args.query], normalize_embeddings=True, show_progress_bar=False
    )[0]

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", encoding="utf-8") as stream:
        for entity, embedding in zip(entities, embeddings):
            record = {
                "uid": entity["id"],
                "name": entity["name"],
                "description": entity["description"],
                "entity_type": entity.get("entity_type", ""),
                "product": entity.get("product", ""),
                "authority": int(entity.get("authority", 0)),
                "kg_id": entity.get("kg_id", ""),
                "embedding": embedding.tolist(),
            }
            stream.write(json.dumps(record, ensure_ascii=False, separators=(",", ":")))
            stream.write("\n")

    query_output.parent.mkdir(parents=True, exist_ok=True)
    query_output.write_text(
        json.dumps(
            {
                "query": args.query,
                "model": args.model,
                "embedding": query_embedding.tolist(),
            },
            ensure_ascii=False,
            separators=(",", ":"),
        ),
        encoding="utf-8",
    )
    print(f"Wrote {len(entities)} entities to {args.output}")
    print(f"Wrote query vector to {query_output}")


if __name__ == "__main__":
    main()

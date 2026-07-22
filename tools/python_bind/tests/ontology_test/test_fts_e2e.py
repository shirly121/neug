#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""Full-text-search examples for the ontology entity data."""

import logging
import os
import re
import shutil
import sys
import time
from pathlib import Path

import pytest

os.environ["GLOG_minloglevel"] = "3"
logging.disable(logging.CRITICAL)
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "../.."))

from neug.database import Database  # noqa: E402

ENTITY_DATA_PATH = Path("../../workspace/ontology_data/entity.jsonl")
PRODUCT_DATA_PATH = Path("../../workspace/ontology_data/product.jsonl")
REL_EP_DATA_PATH = Path("../../workspace/ontology_data/rel_ep.jsonl")
DATABASE_PATH = Path("../../workspace/fts_e2e_db")
TOPK = 20


# Create the ontology Entity table, import its text fields, and build the FTS index.
@pytest.fixture(scope="module")
def ontology_fts():
    shutil.rmtree(DATABASE_PATH, ignore_errors=True)
    DATABASE_PATH.mkdir(parents=True)

    database = Database(
        db_path=str(DATABASE_PATH),
        mode="w",
        checkpoint_on_close=False,
    )
    connection = database.connect()
    connection.execute(
        "CREATE NODE TABLE Entity("
        "uid STRING PRIMARY KEY, "
        "name STRING, "
        "description STRING, "
        "entity_type STRING, "
        "product STRING, "
        "authority INT64, "
        "kg_id STRING"
        ");"
    )
    connection.execute(
        "CREATE NODE TABLE Product("
        "name STRING PRIMARY KEY, uid STRING, description STRING"
        ");"
    )
    connection.execute(
        "CREATE REL TABLE rel_ep("
        "FROM Entity TO Product, rel_type STRING, content STRING"
        ");"
    )

    import_started_at = time.perf_counter()
    connection.execute(
        f'COPY Entity FROM (LOAD FROM "{ENTITY_DATA_PATH.resolve()}" '
        "RETURN uid, name, description, entity_type, product, authority, kg_id);"
    )
    import_elapsed_seconds = time.perf_counter() - import_started_at
    print(f"Imported entity data in {import_elapsed_seconds:.3f} seconds")

    graph_import_started_at = time.perf_counter()
    connection.execute(
        f'COPY Product FROM (LOAD FROM "{PRODUCT_DATA_PATH.resolve()}" '
        "RETURN name, uid, description);"
    )
    connection.execute(
        f'COPY rel_ep FROM (LOAD FROM "{REL_EP_DATA_PATH.resolve()}" '
        "RETURN source, target, rel_type, content) "
        '(from="Entity", to="Product");'
    )
    graph_import_elapsed_seconds = time.perf_counter() - graph_import_started_at
    print(
        f"Imported Product and rel_ep data in "
        f"{graph_import_elapsed_seconds:.3f} seconds"
    )

    connection.execute("LOAD hybrid_search;")
    index_started_at = time.perf_counter()
    connection.execute(
        "CREATE INDEX entity_description_fts "
        "ON Entity USING SQLITE_FTS (description);"
    )
    index_elapsed_seconds = time.perf_counter() - index_started_at
    print(f"Created entity_description_fts in {index_elapsed_seconds:.3f} seconds")

    yield connection

    connection.close()
    database.close()


# Test a single-column FTS query on Entity.description and display the Top 20 rows.
def test_single_column_fts(ontology_fts):
    query_started_at = time.perf_counter()
    rows = list(
        ontology_fts.execute(
            "MATCH (n:Entity) "
            "RETURN n.uid, n.name, n.description, n.entity_type, "
            "n.product, n.authority, "
            "bm25(n.description, 'MaxCompute') AS score "
            f"ORDER BY score ASC LIMIT {TOPK};"
        )
    )
    query_elapsed_seconds = time.perf_counter() - query_started_at

    print(
        f"MaxCompute Top {TOPK} query completed in {query_elapsed_seconds:.3f} seconds"
    )
    for row in rows:
        print(row)

    assert len(rows) == TOPK
    assert [row[6] for row in rows] == sorted(row[6] for row in rows)


# Test an ordered FTS5 phrase query on Entity.description.
def test_phrase_fts(ontology_fts):
    rows = list(
        ontology_fts.execute(
            "MATCH (n:Entity) "
            "RETURN n.uid, n.name, n.description, "
            "bm25(n.description, '\"Flink Connector\"') AS score "
            f"ORDER BY score ASC LIMIT {TOPK};"
        )
    )

    for row in rows:
        print(row)

    assert rows
    assert len(rows) <= TOPK
    assert [row[3] for row in rows] == sorted(row[3] for row in rows)
    assert all("flink connector" in row[2].lower() for row in rows)


# Test an FTS5 boolean expression requiring both Flink and MaxCompute.
def test_boolean_expression_fts(ontology_fts):
    rows = list(
        ontology_fts.execute(
            "MATCH (n:Entity) "
            "RETURN n.uid, n.name, n.description, "
            "bm25(n.description, 'Flink AND MaxCompute') AS score "
            f"ORDER BY score ASC LIMIT {TOPK};"
        )
    )

    for row in rows:
        print(row)

    assert rows
    assert len(rows) <= TOPK
    assert [row[3] for row in rows] == sorted(row[3] for row in rows)
    assert all(
        "flink" in row[2].lower() and "maxcompute" in row[2].lower() for row in rows
    )


# Test an FTS5 prefix query matching description tokens that start with vect.
def test_prefix_fts(ontology_fts):
    rows = list(
        ontology_fts.execute(
            "MATCH (n:Entity) "
            "RETURN n.uid, n.name, n.description, "
            "bm25(n.description, 'vect*') AS score "
            f"ORDER BY score ASC LIMIT {TOPK};"
        )
    )

    for row in rows:
        print(row)

    assert rows
    assert len(rows) <= TOPK
    assert [row[3] for row in rows] == sorted(row[3] for row in rows)
    assert all(
        any(
            token.startswith("vect") for token in re.findall(r"[^\W_]+", row[2].lower())
        )
        for row in rows
    )


# Test graph traversal after retrieving the Top 20 description FTS matches.
def test_fts_post_filtering_graph_query(ontology_fts):
    rows = list(
        ontology_fts.execute(
            "MATCH (n:Entity) "
            "WITH n, "
            "bm25(n.description, 'MaxCompute') AS score "
            f"ORDER BY score ASC LIMIT {TOPK} "
            "MATCH (n)-[r:rel_ep]->(p:Product) "
            "RETURN n.uid, n.name, score, p.name AS related_product, "
            "r.rel_type, r.content "
            "ORDER BY score ASC;"
        )
    )

    for row in rows:
        print(row)

    assert rows
    assert len({row[0] for row in rows}) <= TOPK
    assert [row[2] for row in rows] == sorted(row[2] for row in rows)
    assert all(row[3] and row[4] for row in rows)


# Test scalar pre-filtering before the description FTS Top 20 search.
def test_fts_pre_filtering(ontology_fts):
    rows = list(
        ontology_fts.execute(
            "MATCH (n:Entity) "
            "WHERE n.product = 'Flink' AND n.authority >= 2 "
            "RETURN n.uid, n.name, n.description, n.authority, "
            "bm25(n.description, 'MaxCompute') AS score "
            f"ORDER BY score ASC LIMIT {TOPK};"
        )
    )

    for row in rows:
        print(row)

    eligible_uids = {
        row[0]
        for row in ontology_fts.execute(
            "MATCH (n:Entity) "
            "WHERE n.product = 'Flink' AND n.authority >= 2 "
            "RETURN n.uid;"
        )
    }
    assert rows
    assert len(rows) <= TOPK
    assert all(row[0] in eligible_uids for row in rows)
    assert [row[4] for row in rows] == sorted(row[4] for row in rows)
    assert all(row[3] >= 2 for row in rows)
    assert all("maxcompute" in row[2].lower() for row in rows)


# Test graph-based pre-filtering before the description FTS Top 20 search.
def test_graph_based_fts_pre_filtering(ontology_fts):
    rows = list(
        ontology_fts.execute(
            "MATCH (n:Entity)-[r:rel_ep]->(p:Product {name: 'MaxCompute'}) "
            "RETURN n.uid, n.name, n.description, r.rel_type, r.content, "
            "bm25(n.description, 'Flink') AS score "
            f"ORDER BY score ASC LIMIT {TOPK};"
        )
    )

    for row in rows:
        print(row)

    eligible_uids = {
        row[0]
        for row in ontology_fts.execute(
            "MATCH (n:Entity)-[:rel_ep]->(p:Product {name: 'MaxCompute'}) "
            "RETURN n.uid;"
        )
    }
    assert rows
    assert len(rows) <= TOPK
    assert all(row[0] in eligible_uids for row in rows)
    assert [row[5] for row in rows] == sorted(row[5] for row in rows)
    assert all(row[3] for row in rows)
    assert all("flink" in row[2].lower() for row in rows)

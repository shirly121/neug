#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import math
import os

import pytest

from neug import Database

EXTENSION_TESTS_ENABLED = os.environ.get("NEUG_RUN_EXTENSION_TESTS", "").lower() in (
    "1",
    "true",
    "yes",
    "on",
)
extension_test = pytest.mark.skipif(
    not EXTENSION_TESTS_ENABLED,
    reason=(
        "Extension tests disabled by default; set NEUG_RUN_EXTENSION_TESTS=1 "
        "to enable."
    ),
)
pytestmark = extension_test

DIMENSION = 16
NUM_VECTORS = 1000


def _array_literal(values):
    literals = []
    for value in values:
        literal = format(float(value), ".9g")
        if "." not in literal and "e" not in literal.lower():
            literal += ".0"
        literals.append(literal)
    return "[" + ",".join(literals) + "]"


def _constant_vector(value):
    return [float(value)] * DIMENSION


def _cosine_vector(index):
    angle = 2.0 * math.pi * index / NUM_VECTORS
    return [math.cos(angle), math.sin(angle)] + [0.0] * (DIMENSION - 2)


def _open_database(path, checkpoint_on_close=True):
    db = Database(db_path=str(path), mode="w", checkpoint_on_close=checkpoint_on_close)
    conn = db.connect()
    conn.execute("LOAD zvec;")
    return db, conn


def _close_database(db, conn):
    conn.close()
    db.close()


def _l2_search(conn, query_value, topk=10, predicate=""):
    where = f"WHERE {predicate} " if predicate else ""
    return list(
        conn.execute(
            f"MATCH (n:Item) {where}"
            "RETURN n.id, "
            f"vector_distance_l2(n.l2_vec, {_array_literal(_constant_vector(query_value))}) "
            "AS score ORDER BY score ASC "
            f"LIMIT {topk};"
        )
    )


def _create_advanced_data(conn):
    conn.execute(
        "CREATE NODE TABLE Item("
        "id INT64 PRIMARY KEY, group_id INT64, "
        f"l2_vec FLOAT[{DIMENSION}], "
        f"cosine_vec FLOAT[{DIMENSION}], "
        f"ip_vec FLOAT[{DIMENSION}]);"
    )
    conn.execute("CREATE REL TABLE NEXT(FROM Item TO Item);")
    for start in range(0, NUM_VECTORS, 100):
        nodes = []
        for index in range(start, min(start + 100, NUM_VECTORS)):
            nodes.append(
                "(:Item {"
                f"id: {index}, group_id: {index % 2}, "
                f"l2_vec: {_array_literal(_constant_vector(index))}, "
                f"cosine_vec: {_array_literal(_cosine_vector(index))}, "
                f"ip_vec: {_array_literal(_constant_vector(index))}"
                "})"
            )
        conn.execute("CREATE " + ",".join(nodes) + ";")

    for start in range(0, NUM_VECTORS - 1, 100):
        matches = []
        edges = []
        for index in range(start, min(start + 100, NUM_VECTORS - 1)):
            suffix = index - start
            matches.append(
                f"(source{suffix}:Item {{id: {index}}}),"
                f"(target{suffix}:Item {{id: {index + 1}}})"
            )
            edges.append(f"(source{suffix})-[:NEXT]->(target{suffix})")
        conn.execute("MATCH " + ",".join(matches) + " CREATE " + ",".join(edges) + ";")

    conn.execute(
        "CREATE INDEX item_l2_hnsw ON Item USING HNSW (l2_vec) "
        "WITH (metric = 'l2', m = 16, ef_construction = 200);"
    )
    conn.execute(
        "CREATE INDEX item_cosine_hnsw ON Item USING HNSW (cosine_vec) "
        "WITH (metric = 'cosine', m = 16, ef_construction = 200);"
    )
    conn.execute(
        "CREATE INDEX item_ip_hnsw ON Item USING HNSW (ip_vec) "
        "WITH (metric = 'ip', m = 16, ef_construction = 200);"
    )


@pytest.fixture(scope="module")
def advanced_database(tmp_path_factory):
    db_path = tmp_path_factory.mktemp("hnsw-advanced") / "database"
    db, conn = _open_database(db_path)
    try:
        _create_advanced_data(conn)
    finally:
        _close_database(db, conn)
    return db_path


@pytest.fixture(scope="module")
def advanced_connection(advanced_database):
    db, conn = _open_database(advanced_database, checkpoint_on_close=False)
    try:
        yield conn
    finally:
        _close_database(db, conn)


def test_l2_index_scan(advanced_connection):
    rows = _l2_search(advanced_connection, 3.1)
    assert [row[0] for row in rows[:3]] == [3, 4, 2]
    assert [row[1] for row in rows[:3]] == pytest.approx(
        [math.sqrt(0.16), math.sqrt(12.96), math.sqrt(19.36)], abs=1e-6
    )

    assert _l2_search(advanced_connection, 0.0)[0][0] == 0
    assert [row[0] for row in _l2_search(advanced_connection, 500.0)[:3]] == [
        500,
        501,
        499,
    ]
    assert _l2_search(advanced_connection, 999.0)[0][0] == 999

    filtered = _l2_search(advanced_connection, 3.1, topk=2, predicate="n.group_id = 0")
    assert [row[0] for row in filtered] == [4, 2]


def test_cosine_index_scan(advanced_connection):
    target_id = 181
    target = _array_literal(_cosine_vector(target_id))
    rows = list(
        advanced_connection.execute(
            "MATCH (n:Item) RETURN n.id, "
            f"vector_distance_cosine(n.cosine_vec, {target}) AS score "
            "ORDER BY score ASC LIMIT 3;"
        )
    )
    assert rows[0][0] == target_id
    assert rows[0][1] == pytest.approx(0.0, abs=1e-6)
    assert {rows[1][0], rows[2][0]} == {target_id - 1, target_id + 1}


def test_inner_product_index_scan(advanced_connection):
    target = _array_literal([1.0] * DIMENSION)
    rows = list(
        advanced_connection.execute(
            "MATCH (n:Item) RETURN n.id, "
            f"vector_distance_ip(n.ip_vec, {target}) AS score "
            "ORDER BY score DESC LIMIT 3;"
        )
    )
    assert [row[0] for row in rows] == [999, 998, 997]
    assert [row[1] for row in rows] == pytest.approx(
        [999 * DIMENSION, 998 * DIMENSION, 997 * DIMENSION]
    )


def test_graph_query_then_l2_index_scan(advanced_connection):
    rows = list(
        advanced_connection.execute(
            "MATCH (source:Item)-[:NEXT]->(n:Item) "
            "RETURN n.id, "
            f"vector_distance_l2(n.l2_vec, {_array_literal(_constant_vector(500.1))}) "
            "AS score ORDER BY score ASC LIMIT 3;"
        )
    )
    assert rows[0][0] == 500
    assert {row[0] for row in rows[1:3]} == {499, 501}


def test_update_data_then_index_scan(advanced_connection):
    advanced_connection.execute(
        "MATCH (n:Item {id: 3}) "
        f"SET n.l2_vec = {_array_literal(_constant_vector(700.25))};"
    )
    rows = _l2_search(advanced_connection, 700.25, topk=1)
    assert rows[0][0] == 3
    assert rows[0][1] == pytest.approx(0.0, abs=1e-6)


def test_delete_vector_then_index_scan(advanced_connection):
    advanced_connection.execute("MATCH (n:Item {id: 333}) DELETE n;")
    rows = _l2_search(advanced_connection, 333.1)
    ids = [row[0] for row in rows]
    assert 333 not in ids
    assert ids[0] == 334


def test_index_persistence_after_reopen(advanced_connection):
    rows = _l2_search(advanced_connection, 500.0)
    assert rows[0][0] == 500
    assert {row[0] for row in rows[1:3]} == {499, 501}


def test_updated_vector_persistence_after_checkpoint_and_close(tmp_path):
    db_path = tmp_path / "database"
    db, conn = _open_database(db_path, checkpoint_on_close=False)
    _create_advanced_data(conn)
    conn.execute(
        "MATCH (n:Item {id: 3}) "
        f"SET n.l2_vec = {_array_literal(_constant_vector(700.25))};"
    )
    conn.execute("CHECKPOINT;")
    _close_database(db, conn)

    reopened_db, reopened_conn = _open_database(db_path, checkpoint_on_close=False)
    try:
        rows = _l2_search(reopened_conn, 700.25, topk=1)
        assert rows[0][0] == 3
        assert rows[0][1] == pytest.approx(0.0, abs=1e-6)
    finally:
        _close_database(reopened_db, reopened_conn)

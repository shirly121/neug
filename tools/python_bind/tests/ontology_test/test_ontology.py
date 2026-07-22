#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""Vector-search examples from specs/vec-design/poc.md."""

import json
import os
import re
from pathlib import Path

import numpy as np
import pytest

from neug.database import Database

DEFAULT_DATA_PATH = os.environ.get("NEUG_HNSW_TEST_DATA")
DIMENSION = 512
TOPK = 20


def _array_literal(values):
    return "[" + ",".join(format(float(value), ".9g") for value in values) + "]"


def _string_literal(value):
    return "'" + value.replace("\\", "\\\\").replace("'", "\\'") + "'"


def _data_path():
    if not DEFAULT_DATA_PATH:
        raise RuntimeError(
            "NEUG_HNSW_TEST_DATA must point to the preprocessed ontology directory"
        )
    path = Path(DEFAULT_DATA_PATH)
    if not path.is_dir():
        raise FileNotFoundError(
            f"preprocessed ontology directory is unavailable: {path}"
        )
    return path


def _load_query_vector(data_path):
    query_path = Path(os.environ.get("NEUG_HNSW_QUERY_DATA", f"{data_path}.query.json"))
    if not query_path.is_file():
        raise FileNotFoundError(
            f"preprocessed query vector is unavailable: {query_path}"
        )
    value = json.loads(query_path.read_text(encoding="utf-8"))
    vector = np.asarray(value["embedding"], dtype=np.float32)
    assert vector.shape == (DIMENSION,)
    return vector


def _load_entities(data_path):
    entities = []
    with data_path.open(encoding="utf-8") as stream:
        for line in stream:
            entity = json.loads(line)
            embedding = np.asarray(entity["embedding"], dtype=np.float32)
            assert embedding.shape == (DIMENSION,)
            entity["embedding"] = embedding
            entities.append(entity)
    return entities


def _expected_uids(entities, query_vector, predicate=lambda _: True, topk=TOPK):
    scores = {
        entity["uid"]: float(entity["embedding"] @ query_vector)
        for entity in entities
        if predicate(entity)
    }
    return set(sorted(scores, key=scores.get, reverse=True)[:topk])


def _assert_recall(rows, expected_uids, uid_column=0):
    assert rows
    actual_uids = {row[uid_column] for row in rows}
    denominator = min(TOPK, len(expected_uids))
    assert denominator > 0
    recall = len(actual_uids & expected_uids) / denominator
    assert recall > 0.8, f"expected recall > 0.8, got {recall:.2%}"


def _execute_index_query(conn, query, index_kind):
    function_names = {
        "HNSWIndexScan": ("vector_distance_ip",),
        "FTSIndexScan": ("bm25",),
        "HybridIndexScan": ("bm25", "vector_distance_ip"),
    }[index_kind]
    assert all(function_name in query for function_name in function_names)

    for mode in ("EXPLAIN", "PROFILE"):
        result = conn.execute(f"{mode} {query}")
        rows = list(result)
        assert result.has_profile_result(), f"{mode} did not return a plan"
        operators = [
            operator["operator_name"]
            for operator in result.get_profile_metrics()["operators"]
        ]
        assert operators.count("IndexScanOpr") >= 1, (
            f"{index_kind} was not selected by {mode}: {operators}\n"
            f"{result.get_profile_text()}"
        )
        if mode == "PROFILE":
            return rows

    raise AssertionError("PROFILE query was not executed")


def _assert_vector_scores(rows, ontology_graph, score_column, unique_uids=True):
    assert rows
    uids = [row[0] for row in rows]
    if unique_uids:
        assert len(uids) == len(set(uids))
    for row in rows:
        entity = ontology_graph["entity_by_uid"][row[0]]
        expected_score = float(entity["embedding"] @ ontology_graph["query_vector"])
        assert row[score_column] == pytest.approx(expected_score, abs=1e-5)


def _assert_text_matches(rows, text_column, *terms):
    assert rows
    for row in rows:
        text = row[text_column].casefold()
        assert all(term.casefold() in text for term in terms)


@pytest.fixture(scope="module")
def ontology_graph(tmp_path_factory):
    data_path = _data_path()
    entity_path = data_path / "entity.jsonl"
    if not entity_path.is_file():
        raise FileNotFoundError(
            f"preprocessed ontology data is unavailable: {entity_path}"
        )
    entities = _load_entities(entity_path)
    query_vector = _load_query_vector(entity_path)
    assert len(entities) >= TOPK
    split_paths = {
        "Product": data_path / "product.jsonl",
        "rel_ee": data_path / "rel_ee.jsonl",
        "rel_ep": data_path / "rel_ep.jsonl",
        "rel_pe": data_path / "rel_pe.jsonl",
        "rel_pp": data_path / "rel_pp.jsonl",
    }
    for path in split_paths.values():
        if not path.is_file():
            raise FileNotFoundError(
                f"preprocessed ontology data is unavailable: {path}"
            )
    maxcompute_graph_uids = set()
    rel_ep_rows = set()
    with split_paths["rel_ep"].open(encoding="utf-8") as stream:
        for line in stream:
            relation = json.loads(line)
            rel_ep_rows.add(
                (
                    relation["source"],
                    relation["target"],
                    relation["rel_type"],
                    relation["content"],
                )
            )
            if relation["target"] == "MaxCompute":
                maxcompute_graph_uids.add(relation["source"])

    db_path = tmp_path_factory.mktemp("ontology-hnsw") / "database"
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
    for name, source, target in (
        ("rel_ee", "Entity", "Entity"),
        ("rel_ep", "Entity", "Product"),
        ("rel_pe", "Product", "Entity"),
        ("rel_pp", "Product", "Product"),
    ):
        conn.execute(
            f"CREATE REL TABLE {name}(FROM {source} TO {target}, "
            "rel_type STRING, content STRING);"
        )
    conn.execute(
        f'COPY Entity FROM (LOAD FROM "{entity_path}" '
        "RETURN uid, name, description, entity_type, product, authority, "
        "kg_id, CAST(embedding, 'FLOAT[512]'))"
    )

    conn.execute(
        f'COPY Product FROM (LOAD FROM "{split_paths["Product"]}" '
        "RETURN name, uid, description);"
    )
    relation_labels = {
        "rel_ee": ("Entity", "Entity"),
        "rel_ep": ("Entity", "Product"),
        "rel_pe": ("Product", "Entity"),
        "rel_pp": ("Product", "Product"),
    }
    for name, (source, target) in relation_labels.items():
        path = split_paths[name]
        if path.stat().st_size == 0:
            continue
        conn.execute(
            f'COPY {name} FROM (LOAD FROM "{path}" '
            "RETURN source, target, rel_type, content) "
            f'(from="{source}", to="{target}");'
        )
    conn.execute(
        "CREATE INDEX entity_embedding_hnsw ON Entity USING HNSW (embedding) "
        "WITH (metric = 'ip', m = 16, ef_construction = 200);"
    )
    conn.execute(
        "CREATE INDEX entity_description_fts "
        "ON Entity USING SQLITE_FTS (description);"
    )
    conn.execute(
        "CREATE INDEX entity_text_fts "
        "ON Entity USING SQLITE_FTS (name, description) "
        "WITH (name_weight = 8.0, description_weight = 2.0);"
    )

    yield {
        "connection": conn,
        "database": db,
        "entities": entities,
        "entity_by_uid": {entity["uid"]: entity for entity in entities},
        "query_vector": query_vector,
        "query_literal": _array_literal(query_vector),
        "maxcompute_graph_uids": maxcompute_graph_uids,
        "rel_ep_rows": rel_ep_rows,
    }

    conn.close()
    db.close()


def test_vector_similarity_search(ontology_graph):
    conn = ontology_graph["connection"]
    query = (
        "MATCH (n:Entity) "
        "RETURN n.uid, n.name, n.description, n.entity_type, n.product, "
        "n.authority, vector_distance_ip(n.embedding, "
        f"{ontology_graph['query_literal']}) AS score "
        f"ORDER BY score DESC LIMIT {TOPK};"
    )
    rows = _execute_index_query(conn, query, "HNSWIndexScan")

    assert len(rows) == TOPK
    assert [row[6] for row in rows] == sorted((row[6] for row in rows), reverse=True)
    _assert_vector_scores(rows, ontology_graph, 6)
    expected = _expected_uids(
        ontology_graph["entities"], ontology_graph["query_vector"]
    )
    _assert_recall(rows, expected)


def test_vector_search_then_graph_query(ontology_graph):
    conn = ontology_graph["connection"]
    query = (
        "MATCH (n:Entity) "
        "WITH n, vector_distance_ip(n.embedding, "
        f"{ontology_graph['query_literal']}) AS score "
        f"ORDER BY score DESC LIMIT {TOPK} "
        "MATCH (n)-[r:rel_ep]->(p:Product) "
        "RETURN n.uid, n.name, score, p.name AS related_product, "
        "r.rel_type, r.content ORDER BY score DESC;"
    )
    rows = _execute_index_query(conn, query, "HNSWIndexScan")

    assert rows
    assert [row[2] for row in rows] == sorted((row[2] for row in rows), reverse=True)
    _assert_vector_scores(rows, ontology_graph, 2, unique_uids=False)
    assert all(
        (row[0], row[3], row[4], row[5]) in ontology_graph["rel_ep_rows"]
        for row in rows
    )


def test_scalar_filter_then_vector_search(ontology_graph):
    entities = ontology_graph["entities"]
    matching = [
        entity
        for entity in entities
        if entity["product"] == "MaxCompute" and entity["authority"] >= 2
    ]
    assert matching
    expected = _expected_uids(
        entities,
        ontology_graph["query_vector"],
        lambda entity: entity["product"] == "MaxCompute" and entity["authority"] >= 2,
    )
    query = (
        "MATCH (n:Entity) "
        "WHERE n.product = 'MaxCompute' AND n.authority >= 2 "
        "RETURN n.uid, n.name, n.description, n.authority, "
        "vector_distance_ip(n.embedding, "
        f"{ontology_graph['query_literal']}) AS score "
        f"ORDER BY score DESC LIMIT {TOPK};"
    )
    rows = _execute_index_query(ontology_graph["connection"], query, "HNSWIndexScan")

    assert all(
        ontology_graph["entity_by_uid"][row[0]]["product"] == "MaxCompute"
        and row[3] >= 2
        for row in rows
    )
    _assert_vector_scores(rows, ontology_graph, 4)
    _assert_recall(rows, expected)


def test_graph_query_then_vector_search(ontology_graph):
    entities = ontology_graph["entities"]
    candidate_uids = ontology_graph["maxcompute_graph_uids"]
    assert candidate_uids
    expected = _expected_uids(
        entities,
        ontology_graph["query_vector"],
        lambda entity: entity["uid"] in candidate_uids,
    )
    query = (
        "MATCH (n:Entity)-[r:rel_ep]->(p:Product {name: 'MaxCompute'}) "
        "WITH n "
        "RETURN n.uid, n.name, n.description, "
        "vector_distance_ip(n.embedding, "
        f"{ontology_graph['query_literal']}) AS score "
        f"ORDER BY score DESC LIMIT {TOPK};"
    )
    rows = _execute_index_query(ontology_graph["connection"], query, "HNSWIndexScan")

    assert all(row[0] in candidate_uids for row in rows)
    _assert_vector_scores(rows, ontology_graph, 3)
    _assert_recall(rows, expected)


def test_updated_vector_is_searchable(ontology_graph):
    query_vector = ontology_graph["query_vector"]
    target = min(
        ontology_graph["entities"][:1000],
        key=lambda entity: float(entity["embedding"] @ query_vector),
    )
    target_uid = target["uid"]
    conn = ontology_graph["connection"]
    conn.execute(
        "MATCH (n:Entity) "
        f"WHERE n.uid = {_string_literal(target_uid)} "
        f"SET n.embedding = {ontology_graph['query_literal']};"
    )
    stored_score = list(
        conn.execute(
            "MATCH (n:Entity) "
            f"WHERE n.uid = {_string_literal(target_uid)} "
            "RETURN n.uid, vector_distance_ip(n.embedding, "
            f"{ontology_graph['query_literal']}) AS score;"
        )
    )
    assert len(stored_score) == 1 and stored_score[0][0] == target_uid
    stored_score = stored_score[0][1]
    query = (
        "MATCH (n:Entity) "
        "RETURN n.uid, n.name, vector_distance_ip(n.embedding, "
        f"{ontology_graph['query_literal']}) AS score "
        f"ORDER BY score DESC LIMIT {TOPK};"
    )
    rows = _execute_index_query(conn, query, "HNSWIndexScan")

    assert rows[0][0] == target_uid, (
        f"updated vector score={stored_score}, returned top-1={rows[0][0]} "
        f"with score={rows[0][2]}"
    )
    assert rows[0][2] == pytest.approx(stored_score, abs=1e-5)
    target["embedding"] = query_vector.copy()


def _assert_fts_result(rows, score_column, unique_uids=True):
    assert rows
    assert len(rows) <= TOPK
    if unique_uids:
        uids = [row[0] for row in rows]
        assert len(uids) == len(set(uids))
    scores = [row[score_column] for row in rows]
    assert all(np.isfinite(score) and score <= 0.0 for score in scores)
    assert scores == sorted(scores)


def _assert_fts_query_matches(rows, query_string, text_column):
    for row in rows:
        text = row[text_column].casefold()
        words = re.findall(r"\w+", text)
        if query_string == '"Flink Connector"':
            assert "flink connector" in text
        elif query_string == "Flink AND MaxCompute":
            assert "flink" in words and "maxcompute" in words
        elif query_string == "(Flink OR Connector) AND MaxCompute":
            assert ("flink" in words or "connector" in words) and "maxcompute" in words
        elif query_string == "vect*":
            assert "vect" in text
        elif query_string == "Flink* AND MaxCompute":
            assert "flink" in text and "maxcompute" in text
        else:
            raise AssertionError(f"missing validation for FTS query: {query_string}")


def test_single_column_full_text_search(ontology_graph):
    query = (
        "MATCH (n:Entity) "
        "RETURN n.uid, n.name, n.description, n.entity_type, n.product, "
        "n.authority, "
        "bm25(n.description, 'MaxCompute') AS score "
        f"ORDER BY score ASC LIMIT {TOPK};"
    )
    rows = _execute_index_query(ontology_graph["connection"], query, "FTSIndexScan")

    _assert_fts_result(rows, 6)
    _assert_text_matches(rows, 2, "MaxCompute")


def test_multi_column_full_text_search(ontology_graph):
    query = (
        "MATCH (n:Entity) "
        "RETURN n.uid, n.name, n.description, n.entity_type, n.product, "
        "n.authority, "
        "bm25([n.name, n.description], 'MaxCompute') AS score "
        f"ORDER BY score ASC LIMIT {TOPK};"
    )
    rows = _execute_index_query(ontology_graph["connection"], query, "FTSIndexScan")

    _assert_fts_result(rows, 6)
    assert all("maxcompute" in f"{row[1]} {row[2]}".casefold() for row in rows)


@pytest.mark.parametrize(
    "query_string",
    [
        '"Flink Connector"',
        "Flink AND MaxCompute",
        "(Flink OR Connector) AND MaxCompute",
        "vect*",
        "Flink* AND MaxCompute",
    ],
)
def test_full_text_query_syntax(ontology_graph, query_string):
    query = (
        "MATCH (n:Entity) "
        "RETURN n.uid, n.name, n.description, "
        "bm25(n.description, "
        f"{_string_literal(query_string)}) AS score "
        f"ORDER BY score ASC LIMIT {TOPK};"
    )
    rows = _execute_index_query(ontology_graph["connection"], query, "FTSIndexScan")

    _assert_fts_result(rows, 3)
    _assert_fts_query_matches(rows, query_string, 2)


def test_full_text_search_then_graph_query(ontology_graph):
    query = (
        "MATCH (n:Entity) "
        "WITH n, bm25(n.description, "
        "'MaxCompute') AS score "
        f"ORDER BY score ASC LIMIT {TOPK} "
        "MATCH (n)-[r:rel_ep]->(p:Product) "
        "RETURN n.uid, n.name, score, p.name AS related_product, "
        "r.rel_type, r.content ORDER BY score ASC;"
    )
    rows = _execute_index_query(ontology_graph["connection"], query, "FTSIndexScan")

    assert rows
    assert len({row[0] for row in rows}) <= TOPK
    scores = [row[2] for row in rows]
    assert scores == sorted(scores)
    assert all(
        "maxcompute"
        in ontology_graph["entity_by_uid"][row[0]]["description"].casefold()
        and (row[0], row[3], row[4], row[5]) in ontology_graph["rel_ep_rows"]
        for row in rows
    )


def test_scalar_filter_then_full_text_search(ontology_graph):
    query = (
        "MATCH (n:Entity) "
        "WHERE n.product = 'Flink' AND n.authority >= 2 "
        "RETURN n.uid, n.name, n.description, n.authority, "
        "bm25(n.description, 'MaxCompute') AS score "
        f"ORDER BY score ASC LIMIT {TOPK};"
    )
    rows = _execute_index_query(ontology_graph["connection"], query, "FTSIndexScan")

    _assert_fts_result(rows, 4)
    _assert_text_matches(rows, 2, "MaxCompute")
    assert all(
        row[3] >= 2 and ontology_graph["entity_by_uid"][row[0]]["product"] == "Flink"
        for row in rows
    )


def test_graph_query_then_full_text_search(ontology_graph):
    query = (
        "MATCH (n:Entity)-[r:rel_ep]->(p:Product {name: 'MaxCompute'}) "
        "RETURN n.uid, n.name, n.description, r.rel_type, r.content, "
        "bm25(n.description, 'Flink') AS score "
        f"ORDER BY score ASC LIMIT {TOPK};"
    )
    rows = _execute_index_query(ontology_graph["connection"], query, "FTSIndexScan")

    _assert_fts_result(rows, 5)
    _assert_text_matches(rows, 2, "Flink")
    assert all(
        (row[0], "MaxCompute", row[3], row[4]) in ontology_graph["rel_ep_rows"]
        for row in rows
    )


def test_full_text_and_vector_search(ontology_graph):
    query = (
        "MATCH (n:Entity) "
        "WITH n, bm25(n.description, "
        "'MaxCompute') AS text_score "
        "ORDER BY text_score ASC LIMIT 200 "
        "RETURN n.uid, n.name, n.description, text_score, "
        "vector_distance_ip(n.embedding, "
        f"{ontology_graph['query_literal']}) AS vector_score "
        f"ORDER BY vector_score DESC LIMIT {TOPK};"
    )
    rows = _execute_index_query(ontology_graph["connection"], query, "HybridIndexScan")

    assert rows
    assert len(rows) <= TOPK
    assert all(np.isfinite(row[3]) and row[3] <= 0.0 for row in rows)
    _assert_text_matches(rows, 2, "MaxCompute")
    _assert_vector_scores(rows, ontology_graph, 4)
    vector_scores = [row[4] for row in rows]
    assert vector_scores == sorted(vector_scores, reverse=True)

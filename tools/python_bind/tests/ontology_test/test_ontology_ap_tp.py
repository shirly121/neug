#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""AP-to-TP ontology vector-index roundtrip test."""

import json

import pytest

from neug import Session
from neug.database import Database

from . import test_ontology as ontology


def test_ap_tp_vector_index_roundtrip(tmp_path, unused_tcp_port):
    """Build the ontology in AP mode, then query and update it in TP mode."""
    data_path = ontology._data_path()
    entity_path = data_path / "entity.jsonl"
    product_path = data_path / "product.jsonl"
    rel_ep_path = data_path / "rel_ep.jsonl"
    entities = ontology._load_entities(entity_path)
    query_vector = ontology._load_query_vector(entity_path)
    query_literal = ontology._array_literal(query_vector)
    entity_by_uid = {entity["uid"]: entity for entity in entities}

    rel_ep_rows = set()
    maxcompute_graph_uids = set()
    with rel_ep_path.open(encoding="utf-8") as stream:
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

    db = Database(db_path=str(tmp_path / "ap-tp-database"), mode="w")
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

    # FTS indexes and hybrid FTS/vector queries are intentionally excluded
    # until FTSIndex supports the AP-to-TP lifecycle.
    conn.close()
    uri = db.serve(
        port=unused_tcp_port,
        host="localhost",
        blocking=False,
        thread_num=4,
    )
    query_session = Session(uri, timeout="30s")
    write_session = Session(uri, timeout="30s")
    graph = {
        "entities": entities,
        "entity_by_uid": entity_by_uid,
        "query_vector": query_vector,
    }
    try:
        # Plain vector similarity search.
        rows = list(
            query_session.execute(
                "MATCH (n:Entity) "
                "RETURN n.uid, n.name, n.description, n.entity_type, "
                "n.product, n.authority, "
                f"vector_distance_ip(n.embedding, {query_literal}) AS score "
                f"ORDER BY score DESC LIMIT {ontology.TOPK};",
                access_mode="read",
            )
        )
        assert len(rows) == ontology.TOPK
        assert [row[6] for row in rows] == sorted(
            (row[6] for row in rows), reverse=True
        )
        ontology._assert_vector_scores(rows, graph, 6)
        ontology._assert_recall(rows, ontology._expected_uids(entities, query_vector))

        # Vector search followed by graph expansion.
        rows = list(
            query_session.execute(
                "MATCH (n:Entity) "
                f"WITH n, vector_distance_ip(n.embedding, {query_literal}) "
                f"AS score ORDER BY score DESC LIMIT {ontology.TOPK} "
                "MATCH (n)-[r:rel_ep]->(p:Product) "
                "RETURN n.uid, n.name, score, p.name AS related_product, "
                "r.rel_type, r.content ORDER BY score DESC;",
                access_mode="read",
            )
        )
        assert rows
        assert [row[2] for row in rows] == sorted(
            (row[2] for row in rows), reverse=True
        )
        ontology._assert_vector_scores(rows, graph, 2, unique_uids=False)
        assert all((row[0], row[3], row[4], row[5]) in rel_ep_rows for row in rows)

        # Scalar filtering followed by vector search.
        expected = ontology._expected_uids(
            entities,
            query_vector,
            lambda entity: entity["product"] == "MaxCompute"
            and entity["authority"] >= 2,
        )
        rows = list(
            query_session.execute(
                "MATCH (n:Entity) "
                "WHERE n.product = 'MaxCompute' AND n.authority >= 2 "
                "RETURN n.uid, n.name, n.description, n.authority, "
                f"vector_distance_ip(n.embedding, {query_literal}) AS score "
                f"ORDER BY score DESC LIMIT {ontology.TOPK};",
                access_mode="read",
            )
        )
        assert all(
            entity_by_uid[row[0]]["product"] == "MaxCompute" and row[3] >= 2
            for row in rows
        )
        ontology._assert_vector_scores(rows, graph, 4)
        ontology._assert_recall(rows, expected)

        # Graph filtering followed by vector search.
        expected = ontology._expected_uids(
            entities,
            query_vector,
            lambda entity: entity["uid"] in maxcompute_graph_uids,
        )
        rows = list(
            query_session.execute(
                "MATCH (n:Entity)-[r:rel_ep]->"
                "(p:Product {name: 'MaxCompute'}) "
                "WITH n RETURN n.uid, n.name, n.description, "
                f"vector_distance_ip(n.embedding, {query_literal}) AS score "
                f"ORDER BY score DESC LIMIT {ontology.TOPK};",
                access_mode="read",
            )
        )
        assert all(row[0] in maxcompute_graph_uids for row in rows)
        ontology._assert_vector_scores(rows, graph, 3)
        ontology._assert_recall(rows, expected)

        # Update through the write session, then query the vector index through
        # that same session to verify the new index entry is visible.
        target = min(
            entities[:1000],
            key=lambda entity: float(entity["embedding"] @ query_vector),
        )
        target_uid = target["uid"]
        update_rows = list(
            write_session.execute(
                "MATCH (n:Entity) "
                f"WHERE n.uid = {ontology._string_literal(target_uid)} "
                f"SET n.embedding = {query_literal} RETURN n.uid;",
                access_mode="update",
            )
        )
        assert len(update_rows) == 1 and update_rows[0][0] == target_uid

        rows = list(
            write_session.execute(
                "MATCH (n:Entity) "
                "RETURN n.uid, n.name, "
                f"vector_distance_ip(n.embedding, {query_literal}) AS score "
                f"ORDER BY score DESC LIMIT {ontology.TOPK};",
                access_mode="read",
            )
        )
        assert rows[0][0] == target_uid
        assert rows[0][2] == pytest.approx(float(query_vector @ query_vector), abs=1e-5)
    finally:
        query_session.close()
        write_session.close()
        db.close()

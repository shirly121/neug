#!/usr/bin/env python3
# -*- coding: utf-8 -*-
#
# Copyright 2020 Alibaba Group Holding Limited. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License on an "AS IS" BASIS, WITHOUT WARRANTIES OR
# CONDITIONS OF ANY KIND, either express or implied. See the License for
# the specific language governing permissions and limitations under the
# License.
#
# Tests for GDS / project_graph (see specs/004-gds/compiler-spec.md).
#
# The Cypher parser currently does not lower `{...}` struct literals (see
# transformStructLiteral); use LIST literals instead, which
# extractGraphEntryTableInfos accepts: node side `['table', ...]` (empty
# predicate per string entry), rel side `[['src','rel_group','dst'], ...]`.

import os
import shutil
import sys

import pytest

sys.path.append(os.path.join(os.path.dirname(__file__), "../"))

from neug.database import Database


def _schema_person_knows():
    """Minimal catalog matching compiler-spec triplet [SRC, REL, DST]."""
    return [
        "CREATE NODE TABLE person("
        "id INT64, name STRING, age INT64, PRIMARY KEY(id));",
        "CREATE REL TABLE knows(FROM person TO person, weight DOUBLE);",
    ]


def test_project_graph_and_drop_roundtrip():
    """Register a projected graph alias, then drop it (happy path)."""
    db_dir = "/tmp/test_gds_project_drop"
    shutil.rmtree(db_dir, ignore_errors=True)

    print("start to create database")

    db = Database(db_path=db_dir, mode="w")
    print("database has been created")
    conn = db.connect()
    for stmt in _schema_person_knows():
        conn.execute(stmt)

    print("schema has been created")

    conn.execute(
        "CALL project_graph("
        "'my_subgraph', "
        "['person'], "
        "[['person', 'knows', 'person']]"
        ");"
    )

    print("project_graph has been called")
    conn.execute("CALL drop_projected_graph('my_subgraph');")
    print("drop_projected_graph has been called")

    conn.close()
    db.close()

def test_project_graph_with_predicates():
    """Project a graph with predicates."""
    db_dir = "/tmp/test_gds_project_with_predicates"
    shutil.rmtree(db_dir, ignore_errors=True)

    print("start to create database")

    db = Database(db_path=db_dir, mode="w")
    print("database has been created")
    conn = db.connect()
    for stmt in _schema_person_knows():
        conn.execute(stmt)

    print("schema has been created")

    conn.execute(
        "CALL project_graph("
        "'my_subgraph', "
        "{'person': 'n.age > 20'}, "
        "{'[person, knows, person]': 'r.weight > 1.0'}"
        ");"
    )

    print("project_graph has been called")
    conn.execute("CALL drop_projected_graph('my_subgraph');")
    print("drop_projected_graph has been called")

    conn.close()
    db.close()

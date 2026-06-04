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
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

"""Tests for HNSW index via ZVec extension."""

import os
import shutil
import sys

import pytest

sys.path.append(os.path.join(os.path.dirname(__file__), "../"))

from neug import Database


@pytest.fixture
def db():
    """Create an in-memory database for testing."""
    database = Database(":memory:")
    yield database
    database.close()


@pytest.fixture
def conn(db):
    """Create a connection from the database."""
    connection = db.connect()
    yield connection
    connection.close()


class TestLoadZvec:
    """Test loading the zvec extension."""

    def test_load_zvec_extension(self, conn):
        """LOAD zvec should succeed and register HNSW index type."""
        result = conn.execute("LOAD zvec")
        assert result is not None

    def test_load_zvec_idempotent(self, conn):
        """Loading zvec twice should not raise an error."""
        conn.execute("LOAD zvec")
        # Second load should either succeed silently or raise a known error
        try:
            conn.execute("LOAD zvec")
        except RuntimeError as e:
            # Some extension systems reject double-load; that's acceptable
            assert "already loaded" in str(e).lower() or "duplicate" in str(e).lower()


class TestCreateIndex:
    """End-to-end tests for CREATE INDEX DDL statement.

    These tests verify that CREATE INDEX goes through the full pipeline:
    Cypher → Parser → Binder → Planner → Physical Plan → Engine execution.

    Note: The engine's CreateIndexOpr::Eval() is currently a placeholder that
    returns the context unchanged. These tests verify the pipeline runs without
    error, not that an index is actually created in storage.
    """

    def test_create_index_empty_graph(self, tmp_path):
        """CREATE INDEX on an empty graph (no data) should succeed."""
        db_dir = str(tmp_path / "test_create_index_empty")
        shutil.rmtree(db_dir, ignore_errors=True)

        db = Database(db_dir, "w")
        conn = db.connect()
        conn.execute(
            "CREATE NODE TABLE User(id INT64 PRIMARY KEY, name STRING, age INT64);"
        )
        conn.execute("CREATE INDEX idx_user_age ON User USING HNSW (age);")
        conn.close()
        db.close()

    def test_create_index_with_options(self, tmp_path):
        """CREATE INDEX with WITH clause options should succeed."""
        db_dir = str(tmp_path / "test_create_index_options")
        shutil.rmtree(db_dir, ignore_errors=True)

        db = Database(db_dir, "w")
        conn = db.connect()
        conn.execute(
            "CREATE NODE TABLE User(id INT64 PRIMARY KEY, name STRING, age INT64);"
        )
        conn.execute(
            "CREATE INDEX idx_user_age ON User USING HNSW (age) "
            "WITH (metric = 'cosine');"
        )
        conn.close()
        db.close()

    def test_create_index_if_not_exists(self, tmp_path):
        """CREATE INDEX IF NOT EXISTS should succeed without error."""
        db_dir = str(tmp_path / "test_create_index_if_not_exists")
        shutil.rmtree(db_dir, ignore_errors=True)

        db = Database(db_dir, "w")
        conn = db.connect()
        conn.execute(
            "CREATE NODE TABLE User(id INT64 PRIMARY KEY, name STRING, age INT64);"
        )
        conn.execute(
            "CREATE INDEX IF NOT EXISTS idx_user_age ON User USING HNSW (age);"
        )
        conn.close()
        db.close()

    def test_create_index_after_insert(self, tmp_path):
        """CREATE INDEX after inserting a single vertex should succeed."""
        db_dir = str(tmp_path / "test_create_index_after_insert")
        shutil.rmtree(db_dir, ignore_errors=True)

        db = Database(db_dir, "w")
        conn = db.connect()
        conn.execute(
            "CREATE NODE TABLE User(id INT64 PRIMARY KEY, name STRING, age INT64);"
        )
        conn.execute("CREATE (:User {id: 1, name: 'Alice', age: 30});")
        conn.execute("CREATE INDEX idx_user_age ON User USING HNSW (age);")
        conn.close()
        db.close()

    def test_create_index_after_multiple_inserts(self, tmp_path):
        """CREATE INDEX after inserting multiple vertices should succeed."""
        db_dir = str(tmp_path / "test_create_index_multi_insert")
        shutil.rmtree(db_dir, ignore_errors=True)

        db = Database(db_dir, "w")
        conn = db.connect()
        conn.execute(
            "CREATE NODE TABLE User(id INT64 PRIMARY KEY, name STRING, age INT64);"
        )
        conn.execute("CREATE (:User {id: 1, name: 'Alice', age: 30});")
        conn.execute("CREATE (:User {id: 2, name: 'Bob', age: 25});")
        conn.execute("CREATE (:User {id: 3, name: 'Charlie', age: 35});")
        conn.execute("CREATE INDEX idx_user_age ON User USING HNSW (age);")
        conn.close()
        db.close()

    def test_create_index_multi_columns(self, tmp_path):
        """CREATE INDEX on multiple columns should succeed."""
        db_dir = str(tmp_path / "test_create_index_multi_cols")
        shutil.rmtree(db_dir, ignore_errors=True)

        db = Database(db_dir, "w")
        conn = db.connect()
        conn.execute(
            "CREATE NODE TABLE User(id INT64 PRIMARY KEY, name STRING, age INT64);"
        )
        conn.execute("CREATE INDEX idx_user_name_age ON User USING HNSW (name, age);")
        conn.close()
        db.close()

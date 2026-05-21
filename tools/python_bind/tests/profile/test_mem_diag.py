"""Diagnostic test: isolate Arrow memory growth sources.

Compares batch_read=true vs batch_read=false (full_read) to isolate
whether the issue is in Arrow batch streaming or general CSV loading.
Also tests different batch_size values.
"""

import os
import shutil
import sys
import time

import pytest

sys.path.append(os.path.join(os.path.dirname(__file__), "../../"))
from neug.database import Database

DATA_DIR = os.path.join(os.path.dirname(__file__), "data")
DB_BASE_DIR = "/tmp/test_mem_diag"


def get_current_rss_mb():
    """Get current RSS in MB via ps."""
    import subprocess
    result = subprocess.run(["ps", "-o", "rss=", "-p", str(os.getpid())],
                            capture_output=True, text=True)
    if result.returncode == 0:
        return int(result.stdout.strip()) / 1024  # KB -> MB
    return -1


class TestMemDiag:
    """Diagnostic memory tests."""

    def _run_copy(self, csv_path, label, use_batch_read, batch_size=1048576):
        """Run COPY FROM and measure RSS delta."""
        db_dir = os.path.join(DB_BASE_DIR, f"db_{label}")
        shutil.rmtree(db_dir, ignore_errors=True)

        db = Database(db_dir, "w")
        conn = db.connect()
        conn.execute(
            "CREATE NODE TABLE vertex("
            "id INT64, name STRING, age INT64, score DOUBLE, "
            "PRIMARY KEY(id));"
        )

        rss_before = get_current_rss_mb()

        if use_batch_read:
            query = (
                f'COPY vertex FROM "{csv_path}" '
                f'(header=true, delimiter=",", batch_read=true, batch_size={batch_size});'
            )
        else:
            query = (
                f'COPY vertex FROM "{csv_path}" '
                f'(header=true, delimiter=",");'
            )

        t0 = time.time()
        conn.execute(query)
        elapsed = time.time() - t0

        rss_after = get_current_rss_mb()
        growth = rss_after - rss_before

        del conn
        del db
        shutil.rmtree(db_dir, ignore_errors=True)

        return {"label": label, "growth_mb": growth, "elapsed_s": elapsed,
                "rss_before": rss_before, "rss_after": rss_after}

    def test_batch_vs_full_40mb(self):
        """Compare batch_read=true vs full_read for 40MB."""
        csv_path = os.path.join(DATA_DIR, "vertices_40mb.csv")
        if not os.path.exists(csv_path):
            pytest.skip("40MB test data not generated")

        print("\n" + "=" * 70)
        print("DIAGNOSTIC: batch_read vs full_read (40MB)")
        print("=" * 70)

        # Test 1: full_read (no batch_read)
        r1 = self._run_copy(csv_path, "full_read_40mb", use_batch_read=False)
        print(f"  full_read:  growth={r1['growth_mb']:.1f} MB, time={r1['elapsed_s']:.2f}s")

        # Test 2: batch_read with default 1MB batch_size
        r2 = self._run_copy(csv_path, "batch_1mb_40mb", use_batch_read=True, batch_size=1048576)
        print(f"  batch_read(1MB):  growth={r2['growth_mb']:.1f} MB, time={r2['elapsed_s']:.2f}s")

        # Test 3: batch_read with 4MB batch_size
        r3 = self._run_copy(csv_path, "batch_4mb_40mb", use_batch_read=True, batch_size=4194304)
        print(f"  batch_read(4MB):  growth={r3['growth_mb']:.1f} MB, time={r3['elapsed_s']:.2f}s")

        # Test 4: batch_read with 16MB batch_size
        r4 = self._run_copy(csv_path, "batch_16mb_40mb", use_batch_read=True, batch_size=16777216)
        print(f"  batch_read(16MB): growth={r4['growth_mb']:.1f} MB, time={r4['elapsed_s']:.2f}s")

        print(f"\n  >> batch_read overhead vs full_read: "
              f"{r2['growth_mb'] - r1['growth_mb']:.1f} MB extra")
        print(f"  >> If batch overhead is similar to full, "
              f"problem is in storage, not Arrow streaming")

    def test_batch_sizes_160mb(self):
        """Test different batch sizes for 160MB to see if readahead is the issue."""
        csv_path = os.path.join(DATA_DIR, "vertices_160mb.csv")
        if not os.path.exists(csv_path):
            pytest.skip("160MB test data not generated")

        print("\n" + "=" * 70)
        print("DIAGNOSTIC: varying batch_size (160MB)")
        print("=" * 70)

        # full_read baseline
        r0 = self._run_copy(csv_path, "full_read_160mb", use_batch_read=False)
        print(f"  full_read:        growth={r0['growth_mb']:.1f} MB, time={r0['elapsed_s']:.2f}s")

        for bs_mb, bs in [(1, 1048576), (4, 4194304), (16, 16777216), (64, 67108864)]:
            r = self._run_copy(csv_path, f"batch_{bs_mb}mb_160mb",
                              use_batch_read=True, batch_size=bs)
            print(f"  batch_read({bs_mb:>2}MB): growth={r['growth_mb']:.1f} MB, "
                  f"time={r['elapsed_s']:.2f}s, "
                  f"extra_vs_full={r['growth_mb'] - r0['growth_mb']:.1f} MB")

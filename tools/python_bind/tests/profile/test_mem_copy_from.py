"""Memory profiling test for COPY FROM with batch_read=true.

This test measures RSS memory growth during COPY FROM operations
on vertex tables of different sizes (40MB, 160MB, 640MB CSV files).

Run:
    cd tools/python_bind
    python3 -m pytest -sv tests/profile/test_mem_copy_from.py
"""

import logging
import os
import resource
import shutil
import sys
import time

import pytest

sys.path.append(os.path.join(os.path.dirname(__file__), "../../"))
from neug.database import Database

logger = logging.getLogger(__name__)

DATA_DIR = os.path.join(os.path.dirname(__file__), "data")
DB_BASE_DIR = "/tmp/test_mem_profile"


def get_rss_mb():
    """Get current process RSS in MB."""
    # ru_maxrss is in bytes on macOS, KB on Linux
    usage = resource.getrusage(resource.RUSAGE_SELF)
    if sys.platform == "darwin":
        return usage.ru_maxrss / (1024 * 1024)
    else:
        return usage.ru_maxrss / 1024


def get_current_rss_mb():
    """Get current RSS (not peak) in MB."""
    if sys.platform == "darwin":
        import ctypes
        import ctypes.util

        libc = ctypes.CDLL(ctypes.util.find_library("c"))

        class rusage(ctypes.Structure):
            _fields_ = [
                ("ru_utime_sec", ctypes.c_long),
                ("ru_utime_usec", ctypes.c_int32),
                ("ru_stime_sec", ctypes.c_long),
                ("ru_stime_usec", ctypes.c_int32),
                ("ru_maxrss", ctypes.c_long),
                ("ru_ixrss", ctypes.c_long),
                ("ru_idrss", ctypes.c_long),
                ("ru_isrss", ctypes.c_long),
                ("ru_minflt", ctypes.c_long),
                ("ru_majflt", ctypes.c_long),
                ("ru_nswap", ctypes.c_long),
                ("ru_inblock", ctypes.c_long),
                ("ru_oublock", ctypes.c_long),
                ("ru_msgsnd", ctypes.c_long),
                ("ru_msgrcv", ctypes.c_long),
                ("ru_nsignals", ctypes.c_long),
                ("ru_nvcsw", ctypes.c_long),
                ("ru_nivcsw", ctypes.c_long),
            ]

        import subprocess

        result = subprocess.run(["ps", "-o", "rss=", "-p", str(os.getpid())],
                                capture_output=True, text=True)
        if result.returncode == 0:
            return int(result.stdout.strip()) / 1024  # KB -> MB
    else:
        try:
            with open(f"/proc/{os.getpid()}/status") as f:
                for line in f:
                    if line.startswith("VmRSS:"):
                        return int(line.split()[1]) / 1024  # KB -> MB
        except Exception:
            pass
    return get_rss_mb()


def ensure_test_data():
    """Ensure test data files exist."""
    if not os.path.exists(DATA_DIR):
        os.makedirs(DATA_DIR, exist_ok=True)

    # Only generate 40MB for quick validation
    from tests.profile.gen_test_data import generate_csv

    sizes = [40, 160, 640]
    for size in sizes:
        path = os.path.join(DATA_DIR, f"vertices_{size}mb.csv")
        if not os.path.exists(path):
            generate_csv(path, size)


class TestMemCopyFrom:
    """Memory profiling for COPY FROM with batch_read."""

    @classmethod
    def setup_class(cls):
        """Generate test data if needed."""
        ensure_test_data()

    def _run_copy_from(self, csv_path, size_label, batch_size=1048576):
        """Run COPY FROM and measure memory growth."""
        db_dir = os.path.join(DB_BASE_DIR, f"db_{size_label}")
        shutil.rmtree(db_dir, ignore_errors=True)

        file_size_mb = os.path.getsize(csv_path) / (1024 * 1024)
        print(f"\n{'='*60}")
        print(f"Test: {size_label} (file={file_size_mb:.1f} MB)")
        print(f"batch_size={batch_size}")
        print(f"{'='*60}")

        rss_before_db = get_current_rss_mb()
        print(f"[1] Before DB open: RSS={rss_before_db:.1f} MB")

        db = Database(db_dir, "w")
        conn = db.connect()

        rss_after_db = get_current_rss_mb()
        print(f"[2] After DB open: RSS={rss_after_db:.1f} MB "
              f"(delta={rss_after_db - rss_before_db:.1f} MB)")

        conn.execute(
            "CREATE NODE TABLE vertex("
            "id INT64, name STRING, age INT64, score DOUBLE, "
            "PRIMARY KEY(id));"
        )

        rss_after_schema = get_current_rss_mb()
        print(f"[3] After schema: RSS={rss_after_schema:.1f} MB "
              f"(delta={rss_after_schema - rss_after_db:.1f} MB)")

        # Execute COPY FROM with batch_read
        query = (
            f'COPY vertex FROM "{csv_path}" '
            f'(header=true, delimiter=",", batch_read=true, batch_size={batch_size}, parallel=false);'
        )
        print(f"[4] Executing: {query}")

        t0 = time.time()
        conn.execute(query)
        elapsed = time.time() - t0

        rss_after_copy = get_current_rss_mb()
        total_growth = rss_after_copy - rss_after_schema
        print(f"[5] After COPY: RSS={rss_after_copy:.1f} MB "
              f"(delta={total_growth:.1f} MB, time={elapsed:.2f}s)")

        # Verify data was loaded
        result = list(conn.execute("MATCH (n:vertex) RETURN count(n);"))
        row_count = result[0][0] if result else 0
        print(f"[6] Loaded {row_count} vertices")

        # Calculate expected storage size (rough estimate)
        # id(8B) + name(~20B + overhead) + age(8B) + score(8B) ~= 52B per row
        expected_storage_mb = row_count * 52 / (1024 * 1024)
        arrow_overhead = total_growth - expected_storage_mb
        print(f"[7] Expected graph storage ~{expected_storage_mb:.1f} MB")
        print(f"[8] Unexpected extra memory (arrow overhead): "
              f"~{arrow_overhead:.1f} MB")
        print(f"[9] Ratio (total_growth / file_size): "
              f"{total_growth / file_size_mb:.2f}x")

        # Cleanup
        del conn
        del db
        shutil.rmtree(db_dir, ignore_errors=True)

        return {
            "size_label": size_label,
            "file_size_mb": file_size_mb,
            "total_growth_mb": total_growth,
            "expected_storage_mb": expected_storage_mb,
            "arrow_overhead_mb": arrow_overhead,
            "row_count": row_count,
            "elapsed_s": elapsed,
        }

    def test_copy_40mb(self):
        """Profile COPY FROM with 40MB dataset."""
        csv_path = os.path.join(DATA_DIR, "vertices_40mb.csv")
        if not os.path.exists(csv_path):
            pytest.skip("40MB test data not generated")
        result = self._run_copy_from(csv_path, "40MB")
        # Arrow overhead should ideally be close to batch_size (1MB)
        # If it's >> 10MB, there's a problem
        print(f"\n>>> VERDICT: arrow_overhead={result['arrow_overhead_mb']:.1f} MB "
              f"({'ISSUE DETECTED' if result['arrow_overhead_mb'] > 10 else 'OK'})")

    def test_copy_160mb(self):
        """Profile COPY FROM with 160MB dataset."""
        csv_path = os.path.join(DATA_DIR, "vertices_160mb.csv")
        if not os.path.exists(csv_path):
            pytest.skip("160MB test data not generated")
        result = self._run_copy_from(csv_path, "160MB")
        print(f"\n>>> VERDICT: arrow_overhead={result['arrow_overhead_mb']:.1f} MB "
              f"({'ISSUE DETECTED' if result['arrow_overhead_mb'] > 20 else 'OK'})")

    def test_copy_640mb(self):
        """Profile COPY FROM with 640MB dataset."""
        csv_path = os.path.join(DATA_DIR, "vertices_640mb.csv")
        if not os.path.exists(csv_path):
            pytest.skip("640MB test data not generated")
        result = self._run_copy_from(csv_path, "640MB")
        print(f"\n>>> VERDICT: arrow_overhead={result['arrow_overhead_mb']:.1f} MB "
              f"({'ISSUE DETECTED' if result['arrow_overhead_mb'] > 40 else 'OK'})")

    def test_summary(self):
        """Run all sizes and produce summary report."""
        results = []
        for size in [40, 160, 640]:
            csv_path = os.path.join(DATA_DIR, f"vertices_{size}mb.csv")
            if not os.path.exists(csv_path):
                print(f"Skipping {size}MB - data not generated")
                continue
            results.append(self._run_copy_from(csv_path, f"{size}MB"))

        if not results:
            pytest.skip("No test data available")

        print(f"\n{'='*60}")
        print("SUMMARY REPORT")
        print(f"{'='*60}")
        print(f"{'Size':<8} {'File':<10} {'Growth':<10} {'Storage':<10} "
              f"{'Arrow OH':<10} {'Ratio':<8}")
        print("-" * 60)
        for r in results:
            print(f"{r['size_label']:<8} {r['file_size_mb']:<10.1f} "
                  f"{r['total_growth_mb']:<10.1f} "
                  f"{r['expected_storage_mb']:<10.1f} "
                  f"{r['arrow_overhead_mb']:<10.1f} "
                  f"{r['total_growth_mb']/r['file_size_mb']:<8.2f}")

        print(f"\nConclusion:")
        if len(results) >= 2:
            growth_ratio = results[-1]['arrow_overhead_mb'] / results[0]['arrow_overhead_mb']
            size_ratio = results[-1]['file_size_mb'] / results[0]['file_size_mb']
            print(f"  File size ratio: {size_ratio:.1f}x")
            print(f"  Arrow overhead ratio: {growth_ratio:.1f}x")
            if growth_ratio > size_ratio * 0.5:
                print("  -> Arrow overhead scales with file size: MEMORY ISSUE EXISTS")
            else:
                print("  -> Arrow overhead is bounded: NO significant issue")

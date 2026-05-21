"""Generate CSV test data for memory profiling.

Generates vertex CSV files of approximate target sizes:
- 40MB, 160MB, 640MB

Schema: id (INT64 PK), name (STRING), age (INT64), score (DOUBLE)
"""

import os
import random
import string


def random_string(length=20):
    return "".join(random.choices(string.ascii_lowercase + string.digits, k=length))


def generate_csv(output_path, target_size_mb):
    """Generate a CSV file of approximately target_size_mb."""
    target_bytes = target_size_mb * 1024 * 1024
    header = "id,name,age,score\n"

    written = 0
    row_id = 0
    with open(output_path, "w") as f:
        f.write(header)
        written += len(header)
        while written < target_bytes:
            name = random_string(20)
            age = random.randint(18, 80)
            score = round(random.uniform(0.0, 100.0), 4)
            line = f"{row_id},{name},{age},{score}\n"
            f.write(line)
            written += len(line)
            row_id += 1

    actual_mb = os.path.getsize(output_path) / (1024 * 1024)
    print(f"Generated {output_path}: {actual_mb:.1f} MB, {row_id} rows")
    return row_id


def main():
    data_dir = os.path.join(os.path.dirname(__file__), "data")
    os.makedirs(data_dir, exist_ok=True)

    for size_mb in [40, 160, 640]:
        output_path = os.path.join(data_dir, f"vertices_{size_mb}mb.csv")
        if os.path.exists(output_path):
            actual_mb = os.path.getsize(output_path) / (1024 * 1024)
            print(f"Already exists: {output_path} ({actual_mb:.1f} MB)")
            continue
        generate_csv(output_path, size_mb)


if __name__ == "__main__":
    main()

"""Regression coverage for the actual indexed and expanded detector drivers."""

import csv
import math
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


EXECUTABLE = Path(sys.argv.pop(1)).resolve()

# Shared vertices, non-identity corner order, two source groups, a duplicate
# source point (unused by faces), a duplicate triangle and a collapsed triangle.
# Twelve expanded corners outnumber the four render and five source positions.
MODEL = """v 0 0 0
v 2 0 0
v 2 2 0
v 0 2 0
v 0 0 0
g first
f 3 4 1
f 3 1 2
g second
f 3 4 1
f 2 2 3
"""


class DetectorBenchmarkTests(unittest.TestCase):
    def test_expanded_matches_indexed_source_and_group_counts(self):
        with tempfile.TemporaryDirectory(prefix="woby detector benchmark ") as directory:
            root = Path(directory).resolve()
            model = root / "shared groups.obj"
            model.write_text(MODEL, encoding="utf-8")
            outputs = []
            for workload in ("detectors", "detectors-expanded"):
                with self.subTest(workload=workload):
                    result = subprocess.run(
                        [str(EXECUTABLE), workload, str(model), "3"],
                        cwd=root, capture_output=True, text=True, timeout=60,
                    )
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertEqual(result.stderr, "")
                    lines = result.stdout.splitlines()
                    self.assertEqual(lines[0], "triangles=4 source_points=5 parts=2")
                    self.assertEqual(lines[1], "run,stage,milliseconds,counts")
                    rows = list(csv.reader(lines[2:]))
                    # Source (1), automatic detector batch (2|4|8|64), total.
                    self.assertEqual([(r[0], r[1]) for r in rows], [
                        (str(run), stage) for run in range(3) for stage in ("1", "78", "total")
                    ])
                    for row in rows:
                        elapsed = float(row[2])
                        self.assertTrue(math.isfinite(elapsed) and elapsed >= 0, row)
                        self.assertEqual(len(row), 12 if row[1] == "78" else 3)
                    counts = [tuple(map(int, row[3:])) for row in rows if row[1] == "78"]
                    self.assertEqual(counts, [counts[0]] * 3)
                    # Source IDs must survive expansion: the unused duplicate
                    # point, repeated face and degenerate face each count once.
                    self.assertEqual(counts[0][3:6], (1, 1, 1))
                    outputs.append(counts)
            self.assertEqual(len(outputs), 2)
            self.assertEqual(outputs[0], outputs[1])


if __name__ == "__main__":
    unittest.main()

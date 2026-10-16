"""Check replay equivalence on seams, multiple groups, and unused positions."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory(prefix="woby annotation replay ") as temporary:
    root = Path(temporary).resolve()
    model = root / "seams.obj"
    model.write_text("v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nv 9000 9000 9000\n"
                     "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\nvt .5 .5\n"
                     "g first\nf 3/3 1/1 2/2\ng second\nf 1/5 3/3 4/4\n", encoding="utf-8")
    executable = Path(sys.argv[1]).resolve(strict=True)
    result = subprocess.run([str(executable), str(model), "1"], cwd=root, capture_output=True, text=True, timeout=30, check=True)
    record = json.loads(result.stdout)
    assert record["groups"] == 2
    assert len(record["records"]) == 7
    assert sum(row["hash_validated"] for row in record["records"]) == 4
    assert sum(row["bounds_validated"] for row in record["records"]) == 6
    invalid = subprocess.run([str(executable), str(model), "0"], cwd=root, capture_output=True, text=True, timeout=10)
    assert invalid.returncode != 0
print("Annotation replay output checks passed")

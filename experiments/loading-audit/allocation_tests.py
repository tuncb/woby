"""Check that growth auditing retains parser output and accounts for its ranges."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def run(executable, model):
    return json.loads(subprocess.check_output([executable, "prototype", str(model), str(256 * 1024), "4"], text=True))


with tempfile.TemporaryDirectory(prefix="woby_parser_alloc_") as directory:
    root = Path(directory).resolve()
    model = root / "geometry.obj"
    model.write_text("v 1 2 3\n" * 25000 + "vt .5 .25\n" * 25000 + "f 1/1 2/2 3/3\n" * 150000, encoding="utf-8")
    current = run(sys.argv[1], model)
    audited = run(sys.argv[2], model)
    assert current["fingerprint"] == audited["fingerprint"]
    assert current["positions"] == audited["positions"] == 25000
    assert current["faces"] == audited["faces"] == 150000
    audit = audited["allocation_audit"]
    assert len(audit["ranges"]) == (model.stat().st_size + 256 * 1024 - 1) // (256 * 1024)
    assert len(audit["ranges"]) > 4  # Each worker must be able to report more than one block.
    assert sum(row["input_bytes"] for row in audit["ranges"]) == model.stat().st_size
    for buffer in audit["buffers"]:
        assert buffer["copy_payload_bytes"] <= buffer["allocated_bytes"]
        assert buffer["largest_allocation_bytes"] <= buffer["allocated_bytes"]
    assert sum(row["allocated_bytes"] for row in audit["buffers"]) > 0
    assert sum(row["copy_payload_bytes"] for row in audit["ranges"]) <= sum(row["copy_payload_bytes"] for row in audit["buffers"])
    assert sum(row["faces"] for row in audit["ranges"]) <= audited["faces"]
print("Allocation audit output and range checks passed")

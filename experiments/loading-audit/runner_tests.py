# /// script
# requires-python = ">=3.11"
# dependencies = ["psutil>=6", "nvidia-ml-py>=12", "Pillow>=11"]
# ///
"""Check capture provenance without starting a viewer or a profiler."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory(prefix="woby audit provenance ") as temporary:
    root = Path(temporary).resolve()
    binaries = root / "bin"
    binaries.mkdir()
    names = ("woby", "woby_obj_e2e_viewer", "woby_obj_profile_avx2", "woby_parser_benchmark", "woby_parser_benchmark_modern")
    for name in names:
        (binaries / (name + ".exe")).write_bytes(("MZ fixture " + name).encode("ascii"))
    model = root / "model.obj"
    model.write_text("v 0 0 0\n", encoding="utf-8")
    header = root / "number-decoder.h"
    header.write_text("namespace fast_float {}\n", encoding="utf-8")
    output = root / "capture"
    subprocess.run([sys.executable, str(Path(__file__).with_name("run.py")), str(binaries), str(model),
                    "--output", str(output), "--steps", "", "--fast-float-header", str(header)],
                   cwd=root, capture_output=True, text=True, timeout=30, check=True)
    provenance = json.loads((output / "provenance.json").read_text(encoding="utf-8"))
    assert provenance["model_sha256"] == hashlib.sha256(model.read_bytes()).hexdigest()
    assert provenance["fast_float_v8_3_2_sha256"] == hashlib.sha256(header.read_bytes()).hexdigest()
    assert set(provenance["executable_sha256"]) == set(names)
    assert not (output / "native").exists()
    assert all((output / "matching-bin" / (name + ".exe")).read_bytes() == (binaries / (name + ".exe")).read_bytes() for name in names)
print("Capture provenance checks passed")

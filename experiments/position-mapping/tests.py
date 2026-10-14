"""Check forced position mapping, output parity, and retained path guards."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def run(exe, model, mode, deferred):
    command = [str(exe), mode, str(model), "2"] + (["deferred"] if deferred else [])
    result = subprocess.run(command, capture_output=True, text=True, check=True, timeout=30)
    return json.loads(result.stdout)


def fixture(exe, name, text, normally_staged, force_staged):
    with tempfile.TemporaryDirectory(prefix="woby position paths ") as temporary:
        model = Path(temporary).resolve() / (name + ".obj")
        model.write_text(text, encoding="ascii")
        for deferred in (False, True):
            normal = run(exe, model, "prototype", deferred)
            forced = run(exe, model, "forced", deferred)
            assert normal["corner_mapping"]["staged"] == normally_staged, name
            assert forced["corner_mapping"]["staged"] == force_staged, name
            assert normal["fingerprint"] == forced["fingerprint"], name
        if name == "position":
            verified = subprocess.run([str(exe), "verify", str(model)], capture_output=True,
                                      text=True, check=True, timeout=30)
            assert verified.stdout.count("All mesh buffers and group fields match") == 2


if __name__ == "__main__":
    exe = Path(sys.argv[1]).resolve(strict=True)
    positions = "v 0 0 0\nv 1 0 0\nv 0 1 0\nv 7 7 7\n"
    faces = "g first\nf 3 1 2\ng second\nf 2 1 3\n"
    fixture(exe, "position", positions + faces, False, True)
    fixture(exe, "identity", positions + "f 1 2 3\n", False, True)
    fixture(exe, "mixed", positions + faces + "l 1 4\np 4\n", False, False)
    fixture(exe, "cloud", positions, False, False)
    fixture(exe, "sparse", positions * 4 + "f 3 1 2\n", False, False)
    fixture(exe, "normals", positions + "vn 0 0 1\nf 3//1 1//1 2//1\n", True, True)
    print("Six path-selection and output-parity fixtures passed.")

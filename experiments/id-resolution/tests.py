"""Check resolver replay controls with one temporary root per fixture."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def check_fixture(executable, mode):
    with tempfile.TemporaryDirectory(prefix="woby ID resolution ") as temporary:
        root = Path(temporary).resolve()
        model, output = root / "input.obj", root / "timing.jsonl"
        text = "".join(f"v {i % 7} {i // 7} 0\n" for i in range(37))
        if mode == "normals":
            text += "vn 0 0 1\nvn 0 1 0\n"
        else:
            count = 401 if mode == "unused" else 17
            text += "".join(f"vt {i / 23} .25\n" for i in range(count - 1))
            text += "vt 1e40 .5\n" if mode == "overflow" else "vt .5 .75\n"
        text += "g first\n"
        for face in range(97):
            if face == 1:
                text += "g remainder\n"
            corners = []
            for corner in range(3):
                position = (face * 13 + corner * 17) % 37 + 1
                corners.append(f"{position}//{face % 2 + 1}" if mode == "normals"
                               else f"{position}/{(face * 7 + corner) % 17 + 1}")
            text += "f " + " ".join(corners) + "\n"
        model.write_text(text, encoding="ascii")
        result = subprocess.run([str(executable), str(model), str(output), "1"],
                                capture_output=True, text=True, check=True)
        rows = [json.loads(line) for line in result.stdout.splitlines()]
        assert rows == [json.loads(line) for line in output.read_text().splitlines()]
        assert rows[0]["corners"] == 291
        assert rows[0]["positions"] == 37
        measured = [row for row in rows if "variant" in row]
        assert {"original", "production", "parallel1", "parallel4", "parallel8", "uv-p32", "finite-batch"} <= {row["variant"] for row in measured}
        assert all(row["exact"] and row["resolve_ms"] >= 0 for row in measured)
        assert len({row["vertices"] for row in measured}) == 1
        saved = output.read_bytes()
        repeated = subprocess.run([str(executable), str(model), str(output), "1"], capture_output=True, text=True)
        assert repeated.returncode != 0 and "already exists" in repeated.stderr
        assert output.read_bytes() == saved
        for rounds in (0, 101):
            invalid_output = root / f"invalid-rounds-{rounds}.jsonl"
            invalid = subprocess.run([str(executable), str(model), str(invalid_output), str(rounds)],
                                     capture_output=True, text=True)
            assert invalid.returncode != 0 and "one to 100" in invalid.stderr
            assert not invalid_output.exists()


if __name__ == "__main__":
    executable = Path(sys.argv[1]).resolve(strict=True)
    for fixture in ("finite", "overflow", "normals", "unused"):
        check_fixture(executable, fixture)
    print("Four resolver replay fixtures passed.")

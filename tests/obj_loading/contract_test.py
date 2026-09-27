"""Check that parser benchmarks measure equivalent, untriangulated geometry."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    exe = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="woby-obj-bench-") as temporary:
        root = Path(temporary)
        model = root / "fixture.obj"
        model.write_text("mtllib absent.mtl\n"
                         "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n"
                         "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\nvn 0 0 1\n"
                         "g quad\nf 1/1/1 2/2/1 3/3/1 4/4/1\n"
                         "g relative\nf -4/1/1 -2/3/1 -1/4/1\n", encoding="ascii")
        reference = None
        for backend in ("rapid", "fast", "tiny", "tiny-opt", "tiny-typed", "tiny-opt-cache", "tiny-typed-cache"):
            data = json.loads(subprocess.check_output([exe, backend, model, "2"], cwd=root, text=True))
            assert data["ok"] and data["load_ms"] >= 0, data
            geometry = data["geometry"]
            assert geometry["positions"] == 4 and geometry["faces"] == 2, data
            assert geometry["corners"] == 7 and geometry["triangles"] == 3, data
            assert reference is None or geometry == reference, data
            reference = geometry
        data = json.loads(subprocess.check_output([exe, "woby", model], cwd=root, text=True))
        assert data["triangles"] == 3 and data["positions"] == 4, data
        assert set(data["stages"]) == {"parse_ms", "triangulate_ms", "source_copy_ms", "vertex_map_ms", "normals_ms", "bounds_ms", "compact_ms"}, data
        for backend, path in (("unknown", model), ("rapid", "relative.obj")):
            failed = subprocess.run([exe, backend, path], cwd=root, capture_output=True, text=True)
            assert failed.returncode != 0 and not json.loads(failed.stdout)["ok"], failed
        output = root / "results.jsonl"
        runner = Path(__file__).resolve().with_name("run.py")
        command = [sys.executable, str(runner), "--bin", str(exe.parent), "--models", str(model),
                   "--output", str(output), "--rounds", "1", "--backends"]
        subprocess.run(command + ["rapid", "fast"], cwd=root, check=True, capture_output=True)
        assert len(output.read_text().splitlines()) == 2
        failed = subprocess.run(command + ["unknown"], cwd=root, capture_output=True)
        assert failed.returncode != 0
        rows = [json.loads(line) for line in output.read_text().splitlines()]
        assert len(rows) == 3 and not rows[-1]["ok"]


if __name__ == "__main__":
    main()

"""Render both paths on the same isolated fixture and compare actual pixels."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def run(binary, model, mode, output, quick=True, seconds=2.0):
    command = [str(binary), "--model", str(model), "--mode", mode, "--captures", str(output)]
    if quick:
        command += ["--quick", "--width", "320", "--height", "240"]
    else:
        command += ["--seconds", str(seconds)]
    result = subprocess.run(command, cwd=model.parent, capture_output=True, text=True, timeout=600)
    output.mkdir(parents=True, exist_ok=True)
    (output / "stderr.log").write_text(result.stderr)
    (output / "stdout.log").write_text(result.stdout)
    lines = [line for line in result.stdout.splitlines() if line.startswith("{")]
    data = json.loads(lines[-1]) if lines else {"ok": False, "error": "no JSON result"}
    if result.returncode != 0 or not data.get("ok"):
        raise RuntimeError({"exit": result.returncode, "result": data, "logs": str(output), "stderr_tail": result.stderr[-2000:]})
    return data


def compare_images(left, right, name):
    a = (left / f"{name}.rgba").read_bytes()
    b = (right / f"{name}.rgba").read_bytes()
    assert len(a) == len(b) and len(a) > 0
    if a == b:
        return {"different_pixels": 0, "pixels": len(a)//4, "max_channel_difference": 0}
    changed = sum(a[i:i+4] != b[i:i+4] for i in range(0, len(a), 4))
    max_delta = max(abs(x-y) for x, y in zip(a, b))
    return {"different_pixels": changed, "pixels": len(a)//4, "max_channel_difference": max_delta}


def main():
    binary = Path(sys.argv[1]).resolve()
    # Shared positions, hard-normal seams, shared IDs across groups, occlusion,
    # repeated corners and an unused source point. Every path derives from root.
    fixture = """v -1 -1 0
v 1 -1 0
v 0 1 0
v -.5 -.5 -.2
v .5 -.5 -.2
v 0 .5 -.2
v 99 99 99
vn 0 0 1
vn 0 0 -1
g first
f 1//1 2//1 3//1
f 1//1 3//1 2//1
g shared
f 1//1 4//1 5//1
f 4//1 5//1 6//1
g seam
f 1//2 2//2 3//2
g repeated
f 4//1 4//1 5//1
"""
    with tempfile.TemporaryDirectory(prefix="woby-vertex-drawing-") as temporary:
        root = Path(temporary).resolve()
        model = root / "fixture.obj"
        model.write_text(fixture)
        current = run(binary, model, "current", root / "current")
        assert current["point_entries"] == 12
        assert current["marker_payload_bytes"] == 104 * current["point_entries"]
        for mode in ("shader", "flat"):
            shader = run(binary, model, mode, root / mode)
            for key in ("point_entries", "point_hash", "vertices", "indices", "groups", "base_payload_bytes"):
                assert current[key] == shader[key], key
            assert shader["marker_payload_bytes"] == 4 * shader["point_entries"]
            for a, b in zip(current["scenarios"], shader["scenarios"], strict=True):
                assert a["name"] == b["name"]
                assert a["image"]["covered_pixels"] > 0 and b["image"]["covered_pixels"] > 0
                comparison = compare_images(root / "current", root / mode, a["name"])
                assert comparison["different_pixels"] == 0, (mode, a["name"], comparison)
        failed = subprocess.run([str(binary), "--model", "relative.obj"], cwd=root, capture_output=True, text=True)
        assert failed.returncode != 0 and "absolute model path" in failed.stdout
    print("Vertex drawing contract passed: five pixel-identical scenarios and preserved membership.")


if __name__ == "__main__":
    main()

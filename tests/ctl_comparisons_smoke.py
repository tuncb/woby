"""Exercise comparison rendering, persistence, and invalidation in a real viewer.

python tests/ctl_comparisons_smoke.py build/vs2026-vcpkg/bin/Debug/woby.exe \
    build/comparison-qa
"""
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import uuid


def main():
    executable = Path(sys.argv[1]).resolve()
    root = Path(sys.argv[2]).resolve()
    root.mkdir(parents=True, exist_ok=True)
    instance = "comparisons-" + uuid.uuid4().hex[:12]
    for index, height in enumerate((1, 1.12)):
        vertices = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0),
                    (0, 0, height), (1, 0, height), (1, 1, height), (0, 1, height)]
        faces = [(1, 3, 2), (1, 4, 3), (5, 6, 7), (5, 7, 8), (1, 2, 6), (1, 6, 5),
                 (2, 3, 7), (2, 7, 6), (3, 4, 8), (3, 8, 7), (4, 1, 5), (4, 5, 8)]
        (root / f"cube-{index}.obj").write_text(
            "o cube\n" + "".join(f"v {x} {y} {z}\n" for x, y, z in vertices)
            + "".join(f"f {a} {b} {c}\n" for a, b, c in faces), encoding="utf-8")
    scene = "version = 5\nshow_grid = false\nshow_origin = false\n"
    for index in range(2):
        scene += (f'[[files]]\npath = "cube-{index}.obj"\n'
                  '[[files.groups]]\nname = "cube"\nshow_vertices = false\nshow_triangles = false\n'
                  'color = [0.4, 0.6, 0.8, 1]\n')
    for name, mode, offset in (("Distance check", "distance", 2.5), ("Overlay check", "overlay", 5)):
        scene += (f'[[comparisons]]\nname = "{name}"\ntranslation = [{offset}, 0, 0]\n'
                  f'comparison_enabled = true\ncomparison_mode = "{mode}"\n')
        for side, index in (("a", 0), ("b", 1)):
            scene += (f'[[comparisons.{side}]]\nfile_index = {index}\ngroup_index = 0\n'
                      f'name = "cube-{index}.obj / cube"\n')
    scene_path = root / "multiple.woby"
    scene_path.write_text(scene, encoding="utf-8")
    startup = None
    if os.name == "nt":
        startup = subprocess.STARTUPINFO()
        startup.dwFlags = subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = 0
    with (root / "viewer.log").open("w", encoding="utf-8") as log:
        viewer = subprocess.Popen([str(executable), "--instance", instance, "--scene", str(scene_path)],
                                  cwd=root, stdout=log, stderr=log, startupinfo=startup)

        def ctl(*args, success=True):
            process = subprocess.run([str(executable), "ctl", "--instance", instance, *map(str, args), "--json"],
                                     cwd=root, capture_output=True, text=True, timeout=60)
            result = json.loads(process.stdout)
            assert (process.returncode == 0) == success, (args, result)
            return result

        try:
            deadline = time.monotonic() + 30
            while True:
                assert viewer.poll() is None, (root / "viewer.log").read_text()
                discovered = subprocess.run([str(executable), "ctl", "instances", "--json"],
                                            capture_output=True, text=True, timeout=5)
                if any(item["id"] == instance and item["ready"] for item in json.loads(discovered.stdout)):
                    break
                assert time.monotonic() < deadline, "viewer did not become ready"
                time.sleep(.05)
            objects = ctl("objects")["objects"]
            comparisons = [item for item in objects if item["kind"] == "comparison"]
            files = [item for item in objects if item["kind"] == "file"]
            assert len(comparisons) == 2 and len(files) == 2
            assert ctl("scene", "info")["comparisonCount"] == 2
            first, second = [item["id"] for item in comparisons]
            ctl("camera", "frame")
            ctl("screenshot", root / "both.png")
            ctl("visibility", "set", first, "--visible", "false")
            assert ctl("object", second)["object"]["settings"]["visible"]
            ctl("screenshot", root / "overlay-only.png")
            assert (root / "both.png").read_bytes() != (root / "overlay-only.png").read_bytes()
            ctl("visibility", "set", second, "--visible", "false")
            ctl("screenshot", root / "sources-only.png")
            assert (root / "overlay-only.png").read_bytes() != (root / "sources-only.png").read_bytes()
            ctl("visibility", "set", first, "--visible", "true")
            ctl("visibility", "set", second, "--visible", "true")
            for item in files:
                ctl("visibility", "set", item["id"], "--visible", "false")
            ctl("camera", "frame")
            assert ctl("scene", "bounds")["bounds"]["min"][0] == 2.5
            ctl("screenshot", root / "results-only.png")
            for item in files:
                ctl("visibility", "set", item["id"], "--visible", "true")
            ctl("camera", "frame")
            ctl("transform", "set", second, "--translation", "5", "0", "1")
            assert ctl("object", first)["object"]["settings"]["translation"] == [2.5, 0, 0]
            ctl("transform", "set", files[1]["id"], "--translation", "0", "0", "0.25")
            ctl("screenshot", root / "updated.png")
            saved = root / "saved.woby"
            ctl("scene", "save-as", saved, "--overwrite")
            ctl("scene", "open", saved)
            reopened = [item for item in ctl("objects")["objects"] if item["kind"] == "comparison"]
            assert len(reopened) == 2
            assert {item["id"] for item in reopened}.isdisjoint({first, second})
            assert ctl("object", reopened[1]["id"])["object"]["settings"]["translation"] == [5, 0, 1]
            source = next(item for item in ctl("objects")["objects"] if item["kind"] == "file")
            ctl("model", "remove", source["id"])
            for item in reopened:
                assert not ctl("object", item["id"])["object"]["valid"]
                assert ctl("object", item["id"])["object"]["missingPartCount"] == 1
            ctl("screenshot", root / "incomplete.png", success=False)
            ctl("scene", "save-as", root / "missing.woby", "--overwrite")
            ctl("scene", "open", root / "missing.woby")
            for item in ctl("objects")["objects"]:
                if item["kind"] == "comparison":
                    assert ctl("object", item["id"])["object"]["missingPartCount"] == 1
            print("Comparison viewer smoke test passed: rendering, visibility, movement, recomputation, reopen, missing inputs.")
        finally:
            if viewer.poll() is None:
                try:
                    ctl("quit", "--on-dirty", "discard")
                    viewer.wait(timeout=10)
                except (AssertionError, subprocess.SubprocessError):
                    viewer.terminate()
                    viewer.wait(timeout=10)


if __name__ == "__main__":
    main()

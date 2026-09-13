"""Exercise degenerate findings in the real viewer; optionally save screenshots.

python tests/ctl_degenerates_smoke.py build/vs2026-vcpkg/bin/Debug/woby.exe build/degenerate-qa
"""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import uuid


def main():
    executable = Path(sys.argv[1]).resolve()
    output = Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else None
    if output:
        output.mkdir(parents=True, exist_ok=True)
    instance = "degenerates-" + uuid.uuid4().hex[:12]
    with tempfile.TemporaryDirectory(prefix="woby-degenerates-") as temporary:
        root = Path(temporary).resolve()
        model = root / "cases.obj"
        model.write_text(
            "o cases\nv 0 0 0\nv 1 0 0\nv 0 1 0\n"
            "v 2 0 0\nv 3 0 0\nv 2 .0001 0\n"
            "v 4 0 0\nv 6 0 0\nv 5 .01 0\n"
            "v 7 0 0\nv 8 0 0\nv 9 0 0\n"
            "f 1 2 3\nf 4 5 6\nf 7 8 9\nf 10 11 12\n", encoding="utf-8")
        scene = root / "detectors.woby"
        scene.write_text(
            'version = 9\nshow_grid = false\nshow_origin = false\n'
            '[[files]]\npath = "cases.obj"\n[[files.groups]]\nname = "cases"\n'
            'show_vertices = false\nshow_triangles = false\n'
            '[[analyses]]\nname = "Degenerate QA"\nanalysis_enabled = true\n'
            'analysis_mode = "a"\ndiagnostic_category = "degenerate_triangles"\n'
            'analysis_show_boundaries = false\nanalysis_show_non_manifold = false\n'
            '[[analyses.a]]\nfile_index = 0\ngroup_index = 0\nname = "cases"\n', encoding="utf-8")
        startup = None
        if os.name == "nt":
            startup = subprocess.STARTUPINFO()
            startup.dwFlags = subprocess.STARTF_USESHOWWINDOW
            startup.wShowWindow = 0
        with (root / "viewer.log").open("w", encoding="utf-8") as log:
            viewer = subprocess.Popen([str(executable), "--instance", instance, "--scene", str(scene)],
                                      cwd=root, stdout=log, stderr=log, startupinfo=startup)

            def ctl(*args):
                completed = subprocess.run([str(executable), "ctl", "--instance", instance,
                                            *map(str, args), "--json"], cwd=root,
                                           capture_output=True, text=True, timeout=65)
                assert completed.returncode == 0, (args, completed.stdout, completed.stderr)
                return json.loads(completed.stdout)

            try:
                deadline = time.monotonic() + 40
                while True:
                    assert viewer.poll() is None, (root / "viewer.log").read_text()
                    instances = subprocess.run([str(executable), "ctl", "instances", "--json"],
                                               capture_output=True, text=True, timeout=5)
                    if any(item["id"] == instance and item["ready"] for item in json.loads(instances.stdout)):
                        break
                    assert time.monotonic() < deadline, "viewer did not become ready"
                    time.sleep(.05)
                objects = ctl("objects")["objects"]
                analysis = next(item["id"] for item in objects if item["kind"] == "analysis")
                source = next(item["id"] for item in objects if item["kind"] == "file")

                def findings(side="aToB"):
                    return ctl("analysis", "results", analysis)[side]["detectors"]["degenerate_tris"]

                initial = findings()
                assert initial["count"] == 3, initial
                assert initial["reasonCounts"] == {"collapsed": 1, "needle": 1, "cap": 2}, initial
                assert [f["triangleId"] for f in initial["findings"]] == [2, 3, 4]
                legacy = ctl("analysis", "results", analysis)["aToB"]["diagnostics"]["degenerateTriangles"]
                assert legacy == 1
                if output:
                    ctl("camera", "view", "top")
                    ctl("camera", "frame", "--object", analysis)
                    ctl("screenshot", output / "degenerates.png")
                ctl("analysis", "set", analysis, "--show-degenerate-triangles", "false")
                assert findings() == initial
                ctl("analysis", "set", analysis, "--degenerate-triangles", "false", "--cap-min-angle-degrees", "179.5")
                disabled = findings()
                assert disabled["status"] == "disabled" and disabled["count"] is None and not disabled["findings"]
                assert disabled["capMinAngleDegrees"] == 179.5
                ctl("analysis", "set", analysis, "--degenerate-triangles", "true", "--show-degenerate-triangles", "true",
                    "--needle-threshold-ratio", "20000", "--cap-min-angle-degrees", "180")
                assert findings()["count"] == 1
                ctl("analysis", "set", analysis, "--needle-threshold-ratio", "1000", "--cap-min-angle-degrees", "179.5")
                assert findings()["count"] == 2
                saved = root / "saved.woby"
                ctl("scene", "save-as", saved, "--overwrite")
                assert "version = 11" in saved.read_text(encoding="utf-8")
                ctl("scene", "open", saved)
                objects = ctl("objects")["objects"]
                analysis = next(item["id"] for item in objects if item["kind"] == "analysis")
                source = next(item["id"] for item in objects if item["kind"] == "file")
                assert findings()["count"] == 2
                ctl("analysis", "add", analysis, "--side", "b", "--object", source)
                assert findings("bToA")["count"] == 2
                ctl("analysis", "clear", analysis, "--side", "a")
                assert findings("bToA")["count"] == 2
                ctl("analysis", "set", analysis, "--visible", "false", "--cap-min-angle-degrees", "177.5")
                assert findings("bToA")["count"] == 3  # Hidden analyses still compute fresh CLI results.
                print("Degenerate viewer smoke passed: source IDs, reasons, thresholds, Run/Show, persistence, A/B, hidden results, and screenshot.")
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

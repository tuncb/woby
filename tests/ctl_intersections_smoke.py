"""Exercise intersection findings in the real viewer; optionally save screenshots.

python tests/ctl_intersections_smoke.py build/vs2026-vcpkg/bin/Debug/woby.exe build/intersection-qa
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
    instance = "intersections-" + uuid.uuid4().hex[:12]
    with tempfile.TemporaryDirectory(prefix="woby-intersections-") as temporary:
        root = Path(temporary).resolve()
        model = root / "cases.obj"
        model.write_text(
            "o cases\nv 0 0 0\nv 2 0 0\nv 0 2 0\n"
            "v .5 .5 -1\nv .5 .5 1\nv 1.5 .5 0\n"
            "f 1 2 3\nf 4 5 6\n", encoding="utf-8")
        scene = root / "detectors.woby"
        scene.write_text(
            'version = 14\nshow_grid = false\nshow_origin = false\n'
            '[[files]]\npath = "cases.obj"\n[[files.groups]]\nname = "cases"\n'
            'show_vertices = false\nshow_triangles = false\n'
            '[[analyses]]\nname = "Intersection QA"\nanalysis_enabled = true\n'
            'analysis_mode = "a"\ndiagnostic_category = "self_intersections"\n'
            'self_intersections_auto_update = false\nshow_self_intersections = true\n'
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
                    return ctl("analysis", "results", analysis)[side]["detectors"]["self_intersections"]

                def run(side="aToB"):
                    ctl("analysis", "run", analysis, "--detector", "self_intersections")
                    return complete(side)

                def complete(side="aToB"):
                    deadline = time.monotonic() + 30
                    while True:
                        result = findings(side)
                        if result["status"] not in ("queued", "running"):
                            assert result["status"] == "complete", result
                            return result
                        assert time.monotonic() < deadline, result
                        time.sleep(.01)

                unchecked = findings()
                assert unchecked["status"] == "not_checked" and unchecked["count"] is None, unchecked
                initial = run()
                assert initial["count"] == 1 and initial["affectedFaceCount"] == 2, initial
                assert [f["triangleId"] for f in initial["findings"][0]["faces"]] == [1, 2]
                if output:
                    ctl("camera", "view", "isometric")
                    ctl("camera", "frame", "--object", analysis)
                    ctl("screenshot", output / "intersections.png")
                ctl("analysis", "set", analysis, "--show-self-intersections", "false")
                assert findings() == initial
                ctl("analysis", "set", analysis, "--show-self-intersections", "true")
                ctl("transform", "set", source, "--translation", "1", "0", "0")
                stale = findings()
                assert stale["status"] == "out_of_date" and stale["count"] is None, stale
                assert stale["previousCount"] == 1 and not stale["findings"], stale
                if output:
                    ctl("screenshot", output / "outdated.png")
                assert run()["count"] == 1
                ctl("analysis", "set", analysis, "--topology-mode", "exact_position")
                assert findings()["status"] == "out_of_date"
                assert run()["count"] == 1
                ctl("analysis", "cancel", analysis, "--detector", "self_intersections")
                assert findings()["status"] == "canceled"
                assert run()["count"] == 1
                saved = root / "saved.woby"
                ctl("scene", "save-as", saved, "--overwrite")
                text = saved.read_text(encoding="utf-8")
                assert 'version = 14' in text and 'self_intersections_auto_update = false' in text
                ctl("scene", "open", saved)
                objects = ctl("objects")["objects"]
                analysis = next(item["id"] for item in objects if item["kind"] == "analysis")
                source = next(item["id"] for item in objects if item["kind"] == "file")
                assert findings()["status"] == "not_checked"
                assert run()["count"] == 1
                ctl("analysis", "set", analysis, "--auto-update-self-intersections", "true")
                ctl("analysis", "add", analysis, "--side", "b", "--object", source)
                assert complete("bToA")["count"] == 1
                ctl("analysis", "clear", analysis, "--side", "a")
                assert complete("bToA")["count"] == 1
                ctl("analysis", "set", analysis, "--visible", "false")
                assert complete("bToA")["count"] == 1
                print("Self-intersection viewer smoke passed: Run/Update, stale coverage, cancellation, auto-update, persistence, A/B, hidden results, and screenshots.")
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

"""Exercise fin findings in the real viewer; optionally save screenshots.

python tests/ctl_fins_smoke.py build/vs2026-vcpkg/bin/Debug/woby.exe build/fin-qa
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
    instance = "fins-" + uuid.uuid4().hex[:12]
    with tempfile.TemporaryDirectory(prefix="woby-fins-") as temporary:
        root = Path(temporary).resolve()
        model = root / "cases.obj"
        model.write_text(
            "o cases\nv 0 0 0\nv 4 0 0\nv 0 4 0\nv 0 0 4\nv 2 -3 0\n"
            "f 1 3 2\nf 1 2 4\nf 2 3 4\nf 3 1 4\nf 1 2 5\n", encoding="utf-8")
        scene = root / "detectors.woby"
        scene.write_text(
            'version = 15\nshow_grid = false\nshow_origin = false\n'
            '[[files]]\npath = "cases.obj"\n[[files.groups]]\nname = "cases"\n'
            'show_vertices = false\nshow_triangles = false\n'
            '[[analyses]]\nname = "Fin candidates QA"\nanalysis_enabled = true\n'
            'analysis_mode = "a"\ndiagnostic_category = "fins"\n'
            'analysis_fins_enabled = false\nanalysis_show_fins = true\n'
            'analysis_show_boundaries = false\nanalysis_show_non_manifold = false\n'
            'analysis_show_winding = false\nanalysis_show_holes = false\n'
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
                    return ctl("analysis", "results", analysis)[side]["detectors"]["fins"]

                def run(side="aToB"):
                    ctl("analysis", "run", analysis, "--detector", "fins")
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
                assert initial["count"] == 1 and initial["heuristic"], initial
                patch = initial["findings"][0]
                assert [f["triangleId"] for f in patch["faces"]] == [5], patch
                assert patch["area"] == patch["denominatorArea"] == 6, patch
                assert patch["areaRatio"] == 1 and patch["boundaryKind"] == "open", patch
                if output:
                    ctl("camera", "view", "isometric")
                    ctl("camera", "frame", "--object", analysis)
                    ctl("screenshot", output / "fins.png")
                ctl("analysis", "set", analysis, "--show-fins", "false")
                assert findings() == initial
                if output:
                    ctl("screenshot", output / "hidden.png")
                ctl("analysis", "set", analysis, "--show-fins", "true", "--fin-max-area-ratio", ".5")
                stale = findings()
                assert stale["status"] == "out_of_date" and stale["knownCount"] == 1 and not stale["findings"], stale
                assert run()["count"] == 0
                ctl("analysis", "set", analysis, "--fins", "true", "--fin-max-area-ratio", "1")
                assert complete()["count"] == 1
                ctl("analysis", "set", analysis, "--fins", "false")
                ctl("transform", "set", source, "--translation", "1", "0", "0")
                assert findings()["status"] == "out_of_date"
                assert run()["count"] == 1
                ctl("analysis", "set", analysis, "--topology-mode", "exact_position")
                assert findings()["status"] == "out_of_date"
                assert run()["count"] == 1
                ctl("analysis", "cancel", analysis, "--detector", "fins")
                assert findings()["status"] == "canceled"
                assert run()["count"] == 1
                saved = root / "saved.woby"
                ctl("scene", "save-as", saved, "--overwrite")
                text = saved.read_text(encoding="utf-8")
                assert 'version = 15' in text and 'analysis_fins_enabled = false' in text
                assert 'analysis_fin_max_area_ratio = 1' in text
                ctl("scene", "open", saved)
                objects = ctl("objects")["objects"]
                analysis = next(item["id"] for item in objects if item["kind"] == "analysis")
                source = next(item["id"] for item in objects if item["kind"] == "file")
                assert findings()["status"] == "not_checked"
                assert run()["count"] == 1
                ctl("analysis", "set", analysis, "--fins", "true")
                ctl("analysis", "add", analysis, "--side", "b", "--object", source)
                assert complete("bToA")["count"] == 1
                ctl("analysis", "clear", analysis, "--side", "a")
                assert complete("bToA")["count"] == 1
                ctl("analysis", "set", analysis, "--visible", "false")
                assert complete("bToA")["count"] == 1
                print("Fin candidate viewer smoke passed: manual runs, visibility, thresholds, stale results, cancellation, auto-update, persistence, A/B, and screenshots.")
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

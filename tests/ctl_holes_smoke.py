"""Exercise non-manifold vertices and hole loops in the real viewer, including rendered overlays.

python tests/ctl_holes_smoke.py build/vs2026-vcpkg/bin/Debug/woby.exe build/holes-qa
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
    instance = "topology-" + uuid.uuid4().hex[:12]
    with tempfile.TemporaryDirectory(prefix="woby-topology-") as temporary:
        root = Path(temporary).resolve()
        model = root / "cases.obj"
        model.write_text(
            "o cases\nv -4 -4 0\nv 4 -4 0\nv 4 4 0\nv -4 4 0\n"
            "v -1 -1 0\nv 1 -1 0\nv 1 1 0\nv -1 1 0\n"
            "v 8 0 0\nv 10 0 0\nv 8 2 0\nv 6 0 0\nv 8 -2 0\n"
            "f 1 2 6\nf 1 6 5\nf 2 3 7\nf 2 7 6\nf 3 4 8\nf 3 8 7\nf 4 1 5\nf 4 5 8\n"
            "f 9 10 11\nf 9 12 13\n", encoding="utf-8")
        other = root / "other.obj"
        other.write_text(model.read_text(encoding="utf-8"), encoding="utf-8")
        scene = root / "topology.woby"
        scene.write_text(
            'version = 11\nshow_grid = false\nshow_origin = false\n'
            '[[files]]\npath = "cases.obj"\n[[files.groups]]\nname = "cases"\n'
            'show_vertices = false\nshow_triangles = false\n'
            '[[analyses]]\nname = "Vertex and hole QA"\nanalysis_enabled = true\n'
            'analysis_mode = "a"\ndiagnostic_category = "holes"\n'
            'analysis_show_boundaries = false\nanalysis_show_winding = false\nanalysis_show_non_manifold = false\n'
            'show_degenerate_triangles = false\n'
            'show_duplicate_points = false\nshow_duplicate_triangles = false\n'
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

                def result(side="aToB"):
                    return ctl("analysis", "results", analysis)[side]

                initial = result()
                vertices = initial["detectors"]["non_manifold_vertices"]
                assert vertices["count"] == 1 and vertices["findings"][0]["linkComponents"] == 2, vertices
                assert vertices["findings"][0]["pointReferences"][0]["pointId"] == 9
                assert initial["detectors"]["holes"]["count"] == 0
                ctl("analysis", "set", analysis, "--hole-size-ratio-tolerance", ".25")
                filtered = result()
                holes = filtered["detectors"]["holes"]
                assert holes["count"] == 1 and holes["findings"][0]["sizeRatio"] == .25, holes
                assert holes["loopCount"] == 4 and holes["findings"][0]["edgeCount"] == 4
                assert filtered["diagnostics"] == initial["diagnostics"]
                ctl("analysis", "set", analysis, "--show-holes", "false", "--show-non-manifold-vertices", "false")
                assert result() == filtered
                ctl("analysis", "set", analysis, "--show-holes", "true", "--show-non-manifold-vertices", "true")
                if output:
                    ctl("camera", "view", "top")
                    ctl("camera", "frame", "--object", analysis)
                    ctl("screenshot", output / "holes-and-vertices.png")
                ctl("analysis", "set", analysis, "--holes", "false", "--non-manifold-vertices", "false")
                disabled = result()["detectors"]
                assert disabled["holes"]["status"] == "disabled" and disabled["holes"]["count"] is None
                assert disabled["non_manifold_vertices"]["status"] == "disabled"
                ctl("analysis", "set", analysis, "--holes", "true", "--non-manifold-vertices", "true")
                assert result() == filtered
                saved = root / "saved.woby"
                ctl("scene", "save-as", saved, "--overwrite")
                text = saved.read_text(encoding="utf-8")
                assert 'version = 15' in text and 'analysis_hole_size_ratio_tolerance = 0.25' in text
                ctl("scene", "open", saved)
                objects = ctl("objects")["objects"]
                analysis = next(item["id"] for item in objects if item["kind"] == "analysis")
                assert result()["detectors"]["holes"]["count"] == 1
                first_source = next(item["id"] for item in objects if item["kind"] == "file")
                ctl("analysis", "add", analysis, "--side", "b", "--object", first_source)
                assert result("bToA")["detectors"]["holes"]["count"] == 1
                ctl("analysis", "set", analysis, "--visible", "false", "--hole-size-ratio-tolerance", "1")
                assert result("bToA")["detectors"]["holes"]["count"] == 4
                ctl("analysis", "set", analysis, "--topology-mode", "exact_position")
                assert result()["detectors"]["non_manifold_vertices"]["count"] == 1
                print("Vertex and hole viewer smoke passed: source references, thresholds, toggles, overlays, persistence, A/B and hidden results.")
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

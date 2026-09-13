"""Exercise source topology in the real viewer, including rendered overlays.

python tests/ctl_topology_smoke.py build/vs2026-vcpkg/bin/Debug/woby.exe build/topology-qa
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
            "o cases\nv 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 0 0\nv 1 1 0\nv 0 1 0\n"
            "v 3 0 0\nv 4 0 0\nv 4 1 0\nv 3 1 0\n"
            "v 6 0 0\nv 7 0 0\nv 6 1 0\nv 6 -1 0\nv 6 .5 1\n"
            "f 1 2 3\nf 4 5 6\nf 7 8 9\nf 7 10 9\nf 11 12 13\nf 12 11 14\nf 11 12 15\n",
            encoding="utf-8")
        other = root / "other.obj"
        other.write_text(model.read_text(encoding="utf-8"), encoding="utf-8")
        scene = root / "topology.woby"
        scene.write_text(
            'version = 10\nshow_grid = false\nshow_origin = false\n'
            '[[files]]\npath = "cases.obj"\n[[files.groups]]\nname = "cases"\n'
            'show_vertices = false\nshow_triangles = false\n'
            '[[analyses]]\nname = "Topology QA"\nanalysis_enabled = true\n'
            'analysis_mode = "a"\ndiagnostic_category = "winding"\n'
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
                assert initial["detectors"]["boundary_edges"]["count"] == 16, initial
                nonmanifold = initial["detectors"]["non_manifold_edges"]
                assert nonmanifold["count"] == 1 and nonmanifold["findings"][0]["incidentFaceCount"] == 3
                winding = initial["detectors"]["inconsistently_oriented_tris"]
                assert winding["count"] == 2 and winding["findingCount"] == 1
                assert [face["triangleId"] for face in winding["findings"][0]["incidentFaces"]] == [3, 4]
                ctl("analysis", "set", analysis, "--topology-mode", "exact_position")
                exact = result()
                assert exact["detectors"]["boundary_edges"]["count"] == 14
                assert exact["diagnostics"] == initial["diagnostics"]
                assert exact["detectors"]["degenerate_tris"] == initial["detectors"]["degenerate_tris"]
                ctl("analysis", "set", analysis, "--show-non-manifold", "false", "--show-winding", "true")
                assert result() == exact
                if output:
                    ctl("camera", "view", "top")
                    ctl("camera", "frame", "--object", analysis)
                    ctl("screenshot", output / "winding-only.png")
                    ctl("analysis", "set", analysis, "--show-winding", "false", "--show-non-manifold", "true")
                    ctl("screenshot", output / "non-manifold-only.png")
                    ctl("analysis", "set", analysis, "--show-winding", "true", "--show-non-manifold", "false")
                saved = root / "saved.woby"
                ctl("scene", "save-as", saved, "--overwrite")
                text = saved.read_text(encoding="utf-8")
                assert 'version = 10' in text and 'topology_mode = "exact_position"' in text
                assert 'analysis_show_winding = true' in text and 'analysis_show_non_manifold = false' in text
                ctl("scene", "open", saved)
                objects = ctl("objects")["objects"]
                analysis = next(item["id"] for item in objects if item["kind"] == "analysis")
                assert result()["detectors"]["boundary_edges"]["count"] == 14
                first_source = next(item["id"] for item in objects if item["kind"] == "file")
                ctl("model", "add", other)
                second_source = next(item["id"] for item in ctl("objects")["objects"]
                                     if item["kind"] == "file" and item["id"] != first_source)
                ctl("analysis", "add", analysis, "--side", "a", "--object", second_source)
                combined = result()["detectors"]
                assert combined["boundary_edges"]["count"] == 28
                assert combined["non_manifold_edges"]["count"] == 2
                assert combined["inconsistently_oriented_tris"]["count"] == 4
                ctl("analysis", "add", analysis, "--side", "b", "--object", second_source)
                assert result("bToA")["detectors"]["boundary_edges"]["count"] == 14
                ctl("analysis", "set", analysis, "--visible", "false", "--topology-mode", "original_index")
                assert result("bToA")["detectors"]["boundary_edges"]["count"] == 16
                print("Topology viewer smoke passed: modes, source isolation, face references, independent overlays, scene persistence, A/B and hidden results.")
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

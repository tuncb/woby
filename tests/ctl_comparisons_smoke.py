"""Exercise comparison rendering, persistence, and invalidation in a real viewer.

uv run tests/ctl_comparisons_smoke.py build/vs2026-vcpkg/bin/Debug/woby.exe \
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
            # Build and measure a comparison entirely through the CLI/server.
            create_args = ("comparison", "create", "--name", "CLI comparison",
                           "--a", files[0]["id"], "--b", files[1]["id"], "--request-key", "create-cli")
            created = ctl(*create_args)
            third = created["target"]
            assert ctl(*create_args) == created  # Retries must not create duplicates.
            assert ctl("scene", "info")["comparisonCount"] == 3
            assert created["object"]["valid"]
            configured = ctl("comparison", "set", third, "--visible", "false", "--mode", "distance",
                             "--tolerance", ".01", "--color-range", ".2", "--distance-on-a", "true",
                             "--show-edges", "true", "--show-boundaries", "false", "--show-non-manifold", "false")
            assert configured["object"]["settings"]["distanceOnA"]
            metrics = ctl("comparison", "results", third, "--request-key", "metrics-before")
            for direction in ("aToB", "bToA"):
                assert abs(metrics[direction]["maximum"] - .12) < 1e-5, metrics
                assert 0 < metrics[direction]["mean"] <= metrics[direction]["maximum"]
                assert metrics[direction]["diagnostics"]["boundaryEdges"] == 0
            quality_settings = ctl("comparison", "set", third, "--mode", "surface_quality",
                                   "--quality-metric", "shape", "--quality-on-a", "true",
                                   "--quality-minimum-enabled", "true", "--quality-minimum-size", ".5",
                                   "--quality-maximum-enabled", "true", "--quality-maximum-size", "1.45")
            assert quality_settings["object"]["settings"]["qualityMetric"] == "shape"
            assert quality_settings["object"]["settings"]["mode"] == "surface_quality"
            quality_results = ctl("comparison", "results", third)
            assert quality_results["aToB"]["surfaceMeshQuality"]["shape"]["count"] == 12
            assert abs(quality_results["aToB"]["surfaceMeshQuality"]["size_jump"]["maximum"] - 1) < 1e-6
            ctl("scene", "save-as", root / "quality-settings.woby", "--overwrite")
            saved_quality = (root / "quality-settings.woby").read_text(encoding="utf-8")
            assert 'comparison_mode = "surface_quality"' in saved_quality
            assert 'quality_metric = "shape"' in saved_quality
            # A source change requires fresh results, even while the comparison is hidden.
            ctl("transform", "set", files[1]["id"], "--translation", "0", "0", ".25")
            moved = ctl("comparison", "results", third)
            assert moved["bToA"]["maximum"] > metrics["bToA"]["maximum"] + .2
            assert ctl("comparison", "results", third, "--request-key", "metrics-before") == metrics
            ctl("transform", "reset", files[1]["id"])
            ctl("comparison", "remove", third, "--side", "a", "--object", files[0]["id"])
            single = ctl("comparison", "results", third)
            assert single["aToB"] is None and single["bToA"]["maximum"] is None
            assert single["bToA"]["triangleCount"] == 12
            ctl("comparison", "swap", third)
            assert ctl("object", third)["object"]["bPartCount"] == 0
            ctl("comparison", "add", third, "--side", "b", "--object", files[0]["id"])
            ctl("comparison", "add", third, "--side", "b", "--object", files[0]["id"])
            assert ctl("object", third)["object"]["bPartCount"] == 1
            ctl("comparison", "clear", third, "--side", "a")
            ctl("comparison", "add", third, "--side", "a", "--object", files[1]["id"])
            ctl("comparison", "set", third, "--name", "Renamed via CLI", "--mode", "overlay")
            cli_saved = root / "cli-created.woby"
            ctl("scene", "save-as", cli_saved, "--overwrite")
            assert 'name = "Renamed via CLI"' in cli_saved.read_text(encoding="utf-8")
            ctl("comparison", "delete", third)
            ctl("comparison", "results", third, success=False)
            ctl("comparison", "create", "--a", third, "--b", files[0]["id"], success=False)
            ctl("comparison", "delete", files[0]["id"], success=False)
            assert ctl("scene", "info")["comparisonCount"] == 2
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
            ctl("scene", "open", cli_saved)
            restored = next(item for item in ctl("objects")["objects"] if item["name"] == "Renamed via CLI")
            restored_settings = ctl("object", restored["id"])["object"]["settings"]
            assert restored_settings["mode"] == "overlay" and not restored_settings["visible"]
            assert restored_settings["showEdges"] and not restored_settings["showBoundaries"]
            ctl("comparison", "results", restored["id"])
            # A single open mesh renders immediately, including boundary diagnostics,
            # even when the saved mode normally requires both inputs.
            open_mesh = (root / "cube-0.obj").read_text(encoding="utf-8").splitlines()
            (root / "open.obj").write_text("\n".join(open_mesh[:-2]) + "\n", encoding="utf-8")
            single_scene = root / "single.woby"
            single_scene.write_text('version = 5\nshow_grid = false\nshow_origin = false\n'
                                    '[[files]]\npath = "open.obj"\n', encoding="utf-8")
            ctl("scene", "open", single_scene)
            single_source = next(item for item in ctl("objects")["objects"] if item["kind"] == "file")
            inspected = ctl("comparison", "create", "--name", "Single mesh", "--a", single_source["id"])
            single_id = inspected["target"]
            assert inspected["object"]["valid"] and inspected["object"]["bPartCount"] == 0
            ctl("visibility", "set", single_source["id"], "--visible", "false")
            ctl("comparison", "set", single_id, "--show-edges", "true")
            ctl("camera", "frame")
            ctl("screenshot", root / "single-a.png")
            metrics = ctl("comparison", "results", single_id)
            assert metrics["aToB"]["diagnostics"]["boundaryEdges"] == 4
            assert metrics["aToB"]["sampleCount"] == 0 and metrics["aToB"]["maximum"] is None
            assert metrics["bToA"] is None
            ctl("comparison", "swap", single_id)
            ctl("comparison", "set", single_id, "--mode", "overlay")
            ctl("screenshot", root / "single-b.png")
            metrics = ctl("comparison", "results", single_id)
            assert metrics["aToB"] is None and metrics["bToA"]["diagnostics"]["boundaryEdges"] == 4
            ctl("scene", "save-as", root / "single-saved.woby", "--overwrite")
            ctl("scene", "open", root / "single-saved.woby")
            single_id = next(item["id"] for item in ctl("objects")["objects"] if item["kind"] == "comparison")
            assert ctl("object", single_id)["object"]["valid"]
            ctl("screenshot", root / "single-reopened.png")
            for metric in ("longest_edge", "equivalent_size", "shape", "size_jump"):
                ctl("comparison", "set", single_id, "--mode", "surface_quality", "--quality-metric", metric,
                    "--quality-on-a", "true", "--quality-maximum-enabled", "true", "--quality-maximum-size", "1")
                ctl("screenshot", root / f"quality-{metric}.png")
                assert (root / f"quality-{metric}.png").stat().st_size > 10000
            ctl("scene", "save-as", root / "quality-single.woby", "--overwrite")
            ctl("scene", "open", root / "quality-single.woby")
            single_id = next(item["id"] for item in ctl("objects")["objects"] if item["kind"] == "comparison")
            assert ctl("object", single_id)["object"]["settings"]["qualityMetric"] == "size_jump"
            ctl("screenshot", root / "quality-reopened.png")
            ctl("comparison", "clear", single_id, "--side", "b")
            ctl("comparison", "results", single_id, success=False)
            ctl("screenshot", root / "empty.png", success=False)
            print("Comparison viewer smoke test passed: single-input inspection, CLI creation, settings, membership, metrics, retries, deletion, rendering, persistence, and invalidation.")
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

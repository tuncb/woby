"""Exercise difference CLI jobs in a headless viewer with synthetic models.

uv run tests/ctl_difference_smoke.py build/vs2026-vcpkg/bin/Release/woby.exe
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
    instance = "difference-" + uuid.uuid4().hex[:12]
    env = dict(os.environ, SDL_VIDEO_DRIVER="woby-test-no-video-driver")
    env.setdefault("VK_LOADER_LAYERS_DISABLE", "~implicit~")
    with tempfile.TemporaryDirectory(prefix="woby-difference-cli-") as directory:
        root = Path(directory).resolve()
        vertices = ("v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n"
                    "v 0 0 1\nv 1 0 1\nv 1 1 1\nv 0 1 1\n")
        faces = ("1 3 2", "1 4 3", "5 6 7", "5 7 8", "1 2 6", "1 6 5",
                 "2 3 7", "2 7 6", "3 4 8", "3 8 7", "4 1 5", "4 5 8")
        closed = root / "closed.obj"
        closed.write_text(vertices + "".join(f"f {face}\n" for face in faces), encoding="utf-8")
        closed_b = root / "closed-b.obj"
        closed_b.write_bytes(closed.read_bytes())
        defective = root / "defective.obj"
        defective.write_text(vertices + "".join(f"f {face}\n" for face in faces[1:]), encoding="utf-8")
        log_path = root / "viewer.log"
        with log_path.open("w", encoding="utf-8") as log:
            viewer = subprocess.Popen([str(executable), "--headless", "--instance", instance],
                                      cwd=root, env=env, stdout=log, stderr=log)

            def arguments(*args):
                return [str(executable), "ctl", "--instance", instance, *map(str, args), "--json"]

            def ctl(*args, success=True, parse_error=False):
                result = subprocess.run(arguments(*args), cwd=root, env=env, capture_output=True,
                                        text=True, encoding="utf-8", timeout=75)
                if parse_error:
                    assert not success and result.returncode != 0 and result.stderr.strip(), (args, result)
                    assert not result.stdout.strip(), result.stdout
                    return {"error": result.stderr}
                assert result.stdout.strip(), (args, result.returncode, result.stderr)
                value = json.loads(result.stdout)
                assert (result.returncode == 0) == success, (args, value, result.stderr)
                return value

            try:
                deadline = time.monotonic() + 45
                while True:
                    assert viewer.poll() is None, log_path.read_text(encoding="utf-8")
                    found = subprocess.run([str(executable), "ctl", "instances", "--json"],
                                           env=env, capture_output=True, text=True, timeout=10)
                    if any(item["id"] == instance and item["ready"] for item in json.loads(found.stdout)):
                        break
                    assert time.monotonic() < deadline, "Viewer startup timed out"
                    time.sleep(.025)
                a = ctl("model", "add", closed)["addedIds"][0]
                b = ctl("model", "add", closed_b)["addedIds"][0]
                ctl("transform", "set", b, "--translation", ".000001", "0", "0")
                analysis = ctl("analysis", "create", "--type", "difference_mesh", "--a", a, "--b", b,
                               "--alignment", "scene", "--lock-placement", "false")["target"]

                def difference(action, *args, **kwargs):
                    return ctl("analysis", "difference-" + action, analysis, *args, **kwargs)

                for action in ("create", "rebuild", "set"):
                    difference(action, "--method", "approximate_mesh", success=False, parse_error=True)
                    difference(action, "--cell-size", "1", success=False, parse_error=True)
                difference("check-resolution", success=False, parse_error=True)
                difference("cancel-check", success=False, parse_error=True)
                difference("set", "--show-walls", "false", success=False, parse_error=True)

                details = ctl("object", analysis)["object"]
                assert details["settings"]["type"] == "difference_mesh", details
                ctl("analysis", "set", analysis, "--mode", "distance", success=False)
                ctl("analysis", "run", analysis, "--detector", "boundary_edges", success=False)
                result = difference("create", "--wait")["difference"]
                assert result["current"] and result["kind"] == "approximate_mesh", result
                assert abs(result["volumeA"] - 1e-6) < 1e-9 and abs(result["volumeB"] - 1e-6) < 1e-9, result
                assert len(result["triangles"]) == 2, result
                for key in ("method", "cellSize", "showWalls", "closureTriangles", "resolutionCheck"):
                    assert key not in result and key not in result["settings"], result
                assert ctl("analysis", "results", analysis)["difference"]["current"]
                difference("set", "--visible", "false", "--show-a-only", "false", "--opacity", ".4")
                assert not ctl("object", analysis)["object"]["settings"]["visible"]
                ctl("visibility", "set", analysis, "--visible", "true")
                assert difference("get")["difference"]["settings"]["visible"]
                displayed = difference("get")["difference"]
                assert displayed["current"] and displayed["volumeA"] == result["volumeA"], displayed
                exported = root / "difference.obj"
                difference("export", "--path", exported)
                contents = exported.read_text(encoding="utf-8")
                assert "g A_only" in contents and "g B_only" in contents
                difference("export", "--path", exported, success=False)
                assert exported.read_text(encoding="utf-8") == contents

                ctl("transform", "set", b, "--translation", ".5", "0", "0")
                outdated = difference("get")["difference"]
                assert not outdated["current"] and outdated["totalVolume"] is None, outdated
                difference("export", "--path", root / "stale.obj", success=False)
                assert not (root / "stale.obj").exists()
                rebuilt = difference("rebuild")["difference"]
                assert rebuilt["current"] and abs(rebuilt["totalVolume"] - 1) < 1e-9, rebuilt
                saved = root / "saved.woby"
                ctl("scene", "save-as", saved)
                saved_text = saved.read_text(encoding="utf-8")
                assert 'analysis_type = "difference_mesh"' in saved_text
                for key in ("difference_method", "difference_cell_size", "difference_show_walls", "difference_visible"):
                    assert key not in saved_text, key
                ctl("scene", "open", saved)
                analysis = next(item["id"] for item in ctl("objects")["objects"] if item["kind"] == "analysis")
                restored = difference("rebuild")["difference"]
                assert restored["current"] and restored["kind"] == "approximate_mesh", restored
                assert restored["totalVolume"] == rebuilt["totalVolume"], restored
                assert not restored["settings"]["showAOnly"], restored
                assert abs(restored["settings"]["opacity"] - .4) < 1e-6, restored
                difference("cancel")
                canceled = difference("get")["difference"]
                assert canceled["state"] == "canceled" and not canceled["current"], canceled
                difference("rebuild")
                difference("delete")
                assert difference("get")["difference"]["state"] == "not_created"
                print("Thin change, display, export, stale result, reload, cancellation, and deletion passed.", flush=True)

                # Surface comparison remains a separate analysis and rejects difference commands.
                surface = ctl("analysis", "create", "--type", "surface_comparison")["target"]
                ctl("analysis", "difference-create", surface, success=False)
                ctl("analysis", "difference-get", surface, success=False)
                ctl("analysis", "delete", surface)

                # Open topology fails with an input-specific error; sources stay unchanged.
                objects = ctl("objects")["objects"]
                a = next(item["id"] for item in objects if item["kind"] == "file")
                bad = ctl("model", "add", defective)["addedIds"][0]
                before = defective.read_bytes()
                analysis = ctl("analysis", "create", "--type", "difference_mesh", "--a", a, "--b", bad,
                               "--alignment", "scene", "--lock-placement", "false")["target"]
                failure = difference("create", success=False)
                assert "Input B:" in failure["error"] and "closed solids" in failure["error"], failure
                assert difference("get")["difference"]["state"] == "failed"
                assert defective.read_bytes() == before
                ctl("quit", "--on-dirty", "discard")
                viewer.wait(timeout=15)
            except BaseException:
                print(log_path.read_text(encoding="utf-8"), file=sys.stderr)
                raise
            finally:
                if viewer.poll() is None:
                    viewer.kill()
                viewer.wait(timeout=15)
    print("Difference CLI passed: single approximate method, lifecycle, persistence, and open-input errors.")


if __name__ == "__main__":
    main()

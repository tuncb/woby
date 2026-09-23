# /// script
# requires-python = ">=3.11"
# dependencies = ["Pillow>=11"]
# ///
"""Exercise windowless rendering, automation, overlays, and process lifecycle.

Requires a working D3D11 (Windows) or Vulkan (Linux) device/driver, not a desktop.
uv run tests/ctl_headless_smoke.py path/to/woby
"""
from contextlib import contextmanager
import ctypes
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import uuid

from PIL import Image, ImageChops


def assert_no_windows(pid):
    if os.name != "nt":
        return
    from ctypes import wintypes
    user32 = ctypes.WinDLL("user32", use_last_error=True)
    callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    user32.EnumWindows.argtypes = [callback_type, wintypes.LPARAM]
    user32.EnumWindows.restype = wintypes.BOOL
    user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    user32.GetWindowThreadProcessId.restype = wintypes.DWORD
    found = []

    @callback_type
    def visit(window, _):
        owner = wintypes.DWORD()
        user32.GetWindowThreadProcessId(window, ctypes.byref(owner))
        if owner.value == pid:
            found.append(window)
        return True

    assert user32.EnumWindows(visit, 0)
    assert not found, f"Headless viewer created native windows: {found}"


def run(executable, *args, **kwargs):
    return subprocess.run([str(executable), *map(str, args)], capture_output=True,
                          text=True, timeout=75, **kwargs)


@contextmanager
def session(executable, root, env, *args):
    instance = "headless-" + uuid.uuid4().hex[:12]
    log_path = root / (instance + ".log")
    with log_path.open("w", encoding="utf-8") as log:
        viewer = subprocess.Popen([str(executable), "--headless", "--instance", instance,
                                   *map(str, args)], cwd=root, env=env, stdout=log, stderr=log)

        def ctl(*command, code=None):
            result = run(executable, "ctl", "--instance", instance, *command, "--json", cwd=root, env=env)
            assert result.returncode == (0 if code is None else 1), (command, result.stdout, result.stderr)
            value = json.loads(result.stdout)
            if code is not None:
                assert value["code"] == code, (command, value)
            return value

        try:
            deadline = time.monotonic() + 45
            while True:
                assert viewer.poll() is None, log_path.read_text(encoding="utf-8")
                discovered = run(executable, "ctl", "instances", "--json", env=env)
                assert discovered.returncode == 0, discovered.stderr
                ready = next((item for item in json.loads(discovered.stdout)
                              if item["id"] == instance and item["ready"]), None)
                if ready:
                    assert ready["headless"] is True
                    break
                assert time.monotonic() < deadline, "Headless startup timed out"
                time.sleep(0.025)
            assert_no_windows(viewer.pid)
            yield ctl, viewer
        except BaseException:
            print(log_path.read_text(encoding="utf-8"), file=sys.stderr)
            raise
        finally:
            if viewer.poll() is None:
                viewer.kill()
            viewer.wait(timeout=15)
    discovered = run(executable, "ctl", "instances", "--json", env=env)
    assert all(item["id"] != instance for item in json.loads(discovered.stdout))


def capture(ctl, path):
    response = ctl("screenshot", path)
    assert response["state"] == "succeeded", response
    with Image.open(response["path"]) as source:
        assert source.format == "PNG" and source.size == (1920, 1800)
        return source.convert("RGB")


def main():
    executable = Path(sys.argv[1]).resolve()
    # A missing SDL video driver makes accidental video initialization fail even
    # on a developer desktop. Also remove the Linux display-server environment.
    env = dict(os.environ, SDL_VIDEO_DRIVER="woby-test-no-video-driver")
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    with tempfile.TemporaryDirectory(prefix="woby headless ") as directory:
        root = Path(directory)
        model = root / "surface.obj"
        model.write_text("o surface\nv -1 -1 0\nv 1 -1 0\nv 1 1 0\nv -1 1 0\nf 1 2 3\nf 1 3 4\n", encoding="utf-8")
        second = root / "other.obj"
        second.write_bytes(model.read_bytes())
        scene = root / "review.woby"

        help_result = run(executable, "--headless", "--help", env=env)
        assert help_result.returncode == 0, help_result.stderr
        for text in ("--headless", "ready=true", "camera frame", "screenshot", "GPU readback", "not a ctl option",
                     "analysis findings", "--collection", "--revision", "analysis export-status",
                     "analysis export-cancel", "--intersection-pair-limit", "--intersection-candidate-limit", "0 explicitly means unlimited"):
            assert text in help_result.stdout, text
        invalid = run(executable, "--headless", "--unknown-option", env=env)
        assert invalid.returncode == 1 and invalid.stderr and not invalid.stdout
        missing = run(executable, "--headless", "--scene", root / "missing.woby", cwd=root, env=env)
        assert missing.returncode == 1 and missing.stderr

        with session(executable, root, env) as (ctl, viewer):
            # The first command is a capture: no warm-up UI frames or desktop.
            capture(ctl, root / "empty.png")
            status = ctl("status")
            assert status["headless"] and status["pane"] is None
            assert status["renderer"] in ("Direct3D 11", "Vulkan")
            assert status["screenshot"] == {"width": 1920, "height": 1800, "format": "png"}
            methods = {item["method"]: item for item in ctl("capabilities")["methods"]}
            assert methods["screenshot.capture"]["available"]
            assert not methods["pane.set"]["available"]
            ctl("pane", "set", "--visible", "false", code=-32602)
            before = ctl("performance", "get")["frameIndex"]
            time.sleep(0.3)
            assert ctl("performance", "get")["frameIndex"] - before < 40, "Idle loop is spinning"

            ctl("grid", "set", "--visible", "false")
            loaded = ctl("model", "add", model)
            assert loaded["addedCount"] == 1, loaded
            file_id = loaded["addedIds"][0]
            group = next(item["id"] for item in ctl("objects")["objects"] if item["kind"] == "group")
            ctl("color", "set", group, "--rgb", 1, 0, 0)
            ctl("up-axis", "set", "y")
            ctl("camera", "look-at", "--eye", 0, 0, 4, "--target", 0, 0, 0)
            baseline = capture(ctl, root / "nested" / "overview.png")
            red, green, blue = baseline.split()
            assert ImageChops.subtract(red, ImageChops.lighter(green, blue)).getbbox(), "Missing model pixels"
            saved_camera = ctl("camera", "get")["camera"]
            moved = ctl("camera", "orbit", "--yaw-degrees", 25, "--request-key", "orbit-once")
            assert ctl("camera", "orbit", "--yaw-degrees", 25, "--request-key", "orbit-once") == moved
            assert ctl("camera", "get")["camera"] == moved["camera"]
            assert ImageChops.difference(baseline, capture(ctl, root / "rotated.png")).getbbox()
            ctl("camera", "look-at", "--eye", 0, 0, 4, "--target", 0, 0, 0)
            assert ctl("camera", "get")["camera"] == saved_camera

            annotation = ctl("annotation", "create", group, "--shape", "line", "--start", -.2, 0,
                             "--end", .2, 0, "--name", "Review range")["target"]
            ctl("annotation", "set", annotation, "--rgb", 0, 1, 1, "--width", 5, "--comments", "Inspect this surface")
            marked = capture(ctl, root / "marked.png")
            assert ImageChops.difference(baseline, marked).getbbox(), "Missing annotation pixels"
            ctl("grid", "set", "--visible", "true")
            grid = capture(ctl, root / "grid.png")
            assert ImageChops.difference(marked, grid).getbbox(), "Missing grid/scale overlay"
            ctl("scene", "undo")
            assert not ctl("scene", "info")["showGrid"]
            ctl("scene", "redo")
            assert ctl("scene", "info")["showGrid"]

            other_id = ctl("model", "add", second)["addedIds"][0]
            ctl("transform", "set", other_id, "--translation", 0, 0, .2)
            analysis_id = ctl("analysis", "create", "--name", "Headless comparison", "--a", file_id, "--b", other_id)["target"]
            ctl("camera", "view", "isometric")
            ctl("camera", "frame")
            # Capture waits for computations and uploads; no analysis.results warm-up.
            annotated = capture(ctl, root / "analysis.png")
            repeat = capture(ctl, root / "analysis-repeat.png")
            assert ImageChops.difference(annotated, repeat).getbbox() is None
            panel = annotated.crop((int(annotated.width * .58), 0, annotated.width, annotated.height))
            red, green, blue = panel.split()
            light = ImageChops.darker(red, ImageChops.darker(green, blue))
            assert sum(light.histogram()[151:]) > 300, "Missing export text"
            page = ctl("analysis", "findings", analysis_id, "--side", "a", "--detector", "boundary_edges", "--limit", 2)
            assert page["total"] == 4 and len(page["items"]) == 2 and page["nextOffset"] == 2
            tail = ctl("analysis", "findings", analysis_id, "--side", "a", "--detector", "boundary_edges",
                       "--offset", page["nextOffset"], "--revision", page["revision"])
            assert len(tail["items"]) == 2 and tail["nextOffset"] is None
            nested = ctl("analysis", "findings", analysis_id, "--side", "a", "--detector", "boundary_edges",
                         "--collection", "/findings/3/endpoints/0/sourcePoints", "--revision", page["revision"])
            assert nested["total"] >= 1 and nested["items"][0]["pointId"] > 0
            ctl("analysis", "findings", analysis_id, "--side", "a", "--detector", "boundary_edges", "--revision", "stale", code=-32602)
            exported = root / "full diagnostics.json"
            ctl("analysis", "export", analysis_id, "--path", exported.name)
            deadline = time.monotonic() + 15
            while (job := ctl("analysis", "export-status")["export"])["state"] == "running":
                assert time.monotonic() < deadline
                time.sleep(.025)
            assert job["state"] == "complete", job
            full = json.loads(exported.read_text(encoding="utf-8"))
            assert full["allRetainedResults"] and not full["detectionComplete"]  # manual intersections unchecked
            assert len(full["aToB"]["detectors"]["boundary_edges"]["findings"]) == 4
            assert not full["aToB"]["detectors"]["boundary_edges"]["findingsTruncated"]
            assert ctl("analysis", "export-cancel")["export"]["state"] == "complete"
            ctl("analysis", "export", analysis_id, "--path", exported, code=-32004)
            ctl("quit", code=-32011)
            assert viewer.poll() is None
            ctl("scene", "save-as", scene)
            final_camera = ctl("camera", "get")["camera"]

            # Capture failures must release the queue, allowing queries and shutdown.
            blocker = root / "not-a-directory"
            blocker.write_text("block output directory", encoding="utf-8")
            ctl("screenshot", blocker / "failed.png", code=-32004)
            assert not ctl("status")["capturing"]
            assert ImageChops.difference(annotated, capture(ctl, root / "recovered.png")).getbbox() is None
            assert_no_windows(viewer.pid)
            ctl("quit")
            assert viewer.wait(timeout=15) == 0

        with session(executable, root, env, "--scene", scene) as (ctl, viewer):
            # First-frame fonts and restored scene/camera must also work on launch.
            restored = capture(ctl, root / "restored.png")
            difference = ImageChops.difference(annotated, restored)
            scene_width = annotated.width - int(annotated.width * .43)
            assert difference.crop((0, 0, scene_width, annotated.height - 60)).getbbox() is None
            # Font atlases can be packed differently in a fresh process. Permit
            # tiny glyph-edge sampling differences, not missing/changed labels.
            red, green, blue = difference.split()
            error = ImageChops.lighter(red, ImageChops.lighter(green, blue))
            assert sum(error.histogram()[1:]) <= 32 and error.getextrema()[1] <= 8
            assert ctl("camera", "get")["camera"] == final_camera
            ctl("scene", "new")
            assert ctl("scene", "info")["fileCount"] == 0
            ctl("scene", "open", scene)
            assert ctl("camera", "get")["camera"] == final_camera
            ctl("quit")
            assert viewer.wait(timeout=15) == 0
        assert not (root / "imgui.ini").exists(), "Headless viewer wrote desktop preferences"
    print("Headless smoke passed: no SDL video/windows, real pixels, camera/retries, annotations/labels, analysis readiness, persistence/history, errors, idle pacing, and shutdown.")


if __name__ == "__main__":
    main()

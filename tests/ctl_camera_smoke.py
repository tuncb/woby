# /// script
# requires-python = ">=3.11"
# dependencies = ["Pillow>=11"]
# ///
"""Verify camera commands against actual scene pixels and saved scene reloads.

uv run tests/ctl_camera_smoke.py build/vs2026-vcpkg/bin/Debug/woby.exe
"""
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import uuid

from PIL import Image, ImageChops


def main():
    executable = Path(sys.argv[1]).resolve()
    instance = "camera-" + uuid.uuid4().hex[:12]
    with tempfile.TemporaryDirectory(prefix="woby-camera-") as directory:
        root = Path(directory)
        model = root / "triangle.obj"
        model.write_text("o triangle\nv 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n", encoding="utf-8")
        startup = None
        if os.name == "nt":
            startup = subprocess.STARTUPINFO()
            startup.dwFlags = subprocess.STARTF_USESHOWWINDOW
            startup.wShowWindow = 0
        with (root / "viewer.log").open("w", encoding="utf-8") as log:
            viewer = subprocess.Popen([str(executable), "--instance", instance], cwd=root,
                                      stdout=log, stderr=log, startupinfo=startup)

            def ctl(*args, code=None):
                process = subprocess.run([str(executable), "ctl", "--instance", instance,
                                          *map(str, args), "--json"], cwd=root,
                                         capture_output=True, text=True, timeout=75)
                result = json.loads(process.stdout)
                assert process.returncode == (0 if code is None else 1), (args, result)
                if code is not None:
                    assert result["code"] == code, result
                return result

            def capture(name):
                before = ctl("camera", "get")["camera"]
                path = root / (name + ".png")
                ctl("screenshot", path)
                assert ctl("camera", "get")["camera"] == before
                with Image.open(path) as png:
                    assert png.format == "PNG"
                    return png.convert("RGB")

            def red_bounds(picture):
                red, green, blue = picture.split()
                mask = ImageChops.subtract(red, ImageChops.lighter(green, blue)).point(
                    lambda value: 255 if value > 30 else 0)
                bounds = mask.getbbox()
                assert bounds is not None, "Screenshot contains no red scene geometry"
                return bounds, mask.histogram()[255]

            try:
                deadline = time.monotonic() + 45
                while True:
                    assert viewer.poll() is None, (root / "viewer.log").read_text()
                    discovered = subprocess.run([str(executable), "ctl", "instances", "--json"],
                                                capture_output=True, text=True, timeout=5)
                    if any(item["id"] == instance and item["ready"] for item in json.loads(discovered.stdout)):
                        break
                    assert time.monotonic() < deadline, "viewer did not become ready"
                    time.sleep(0.05)
                methods = {entry["method"] for entry in ctl("capabilities")["methods"]}
                assert {"camera.set", "camera.look-at", "camera.frame", "screenshot.capture"} <= methods
                file_id = ctl("model", "add", model)["addedIds"][0]
                group_id = next(item["id"] for item in ctl("objects")["objects"] if item["kind"] == "group")
                ctl("render", "set", "scene", "--solid", "true", "--triangles", "false", "--vertices", "false")
                ctl("color", "set", group_id, "--rgb", "1", "0", "0")
                for control in ("grid", "origin", "dimensions"):
                    ctl(control, "set", "--visible", "false")
                ctl("up-axis", "set", "y")
                ctl("camera", "set", "--target", 0.5, 0.5, 0, "--yaw-degrees", -90,
                    "--pitch-degrees", 0, "--roll-degrees", 0, "--distance", 3,
                    "--fov-degrees", 50, "--near-plane", 0.01)
                baseline = capture("baseline")
                original_bounds, original_area = red_bounds(baseline)
                ctl("camera", "set", "--distance", 1.5)
                zoom_bounds, zoom_area = red_bounds(capture("zoom"))
                assert zoom_area > original_area * 3, (zoom_area, original_area)
                assert zoom_bounds[2] - zoom_bounds[0] > original_bounds[2] - original_bounds[0]
                # With Y-up and yaw -90, camera-right points along world -X.
                ctl("camera", "set", "--distance", 3, "--target", 0, 0.5, 0)
                pan_bounds, _ = red_bounds(capture("pan"))
                assert pan_bounds[0] < original_bounds[0] - 50, (pan_bounds, original_bounds)
                ctl("camera", "look-at", "--eye", 0.5, 0.5, 3, "--target", 0.5, 0.5, 0)
                restored = capture("look-at")
                assert ImageChops.difference(baseline, restored).getbbox() is None
                ctl("camera", "set", "--roll-degrees", 90)
                rolled = capture("roll")
                assert ImageChops.difference(baseline, rolled).getbbox() is not None
                red_bounds(rolled)
                ctl("camera", "look-at", "--eye", 2, 2, 3, "--target", 0.5, 0.5, 0)
                oblique = capture("oblique")
                assert ImageChops.difference(rolled, oblique).getbbox() is not None
                red_bounds(oblique)
                camera = ctl("camera", "get")["camera"]
                for actual, expected in zip(camera["eye"], [2, 2, 3]):
                    assert math.isclose(actual, expected, abs_tol=0.00001)
                ctl("scene", "save-as", root / "camera.woby")
                ctl("camera", "set", "--distance", 10)
                assert not ctl("status")["dirty"]
                ctl("scene", "open", root / "camera.woby")
                assert ctl("camera", "get")["camera"] == camera
                assert ImageChops.difference(oblique, capture("reloaded")).getbbox() is None
                # IDs change on scene load; query them again before framing.
                file_id = next(item["id"] for item in ctl("objects")["objects"] if item["kind"] == "file")
                ctl("transform", "set", file_id, "--translation", 10, 20, 30)
                framed = ctl("camera", "frame", "--object", file_id)["camera"]
                assert framed["target"] == [10.5, 20.5, 30], framed
                red_bounds(capture("object"))
                ctl("visibility", "set", file_id, "--visible", "false")
                # Visibility changes scene bounds and therefore the derived far plane.
                framed = ctl("camera", "get")["camera"]
                ctl("camera", "frame", "--object", file_id, code=-32602)
                assert ctl("camera", "get")["camera"] == framed
                ctl("camera", "look-at", "--eye", 0, 0, 0, "--target", 0, 0, 0, code=-32602)
                assert ctl("camera", "get")["camera"] == framed
                ctl("quit", "--on-dirty", "discard")
                assert viewer.wait(timeout=15) == 0
                print("Camera smoke passed: visible scene pixels, zoom, pan, roll, look-at, exact restored captures, persistence, object framing, and errors.")
            finally:
                if viewer.poll() is None:
                    viewer.kill()
                    viewer.wait(timeout=10)


if __name__ == "__main__":
    main()

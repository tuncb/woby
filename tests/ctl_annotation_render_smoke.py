# /// script
# requires-python = ">=3.11"
# dependencies = ["Pillow>=11"]
# ///
"""Check rendered annotation alignment on large models with Y-up and Z-up.

uv run tests/ctl_annotation_render_smoke.py build/vs2026-vcpkg/bin/Debug/woby.exe
"""
import json
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
    instance = "annotation-render-" + uuid.uuid4().hex[:12]
    with tempfile.TemporaryDirectory(prefix="woby-annotation-render-") as directory:
        root = Path(directory)
        startup = None
        if os.name == "nt":
            startup = subprocess.STARTUPINFO()
            startup.dwFlags = subprocess.STARTF_USESHOWWINDOW
            startup.wShowWindow = 0
        with (root / "viewer.log").open("w", encoding="utf-8") as log:
            viewer = subprocess.Popen([str(executable), "--instance", instance], cwd=root,
                                      stdout=log, stderr=log, startupinfo=startup)

            def ctl(*args):
                process = subprocess.run([str(executable), "ctl", "--instance", instance,
                                          *map(str, args), "--json"], cwd=root,
                                         capture_output=True, text=True, timeout=75)
                assert process.returncode == 0, (args, process.stdout, process.stderr)
                return json.loads(process.stdout)

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
                for control in ("grid", "origin", "dimensions"):
                    ctl(control, "set", "--visible", "false")
                for up in ("y", "z"):
                    model = root / (up + ".obj")
                    points = [(-40000, -40000), (40000, -40000), (40000, 40000), (-40000, 40000)]
                    vertices = [(x + 20000, 2000, y + 10000) if up == "y" else
                                (x + 20000, y + 10000, 2000) for x, y in points]
                    model.write_text("o terrain\n" + "".join(f"v {x} {y} {z}\n" for x, y, z in vertices)
                                     + "f 1 2 3\nf 1 3 4\n", encoding="utf-8")
                    file_id = ctl("model", "add", model)["addedIds"][0]
                    group_id = next(item["id"] for item in ctl("objects")["objects"] if item["kind"] == "group")
                    ctl("up-axis", "set", up)
                    ctl("render", "set", "scene", "--solid", "true", "--triangles", "false", "--vertices", "false")
                    ctl("color", "set", group_id, "--rgb", ".5", ".3", ".1")
                    target = (20000, 2000, 10000) if up == "y" else (20000, 10000, 2000)
                    ctl("camera", "set", "--target", *target, "--yaw-degrees", -60,
                        "--pitch-degrees", 50, "--distance", 200000, "--near-plane", .1)
                    for shape in ("line", "rectangle"):
                        start, end = (-.2, -.12), (.2, .12)
                        created = ctl("annotation", "create", group_id, "--shape", shape,
                                      "--start", *start, "--end", *end, "--aspect", 1920 / 1800)
                        annotation = created["target"]
                        ctl("annotation", "set", annotation, "--rgb", 0, 1, 1, "--width", 5)
                        for near in (.1, 10, 100):
                            ctl("camera", "set", "--near-plane", near)
                            path = root / f"{up}-{shape}-{near}.png"
                            ctl("screenshot", path)
                            with Image.open(path) as source:
                                red, green, blue = source.convert("RGB").split()
                                mask = ImageChops.multiply(red.point(lambda v: 255 if v < 80 else 0),
                                    ImageChops.multiply(green.point(lambda v: 255 if v > 180 else 0),
                                                        blue.point(lambda v: 255 if v > 180 else 0)))
                                controls = [start, end] if shape == "line" else [start, (end[0], start[1]), end, (start[0], end[1]), start]
                                for a, b in zip(controls, controls[1:]):
                                    for step in range(17):
                                        t = step / 16
                                        x = round((1 + a[0] + t * (b[0] - a[0])) * source.width / 2)
                                        y = round((1 - a[1] - t * (b[1] - a[1])) * source.height / 2)
                                        assert mask.crop((x-4, y-4, x+5, y+5)).getbbox(), (
                                            up, shape, near, "stroke misses its projected control/edge", (x, y), mask.getbbox())
                        ctl("annotation", "delete", annotation)
                        ctl("camera", "set", "--near-plane", .1)
                    ctl("model", "remove", file_id)
                ctl("quit", "--on-dirty", "discard")
                assert viewer.wait(timeout=15) == 0
                print("Annotation rendering passed: large translated terrain, both up axes, lines/rectangles, and three near planes.")
            finally:
                if viewer.poll() is None:
                    viewer.kill()
                    viewer.wait(timeout=10)


if __name__ == "__main__":
    main()

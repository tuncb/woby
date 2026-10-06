# /// script
# requires-python = ">=3.11"
# dependencies = ["Pillow>=11"]
# ///
"""Check weighted transparency through real model imports and PNG exports."""
import argparse
import os
from pathlib import Path
import shutil
import tempfile

from PIL import Image, ImageChops

from ctl_headless_smoke import session


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("executable", type=Path)
    parser.add_argument("--artifacts", type=Path)
    args = parser.parse_args()
    executable = args.executable.resolve()
    with tempfile.TemporaryDirectory(prefix="woby transparency ") as directory:
        root = Path(directory).resolve()
        for name, depths in (("red", (-.6, .6)), ("blue", (.6, -.6))):
            (root / (name + ".obj")).write_text(
                f"v -1 -1 {depths[0]}\nv 1 -1 {depths[1]}\n"
                f"v 1 1 {depths[1]}\nv -1 1 {depths[0]}\n"
                "g sheet\nf 1 2 3\nf 1 3 4\n", encoding="utf-8")
        (root / "point.obj").write_text("v 0 0 -1\np 1\n", encoding="utf-8")
        env = dict(os.environ, SDL_VIDEO_DRIVER="woby-test-no-video-driver")
        with session(executable, root, env) as (ctl, _):
            for control in ("grid", "origin", "dimensions"):
                ctl(control, "set", "--visible", "false")
            ctl("up-axis", "set", "y")

            def capture(name):
                path = root / (name + ".png")
                ctl("screenshot", path)
                if args.artifacts:
                    destination = args.artifacts.resolve()
                    destination.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(path, destination / path.name)
                with Image.open(path) as image:
                    return image.convert("RGB")

            def add_sheet(name):
                identity = ctl("model", "add", root / (name + ".obj"))["addedIds"][0]
                ctl("render", "set", identity, "--solid", "true", "--triangles", "false", "--vertices", "false")
                ctl("color", "set", identity, "--rgb", *((1, 0, 0) if name == "red" else (0, 0, 1)))
                ctl("opacity", "set", identity, "--value", .4 if name == "red" else .55)
                return identity

            files = [add_sheet(name) for name in ("red", "blue")]
            ctl("camera", "look-at", "--eye", 0, 0, 4, "--target", 0, 0, 0)
            original = capture("red-blue")
            for identity in files:
                ctl("model", "remove", identity)
            files = [add_sheet(name) for name in ("blue", "red")]
            ctl("camera", "look-at", "--eye", 0, 0, 4, "--target", 0, 0, 0)
            swapped = capture("blue-red")
            difference = ImageChops.difference(original, swapped)
            assert max(high for _, high in difference.getextrema()) <= 1, difference.getextrema()
            colors = original.getcolors(original.width * original.height)
            assert sum(count for count, rgb in colors if abs(rgb[0]-rgb[2]) > 20) > 1000, colors
            left = original.getpixel((int(original.width*.4), original.height//2))
            right = original.getpixel((int(original.width*.6), original.height//2))
            assert left[0]*right[2] > right[0]*left[2]*1.1, (left, right)

            point = ctl("model", "add", root / "point.obj")["addedIds"][0]
            ctl("color", "set", point, "--rgb", 0, 1, 0)
            ctl("render", "set", point, "--vertices", "true")
            ctl("vertex-size", "set", "scene", "--pixels", 24)
            ctl("camera", "look-at", "--eye", 0, 0, 4, "--target", 0, 0, 0)
            mixed = capture("prominent-point")
            green = [(x, y) for y in range(mixed.height) for x in range(mixed.width)
                     if mixed.getpixel((x, y)) == (0, 255, 0)]
            assert len(green) > 200, len(green)
            for identity in files:
                ctl("opacity", "set", identity, "--value", 0)
            point_only = capture("point-only")
            assert all(point_only.getpixel(pixel) == mixed.getpixel(pixel) for pixel in green)
            for identity in files:
                ctl("opacity", "set", identity, "--value", 1)
            opaque = capture("opaque-occlusion")
            assert all(opaque.getpixel(pixel) != (0, 255, 0) for pixel in green)
        print("PNG exports preserve surface ordering, prominent opaque points, and opaque occlusion")


if __name__ == "__main__":
    main()

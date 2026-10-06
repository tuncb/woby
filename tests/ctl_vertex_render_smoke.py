# /// script
# requires-python = ">=3.11"
# dependencies = ["Pillow>=11"]
# ///
"""Check production shader-generated markers through the real viewer/export path."""
import os
from pathlib import Path
import sys
import tempfile

from PIL import Image, ImageChops

from ctl_headless_smoke import session


def components(picture):
    red, green, blue = picture.split()
    mask = ImageChops.subtract(red, ImageChops.lighter(green, blue)).point(
        lambda value: 255 if value > 30 else 0)
    box = mask.getbbox()
    if box is None:
        return []
    crop = mask.crop(box)
    pixels = crop.load()
    pending = {(x, y) for y in range(crop.height) for x in range(crop.width) if pixels[x, y]}
    result = []
    while pending:
        stack = [pending.pop()]
        found = []
        while stack:
            x, y = stack.pop()
            found.append((x, y))
            for neighbor in ((x-1, y), (x+1, y), (x, y-1), (x, y+1)):
                if neighbor in pending:
                    pending.remove(neighbor)
                    stack.append(neighbor)
        xs, ys = zip(*found)
        result.append((len(found), max(xs)-min(xs)+1, max(ys)-min(ys)+1))
    return sorted(result)


def main():
    executable = Path(sys.argv[1]).resolve()
    env = dict(os.environ, SDL_VIDEO_DRIVER="woby-test-no-video-driver")
    with tempfile.TemporaryDirectory(prefix="woby vertex render ") as directory:
        root = Path(directory).resolve()
        model = root / "groups.obj"
        model.write_text("""v -1 -.5 0
v -.4 -.5 0
v -.7 .5 0
v .4 -.5 0
v 1 -.5 0
v .7 .5 0
g left
f 1 2 3
f 1 2 3
g right
f 4 5 6
""", encoding="utf-8")
        saved = root / "markers.woby"
        with session(executable, root, env) as (ctl, viewer):
            file_id = ctl("model", "add", model)["addedIds"][0]
            groups = [item["id"] for item in ctl("objects")["objects"] if item["kind"] == "group"]
            assert len(groups) == 2
            for control in ("grid", "origin", "dimensions"):
                ctl(control, "set", "--visible", "false")
            ctl("up-axis", "set", "y")
            ctl("camera", "look-at", "--eye", 0, 0, 4, "--target", 0, 0, 0)
            ctl("render", "set", "scene", "--solid", "false", "--triangles", "false", "--vertices", "true")
            for group in groups:
                ctl("color", "set", group, "--rgb", 1, 0, 0)

            def capture(name):
                path = root / (name + ".png")
                ctl("screenshot", path)
                with Image.open(path) as image:
                    return image.convert("RGB")

            areas = []
            for size in (4, 8, 40):
                ctl("vertex-size", "set", "scene", "--pixels", size)
                circles = components(capture(f"size-{size}"))
                assert len(circles) == 6, (size, circles)
                for area, width, height in circles:
                    assert abs(width-size) <= 1 and abs(height-size) <= 1, (size, circles)
                    # A circle fills roughly pi/4 of its bounding square.
                    assert .5 * size * size <= area <= .9 * (size+1) ** 2, (size, circles)
                areas.append(sum(circle[0] for circle in circles))
            assert areas[1] > 3 * areas[0] and areas[2] > 5 * areas[1], areas
            opaque = capture("opaque")
            ctl("opacity", "set", file_id, "--value", .5)
            assert ImageChops.difference(opaque, capture("half-opacity")).getbbox() is None
            ctl("opacity", "set", file_id, "--value", 0)
            assert ImageChops.difference(opaque, capture("zero-opacity")).getbbox() is None
            ctl("opacity", "set", file_id, "--value", 1)
            ctl("render", "set", groups[0], "--vertices", "false")
            assert len(components(capture("second-group"))) == 3
            ctl("render", "set", groups[0], "--vertices", "true")
            assert ImageChops.difference(opaque, capture("reenabled")).getbbox() is None
            ctl("render", "set", "scene", "--solid", "true")
            assert sum(item[0] for item in components(capture("solid"))) > areas[-1] * 10
            ctl("render", "set", "scene", "--solid", "false")
            ctl("transform", "set", file_id, "--translation", .2, .1, 0)
            ctl("opacity", "set", file_id, "--value", 0)
            transformed = capture("transformed")
            assert len(components(transformed)) == 6
            assert ImageChops.difference(opaque, transformed).getbbox() is not None
            ctl("scene", "save-as", saved)
            ctl("quit")
            assert viewer.wait(timeout=15) == 0
        with session(executable, root, env, "--scene", saved) as (ctl, viewer):
            path = root / "restored.png"
            ctl("screenshot", path)
            with Image.open(path) as image:
                assert ImageChops.difference(transformed, image.convert("RGB")).getbbox() is None
            ctl("quit")
            assert viewer.wait(timeout=15) == 0
    print("Vertex render smoke passed: circles, group offsets, transparency, solid coexistence, reuse, transforms, and reload.")


if __name__ == "__main__":
    main()

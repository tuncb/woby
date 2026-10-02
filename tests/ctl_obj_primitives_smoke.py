# /// script
# requires-python = ">=3.11"
# dependencies = ["Pillow>=11"]
# ///
"""Verify OBJ point/line rendering, controls, and saved-scene restoration."""
import os
from pathlib import Path
import sys
import tempfile

from PIL import ImageChops

from ctl_headless_smoke import capture, session
from ctl_vertex_render_smoke import components


def exercise(executable, root, kind):
    model = root / f"{kind}.obj"
    vertices = "v -.6 -.5 0\nv .6 -.5 0\nv -.8 .5 0\nv 0 .8 0\nv .8 .5 0\n"
    primitives = {"points": "g samples\np 1 2\n",
                  "lines": "g wire\nl 3 4 5\n",
                  "mixed": "g surface\nf 1 2 4\ng wire\nl 3 4 5\ng samples\np 1 2\n"}
    model.write_text(vertices + primitives[kind], encoding="utf-8")
    saved = root / f"{kind}.woby"
    env = dict(os.environ, SDL_VIDEO_DRIVER="woby-test-no-video-driver")
    with session(executable, root, env, "--file", model) as (ctl, viewer):
        ids = {item["name"]: item["id"] for item in ctl("objects")["objects"] if item["kind"] == "group"}
        for helper in ("grid", "origin", "dimensions"):
            ctl(helper, "set", "--visible", "false")
        ctl("up-axis", "set", "y")
        ctl("camera", "look-at", "--eye", 0, 0, 4, "--target", 0, 0, 0)
        if "surface" in ids:
            ctl("color", "set", ids["surface"], "--rgb", 0, 0, 1)
        if "wire" in ids:
            ctl("color", "set", ids["wire"], "--rgb", 0, 1, 0)
            ctl("render", "set", ids["wire"], "--line-width", 6)
        if "samples" in ids:
            ctl("color", "set", ids["samples"], "--rgb", 1, 0, 0)
            ctl("vertex-size", "set", "scene", "--pixels", 12)
            assert ctl("object", ids["samples"])["object"]["primitive"] == "points"
        picture = capture(ctl, root / f"{kind}-initial.png")
        if "samples" in ids:
            circles = components(picture)
            assert len(circles) == 2, (kind, circles)
            assert all(10 <= width <= 13 and 10 <= height <= 13 for _, width, height in circles), circles
            ctl("render", "set", ids["samples"], "--vertices", "false")
            assert components(capture(ctl, root / f"{kind}-hidden.png")) == []
            ctl("scene", "undo")
            assert ImageChops.difference(picture, capture(ctl, root / f"{kind}-undo.png")).getbbox() is None
        if "wire" in ids:
            green = sum(n for n, (r, g, b) in picture.getcolors(picture.width * picture.height)
                        if g > r + 100 and g > b + 100)
            assert green > 500, (kind, green)
            assert ctl("stats")["lineSegmentCount"] == 2
        assert ctl("stats")["pointCount"] == (2 if "samples" in ids else 0)
        ctl("scene", "save-as", saved)
        ctl("quit")
        assert viewer.wait(timeout=15) == 0
    with session(executable, root, env, "--scene", saved) as (ctl, viewer):
        restored = capture(ctl, root / f"{kind}-restored.png")
        assert ImageChops.difference(picture, restored).getbbox() is None, kind
        ctl("quit")
        assert viewer.wait(timeout=15) == 0


def main():
    executable = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="woby obj primitives ") as directory:
        root = Path(directory).resolve()
        for kind in ("points", "lines", "mixed"):
            exercise(executable, root, kind)
    print("OBJ primitive render smoke passed: points, polylines, mixed geometry, visibility, undo, and reload.")


if __name__ == "__main__":
    main()

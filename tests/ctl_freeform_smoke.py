# /// script
# requires-python = ">=3.11"
# dependencies = ["Pillow>=11"]
# ///
"""Exercise freeform rendering and scene restoration through the real viewer."""
import os
from pathlib import Path
import sys
import tempfile
from PIL import ImageChops
from ctl_headless_smoke import capture, session


def trimmed_scene(executable, root):
    model = root / "trimmed.obj"
    # A rational circle in parameter space, over a curved Bezier panel. The
    # equally sized green panel behind it is visible only through the hole.
    model.write_text(
        "vp .75 .5 1\nvp .75 .75 .7071067811865476\nvp .5 .75 1\n"
        "vp .25 .75 .7071067811865476\nvp .25 .5 1\nvp .25 .25 .7071067811865476\n"
        "vp .5 .25 1\nvp .75 .25 .7071067811865476\n"
        "cstype rat bspline\ndeg 2\ncurv2 1 2 3 4 5 6 7 8 1\n"
        "parm u 0 0 0 1 1 2 2 3 3 4 4 4\nend\n"
        + "".join(f"v {x} {y} {0.5 if x == 0 and y == 0 else .2}\n"
                  for y in (-.7, 0, .7) for x in (-.7, 0, .7))
        + "g trimmed\ncstype bezier\ndeg 2 2\nsurf 0 1 0 1 1 2 3 4 5 6 7 8 9\n"
        "parm u 0 1\nparm v 0 1\nhole 4 0 -1\nend\n"
        "v -.7 -.7 0\nv .7 -.7 0\nv .7 .7 0\nv -.7 .7 0\ng behind\nf 10 11 12 13\n",
        encoding="utf-8")
    saved = root / "trimmed.woby"
    env = dict(os.environ, SDL_VIDEO_DRIVER="woby-test-no-video-driver")
    with session(executable, root, env, "--file", model) as (ctl, viewer):
        ids = {item["name"]: item["id"] for item in ctl("objects")["objects"] if item["kind"] == "group"}
        assert set(ids) == {"trimmed", "behind"}, ids
        for helper in ("grid", "origin", "dimensions"):
            ctl(helper, "set", "--visible", "false")
        ctl("up-axis", "set", "y")
        ctl("camera", "look-at", "--eye", 0, 0, 4, "--target", 0, 0, 0)
        ctl("color", "set", ids["trimmed"], "--rgb", 1, 0, 0)
        ctl("color", "set", ids["behind"], "--rgb", 0, 1, 0)
        picture = capture(ctl, root / "trimmed.png")
        colors = picture.getcolors(picture.width * picture.height)
        assert sum(n for n, (r, g, b) in colors if r > g + 80 and r > b + 80) > 10000
        assert sum(n for n, (r, g, b) in colors if g > r + 80 and g > b + 80) > 5000
        assert ctl("stats")["lineSegmentCount"] == 0
        ctl("color", "set", ids["behind"], "--rgb", 0, 0, 1)
        changed = capture(ctl, root / "trimmed-changed.png")
        assert ImageChops.difference(picture, changed).getbbox() is not None
        ctl("scene", "undo")
        assert ImageChops.difference(picture, capture(ctl, root / "trimmed-undo.png")).getbbox() is None
        ctl("scene", "save-as", saved)
        ctl("quit")
        assert viewer.wait(timeout=15) == 0
    with session(executable, root, env, "--scene", saved) as (ctl, viewer):
        assert ImageChops.difference(picture, capture(ctl, root / "trimmed-restored.png")).getbbox() is None
        ctl("quit")
        assert viewer.wait(timeout=15) == 0


def main():
    executable = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="woby freeform ") as directory:
        root = Path(directory).resolve()
        model = root / "freeform.obj"
        controls = "".join(f"v {x} {y} {0.5 if x == 0 and y == 0 else 0}\n"
                           for y in (-0.4, 0, 0.4) for x in (-0.7, 0, 0.7))
        model.write_text(controls + "v -.7 .55 0\nv 0 1.1 0\nv .7 .55 0\n"
                         "g patch\ncstype bezier\ndeg 2 2\n"
                         "surf 0 1 0 1 1 2 3 4 5 6 7 8 9\nparm u 0 1\nparm v 0 1\nend\n"
                         "g arc\ndeg 2\ncurv 0 1 10 11 12\nparm u 0 1\nend\n", encoding="utf-8")
        saved = root / "freeform.woby"
        env = dict(os.environ, SDL_VIDEO_DRIVER="woby-test-no-video-driver")
        with session(executable, root, env, "--file", model) as (ctl, viewer):
            ids = {item["name"]: item["id"] for item in ctl("objects")["objects"] if item["kind"] == "group"}
            assert set(ids) == {"patch", "arc"}, ids
            for helper in ("grid", "origin", "dimensions"):
                ctl(helper, "set", "--visible", "false")
            ctl("up-axis", "set", "y")
            ctl("camera", "look-at", "--eye", 0, 0, 4, "--target", 0, 0, 0)
            ctl("color", "set", ids["patch"], "--rgb", 1, 0, 0)
            ctl("color", "set", ids["arc"], "--rgb", 0, 1, 0)
            ctl("render", "set", ids["arc"], "--line-width", 6)
            picture = capture(ctl, root / "initial.png")
            colors = picture.getcolors(picture.width * picture.height)
            assert sum(n for n, (r, g, b) in colors if r > g + 80 and r > b + 80) > 1000
            assert sum(n for n, (r, g, b) in colors if g > r + 80 and g > b + 80) > 500
            assert ctl("stats")["lineSegmentCount"] == 32
            assert ctl("stats")["pointCount"] == 0
            ctl("scene", "save-as", saved)
            ctl("quit")
            assert viewer.wait(timeout=15) == 0
        with session(executable, root, env, "--scene", saved) as (ctl, viewer):
            restored = capture(ctl, root / "restored.png")
            assert ImageChops.difference(picture, restored).getbbox() is None
            ctl("quit")
            assert viewer.wait(timeout=15) == 0
        trimmed_scene(executable, root)
    print("Freeform render smoke passed: surfaces, curves, rational trimming, undo, and scene reload.")


if __name__ == "__main__":
    main()

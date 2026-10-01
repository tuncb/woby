# /// script
# requires-python = ">=3.11"
# dependencies = ["Pillow>=11"]
# ///
"""Verify imported line pixels, width, depth, hierarchy, undo and scene reload."""
import os
from pathlib import Path
import sys
import tempfile

from PIL import ImageChops

from ctl_headless_smoke import capture, session


def colors(image):
    pixels = image.getcolors(image.width * image.height)
    return (sum(n for n, (r, g, b) in pixels if g > r + 100 and g > b + 100),
            sum(n for n, (r, g, b) in pixels if r > g + 100 and r > b + 100))


def exercise(executable, plugin, root):
    root.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, SDL_VIDEO_DRIVER="woby-test-no-video-driver")
    model = root / "mixed.wline"
    model.write_text("fixture", encoding="utf-8")
    saved = root / "lines.woby"
    with session(executable, root, env, "--plugin", plugin, "--file", model) as (ctl, viewer):
        objects = ctl("objects")["objects"]
        ids = {item["name"]: item["id"] for item in objects}
        rear, front = ids["Rear curve"], ids["Front curve"]
        for helper in ("grid", "origin", "dimensions"):
            ctl(helper, "set", "--visible", "false")
        ctl("up-axis", "set", "y")
        ctl("camera", "look-at", "--eye", 0, 0, 6, "--target", 0, 0, 0)
        baseline = capture(ctl, root / "01-depth-tested.png")
        green, red = colors(baseline)
        assert green > 100 and red > 100, (green, red)
        ctl("render", "set", rear, "--line-depth-test", "false")
        on_top = capture(ctl, root / "02-on-top.png")
        assert colors(on_top)[0] > green * 1.8, (colors(on_top), (green, red))
        ctl("render", "set", ids["Boundary curves"], "--line-width", 8)
        wide = capture(ctl, root / "03-wide.png")
        assert all(a > b * 2.5 for a, b in zip(colors(wide), colors(on_top))), (colors(wide), colors(on_top))
        ctl("scene", "undo")
        assert ImageChops.difference(on_top, capture(ctl, root / "undo.png")).getbbox() is None
        ctl("scene", "redo")
        assert ImageChops.difference(wide, capture(ctl, root / "redo.png")).getbbox() is None
        ctl("visibility", "set", ids["Boundary curves"], "--visible", "false")
        assert colors(capture(ctl, root / "hidden.png")) == (0, 0)
        ctl("visibility", "set", ids["Boundary curves"], "--visible", "true")
        ctl("render", "set", front, "--line-width", 3)
        final = capture(ctl, root / "04-child-override.png")
        ctl("scene", "save-as", saved, "--overwrite")
        ctl("quit")
        assert viewer.wait(timeout=15) == 0
    with session(executable, root, env, "--plugin", plugin, "--scene", saved) as (ctl, viewer):
        assert ImageChops.difference(final, capture(ctl, root / "restored.png")).getbbox() is None
        ctl("quit")
        assert viewer.wait(timeout=15) == 0
    only = root / "lineonly.wline"
    only.write_text("fixture", encoding="utf-8")
    with session(executable, root, env, "--plugin", plugin, "--file", only) as (ctl, viewer):
        for helper in ("grid", "origin", "dimensions"):
            ctl(helper, "set", "--visible", "false")
        ctl("up-axis", "set", "y")
        ctl("camera", "look-at", "--eye", 0, 0, 6, "--target", 0, 0, 0)
        ctl("render", "set", "scene", "--line-width", 5)
        assert all(n > 1000 for n in colors(capture(ctl, root / "05-lines-only.png")))
        ctl("quit", "--on-dirty", "discard")
        assert viewer.wait(timeout=15) == 0


if __name__ == "__main__":
    executable, plugin = (Path(arg).resolve() for arg in sys.argv[1:3])
    with tempfile.TemporaryDirectory(prefix="woby lines ") as directory:
        exercise(executable, plugin, Path(directory).resolve())
    if len(sys.argv) > 3:
        exercise(executable, plugin, Path(sys.argv[3]).resolve())
    print("Imported lines passed: depth, width, hierarchy visibility, child overrides, undo/redo, reload and line-only rendering.")

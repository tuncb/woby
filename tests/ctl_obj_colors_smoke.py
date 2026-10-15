# /// script
# requires-python = ">=3.11"
# dependencies = ["Pillow>=11"]
# ///
"""Render OBJ RGB, manual overrides, source reset, opacity and scene reload."""
import os
from pathlib import Path
import sys
import tempfile

from PIL import ImageChops
from ctl_headless_smoke import capture, session


def count(image, predicate):
    return sum(n for n, rgb in image.getcolors(image.width * image.height) if predicate(*rgb))


def exercise(executable, root, primitive):
    model = root / f"colors-{primitive}.obj"
    # Every primitive has an authored green part, a red-to-blue gradient, and
    # an uncolored part. Part colors deliberately differ from authored colors.
    lines = []
    for group, x, colors in (("painted", -1.2, ["0 1 0"] * 4),
                             ("gradient", 0, ["1 0 0", "0 0 1", "0 0 1", "1 0 0"]),
                             ("fallback", 1.2, [""] * 4)):
        first = len([line for line in lines if line.startswith("v ")]) + 1
        for (dx, y), color in zip(((-.45, -.5), (.45, -.5), (.45, .5), (-.45, .5)), colors):
            lines.append(f"v {x + dx} {y} 0 {color}")
        lines.append(f"g {group}")
        ids = " ".join(str(first + i) for i in range(4))
        lines.append(f"{primitive} {ids}")
    model.write_text("\n".join(lines) + "\n", encoding="utf-8")
    saved = root / f"colors-{primitive}.woby"
    env = dict(os.environ, SDL_VIDEO_DRIVER="woby-test-no-video-driver")
    with session(executable, root, env, "--file", model) as (ctl, viewer):
        groups = {item["name"]: item["id"] for item in ctl("objects")["objects"] if item["kind"] == "group"}
        for helper in ("grid", "origin", "dimensions"):
            ctl(helper, "set", "--visible", "false")
        ctl("up-axis", "set", "y")
        ctl("camera", "look-at", "--eye", 0, 0, 5, "--target", 0, 0, 0)
        ctl("vertex-size", "set", "scene", "--pixels", 30)
        ctl("color", "set", groups["fallback"], "--rgb", 1, 1, 0)
        for group in groups.values():
            if primitive == "l":
                ctl("render", "set", group, "--line-width", 12)
        picture = capture(ctl, root / f"{primitive}-colors.png")
        minimum = 1000 if primitive == "p" else 4000
        green = lambda r, g, b: g > 120 and r < 10 and b < 10
        yellow = lambda r, g, b: r > 120 and g > 120 and b < 10
        assert count(picture, green) > minimum, primitive
        assert count(picture, yellow) > minimum, primitive
        assert count(picture, lambda r, g, b: r > 120 and g < 10 and b < 50) > 100, primitive
        assert count(picture, lambda r, g, b: b > 120 and g < 10 and r < 50) > 100, primitive
        if primitive != "p":
            gradient = [rgb for _, rgb in picture.getcolors(picture.width * picture.height)
                        if rgb[0] > 30 and rgb[2] > 30 and rgb[1] < 10]
            assert len(gradient) > 30, (primitive, len(gradient))
        for group in groups.values():
            ctl("color", "set", group, "--rgb", 1, 0, 1)
        changed = capture(ctl, root / f"{primitive}-override.png")
        magenta = lambda r, g, b: r > 120 and b > 120 and g < 10
        assert count(changed, green) == 0, "Manual color did not override authored RGB"
        assert count(changed, magenta) > minimum * 2, "Override did not cover the colored parts"
        assert count(changed, yellow) == 0, "Missing RGB did not use the edited part color"
        ctl("color", "reset", groups["painted"])
        reset = capture(ctl, root / f"{primitive}-reset.png")
        assert count(reset, green) == count(picture, green), "Reset did not restore authored RGB"
        assert count(reset, magenta) > minimum, "Reset changed other parts"
        if primitive == "f":
            ctl("render", "set", groups["painted"], "--triangles", "true")
            assert count(capture(ctl, root / "surface-edges.png"), green) > minimum
            ctl("color", "set", groups["painted"], "--rgb", 1, 0, 1)
            assert count(capture(ctl, root / "surface-edges-override.png"), green) == 0
            ctl("render", "set", groups["painted"], "--solid", "false", "--triangles", "false", "--vertices", "true")
            assert count(capture(ctl, root / "surface-points-override.png"), green) == 0
            ctl("color", "reset", groups["painted"])
            assert count(capture(ctl, root / "surface-points-source.png"), green) > 1000
            ctl("render", "set", groups["painted"], "--solid", "true", "--triangles", "true", "--vertices", "false")
            ctl("opacity", "set", groups["painted"], "--value", .5)
            transparent = capture(ctl, root / "surface-opacity.png")
            assert ImageChops.difference(reset, transparent).getbbox() is not None
        expected = capture(ctl, root / f"{primitive}-saved.png")
        ctl("scene", "save-as", saved)
        ctl("quit")
        assert viewer.wait(timeout=15) == 0
    with session(executable, root, env, "--scene", saved) as (ctl, viewer):
        assert ImageChops.difference(expected, capture(ctl, root / f"{primitive}-restored.png")).getbbox() is None
        ctl("quit")
        assert viewer.wait(timeout=15) == 0


def main():
    executable = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="woby vertex colors ") as directory:
        root = Path(directory).resolve()
        for primitive in ("f", "l", "p"):
            exercise(executable, root, primitive)
    print("OBJ colors passed: surfaces, lines, points, gradients, fallback, overrides, source reset, opacity, and reload.")


if __name__ == "__main__":
    main()

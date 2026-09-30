# /// script
# requires-python = ">=3.11"
# dependencies = ["Pillow>=11"]
# ///
"""Check distant rendering, detector distances, annotations, and scene round trips.

uv run tests/ctl_coordinate_origin_smoke.py path/to/woby
"""
import json
import os
from pathlib import Path
import sys
import tempfile
from PIL import ImageChops
from ctl_headless_smoke import session, capture


def model(path, offset, dz=0):
    points = [(-.125, -.125), (.125, -.125), (.125, .125), (-.125, .125)]
    path.write_text("o surface\n" + "".join(
        f"v {offset+x:.17g} {offset+y:.17g} {offset+dz:.17g}\n" for x, y in points
    ) + "f 1 2 3\nf 1 3 4\n", encoding="utf-8")


def main():
    executable = Path(sys.argv[1]).resolve()
    env = dict(os.environ, SDL_VIDEO_DRIVER="woby-test-no-video-driver")
    images = []
    with tempfile.TemporaryDirectory(prefix="woby coordinate origin ") as directory:
        root = Path(directory)
        for offset in (0, 8000000):
            folder = root / str(offset)
            folder.mkdir()
            first, second = folder / "surface.obj", folder / "second.obj"
            model(first, offset)
            model(second, offset, .0625)
            with session(executable, folder, env) as (ctl, viewer):
                ctl("grid", "set", "--visible", "false")
                a = ctl("model", "add", first)["addedIds"][0]
                origin = ctl("scene", "info")["coordinateOrigin"]
                assert origin == [offset, offset, offset], origin
                group = next(v["id"] for v in ctl("objects")["objects"] if v["kind"] == "group")
                ctl("color", "set", group, "--rgb", 1, 0, 0)
                ctl("up-axis", "set", "y")
                ctl("camera", "look-at", "--eye", 0, 0, 1, "--target", 0, 0, 0)
                image = capture(ctl, folder / "surface.png")
                red, green, blue = image.split()
                assert ImageChops.subtract(red, ImageChops.lighter(green, blue)).getbbox(), "Missing surface"
                images.append(image)
                annotation = ctl("annotation", "create", group, "--shape", "line", "--start", -.1, 0,
                                 "--end", .1, 0, "--name", "Precision check")["target"]
                vertices = ctl("annotation", "get", annotation)["object"]["vertices"]
                assert len(vertices) == 2, vertices
                assert abs(vertices[1][0] - vertices[0][0]) > .001, vertices
                assert all(abs(p[0] - offset) < .125 for p in vertices), vertices
                b = ctl("model", "add", second)["addedIds"][0]
                assert ctl("scene", "info")["coordinateOrigin"] == origin
                analysis = ctl("analysis", "create", "--a", a, "--b", b)["target"]
                capture(ctl, folder / "analysis.png")  # waits for detector computation and uploads
                results = ctl("analysis", "results", analysis)
                assert abs(results["aToB"]["mean"] - .0625) < 1e-9, results
                saved = folder / "scene.woby"
                ctl("scene", "save-as", saved)
                camera = ctl("camera", "get")["camera"]
                ctl("scene", "new")
                ctl("scene", "open", saved)
                assert ctl("scene", "info")["coordinateOrigin"] == origin
                assert ctl("camera", "get")["camera"] == camera
                capture(ctl, folder / "restored.png")
                restored = ctl("annotation", "list")["annotations"]
                assert len(restored) == 1 and restored[0]["targetValid"], restored
                assert restored[0]["vertices"] == vertices, restored
                ctl("quit")
                assert viewer.wait(timeout=15) == 0
        difference = ImageChops.difference(*images)
        assert difference.getbbox() is None, "Near-zero and distant model renders differ"
    print(json.dumps({"renderPixelsIdentical": True, "distance": .0625, "originalCoordinates": True,
                      "annotations": True, "sceneRoundTrip": True}))


if __name__ == "__main__":
    main()

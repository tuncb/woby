# /// script
# requires-python = ">=3.11"
# dependencies = ["Pillow>=11"]
# ///
"""Verify deferred point-only uploads on the real renderer and RPC readiness."""
from concurrent.futures import ThreadPoolExecutor
import os
from pathlib import Path
import sys
import tempfile
import time

from PIL import Image
from ctl_headless_smoke import session


def main():
    executable = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="woby post loading ") as directory:
        root = Path(directory).resolve()
        model = root / "points.obj"
        side = 1024  # One complete GPU point chunk; no initial surface buffers.
        with model.open("w", encoding="utf-8") as stream:
            for y in range(side):
                for x in range(side):
                    stream.write(f"v {x / (side - 1) * 2 - 1:.9f} {y / (side - 1) * 2 - 1:.9f} 0\n")
        with session(executable, root, dict(os.environ)) as (ctl, viewer):
            with ThreadPoolExecutor(max_workers=1) as executor:
                loaded = executor.submit(ctl, "model", "add", model)
                deadline = time.monotonic() + 60
                while not loaded.done():
                    status = ctl("status")
                    assert not status["postLoadError"], status
                    if status["postLoading"]:
                        assert not status["postLoadReady"], status
                    assert time.monotonic() < deadline, status
                    time.sleep(.01)
                result = loaded.result()
            assert len(result["addedIds"]) == 1, result
            assert ctl("status")["postLoadReady"]
            for helper in ("grid", "origin", "dimensions"):
                ctl(helper, "set", "--visible", "false")
            ctl("up-axis", "set", "y")
            ctl("camera", "look-at", "--eye", 0, 0, 4, "--target", 0, 0, 0)
            ctl("color", "set", result["addedIds"][0], "--rgb", 1, 0, 0)
            visible = root / "visible.png"
            ctl("screenshot", visible)
            with Image.open(visible) as image:
                colors = image.convert("RGB").getcolors(image.width * image.height)
                red = sum(count for count, (r, g, b) in colors if r > 180 and r > g + 80 and r > b + 80)
                assert red > 10000, red
            ctl("render", "set", "scene", "--vertices", "false")
            hidden = root / "hidden.png"
            ctl("screenshot", hidden)
            with Image.open(hidden) as image:
                colors = image.convert("RGB").getcolors(image.width * image.height)
                red = sum(count for count, (r, g, b) in colors if r > 180 and r > g + 80 and r > b + 80)
                assert red == 0, red
            ctl("quit", "--on-dirty", "discard")
            assert viewer.wait(timeout=15) == 0
    print("Post-load point upload, rendering and readiness passed.")


if __name__ == "__main__":
    main()

# /// script
# requires-python = ">=3.11"
# dependencies = ["Pillow>=11"]
# ///
"""Exercise deferred annotation preparation across import, scene reload and removal."""
import os
from pathlib import Path
import sys
import tempfile

from ctl_headless_smoke import session


def main():
    executable = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="woby-loading-preparation-") as directory:
        root = Path(directory)
        model, scene = root / "surface.obj", root / "scene.woby"
        n = 180  # Above the 50,000-triangle preparation threshold.
        with model.open("w", encoding="utf-8") as stream:
            stream.write("o surface\n")
            for z in range(n + 1):
                for x in range(n + 1):
                    stream.write(f"v {x / n * 2 - 1} 0 {z / n * 2 - 1}\n")
            for z in range(n):
                for x in range(n):
                    a = z * (n + 1) + x + 1
                    b, c, d = a + 1, a + n + 1, a + n + 2
                    stream.write(f"f {a} {c} {d}\nf {a} {d} {b}\n")
        with session(executable, root, dict(os.environ)) as (ctl, viewer):
            ctl("model", "add", model)
            group = next(item["id"] for item in ctl("objects")["objects"] if item["kind"] == "group")
            ctl("camera", "set", "--target", 0, 0, 0, "--yaw-degrees", -60, "--pitch-degrees", 50, "--distance", 4)
            # The adapter waits/resumes if the worker is still preparing; no timing assertion.
            created = ctl("annotation", "create", group, "--shape", "line", "--start", -.1, -.05, "--end", .1, .05)
            assert created["object"]["targetValid"], created
            assert not created["object"]["targetPending"], created
            annotation = created["target"]
            ctl("annotation", "move", annotation, "--delta", .01, 0)
            ctl("scene", "save-as", scene)
            ctl("scene", "new", "--on-dirty", "discard")
            ctl("scene", "open", scene)
            loaded = ctl("annotation", "list")["annotations"]
            assert len(loaded) == 1 and loaded[0]["targetValid"] and not loaded[0]["targetPending"], loaded
            file_id = next(item["id"] for item in ctl("objects")["objects"] if item["kind"] == "file")
            ctl("model", "remove", file_id)
            missing = ctl("annotation", "list")["annotations"]
            assert len(missing) == 1 and not missing[0]["targetValid"] and not missing[0]["targetPending"], missing
            # Replace/remove while preparation may still be in flight.
            ctl("scene", "open", scene, "--on-dirty", "discard")
            ctl("scene", "new", "--on-dirty", "discard")
            assert ctl("scene", "info")["fileCount"] == 0
            ctl("quit", "--on-dirty", "discard")
            assert viewer.wait(timeout=15) == 0
    print("Loading/annotation preparation lifecycle passed.")


if __name__ == "__main__":
    main()

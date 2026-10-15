# /// script
# requires-python = ">=3.11"
# dependencies = ["Pillow>=11"]
# ///
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile

repository = Path(r"C:/Users/tuncb/.t3/worktrees/woby/t3code-9921b770")
sys.path.insert(0, str(repository / "tests"))
from ctl_obj_colors_smoke import exercise
from ctl_headless_smoke import capture, session

executable = Path(r"D:/.worktree/woby-parser-build-9921/bin/Release/woby.exe")
destination = repository / "doc/benchmarks/obj-vertex-colors-20261016"
destination.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix="woby color render evidence ") as directory:
    root = Path(directory).resolve()
    exercise(executable, root, "f")
    for name in ("f-colors.png", "f-fallback.png", "surface-edges.png", "surface-opacity.png", "colors-f.obj"):
        shutil.copy2(root / name, destination / name)
    print("Synthetic render and reload checks passed; evidence saved.", flush=True)
    environment = dict(os.environ, SDL_VIDEO_DRIVER="woby-test-no-video-driver")
    with session(executable, root, environment, "--file", r"D:/temp/obj_tests/20190810_BearTrap_Ground_Model.obj") as (ctl, viewer):
        for helper in ("grid", "origin", "dimensions"):
            ctl(helper, "set", "--visible", "false")
        objects = ctl("objects")
        (destination / "beartrap-objects.json").write_text(json.dumps(objects, indent=2) + "\n", encoding="utf-8")
        capture(ctl, destination / "beartrap-colors.png")
        (destination / "beartrap-status.json").write_text(json.dumps(ctl("status"), indent=2) + "\n", encoding="utf-8")
        ctl("quit", "--on-dirty", "discard")
        assert viewer.wait(timeout=15) == 0
    print("BearTrap source-color render saved.")

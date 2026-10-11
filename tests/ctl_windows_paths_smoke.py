# /// script
# requires-python = ">=3.11"
# dependencies = ["Pillow>=11"]
# ///
"""Exercise long Windows paths through the real viewer and its automation API."""
import os
from pathlib import Path
import sys
import tempfile
import winreg

from ctl_headless_smoke import session


def main():
    try:
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"SYSTEM\CurrentControlSet\Control\FileSystem") as key:
            enabled, _ = winreg.QueryValueEx(key, "LongPathsEnabled")
    except FileNotFoundError:
        enabled = 0
    if enabled != 1:
        print("Windows long paths are disabled; skip filesystem checks.")
        return
    executable = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="woby-windows-paths-") as directory:
        root = Path(directory).resolve()
        with session(executable, root, os.environ) as (ctl, viewer):
            for minimum in (300, 1200):
                folder = root / str(minimum)
                while len(str(folder)) < minimum:
                    folder /= "café 模型-" + "x" * 64
                folder.mkdir(parents=True)
                small = folder / "small café 模型.obj"
                large = folder / "large café 模型.obj"
                model = "o mesh\nv 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n"
                small.write_text(model, encoding="utf-8")
                large.write_text(("#" + "x" * 62 + "\n") * 32768 + model, encoding="utf-8")
                for path in (small, large):
                    result = ctl("model", "add", path)
                    assert result["addedCount"] == 1 and result["failedCount"] == 0, result
                result = ctl("folder", "add", folder, "--tree")
                assert result["addedCount"] == 2 and result["failedCount"] == 0, result
                count = ctl("scene", "info")["fileCount"]
                scene = folder / "review café 模型.woby"
                ctl("scene", "save-as", scene)
                ctl("scene", "new")
                ctl("scene", "open", scene)
                assert ctl("scene", "info")["fileCount"] == count
                png = folder / "new output" / "image café 模型.png"
                ctl("screenshot", png)
                assert png.read_bytes().startswith(b"\x89PNG\r\n\x1a\n")
                ctl("folder", "add", folder / "missing café 模型", "--tree", code=-32004)
                ctl("scene", "open", folder / "missing café 模型.woby", code=-32016)
                assert viewer.poll() is None
                assert ctl("scene", "info")["fileCount"] == count
            ctl("quit")
            assert viewer.wait(timeout=15) == 0
    print("Windows long model, folder, scene, screenshot, and Unicode error checks passed.")


if __name__ == "__main__":
    main()

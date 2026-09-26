# /// script
# requires-python = ">=3.11"
# dependencies = ["Pillow>=11"]
# ///
"""Verify portable startup; requires no remembered registration for OFF on Windows."""
import os
from pathlib import Path
import shutil
import sys
import tempfile

from ctl_headless_smoke import session
from update_helper_smoke import stage_template


def main():
    binary, helper, assets, importer = (Path(arg).resolve() for arg in sys.argv[1:])
    with tempfile.TemporaryDirectory(prefix="woby-portable-importers-") as temporary:
        root = Path(temporary).resolve()
        deployment = root / "deployment ünicode"
        stage_template(binary, helper, assets, deployment)
        package = deployment / "importers" / "off"
        package.mkdir(parents=True)
        shutil.copy2(importer, package / importer.name)
        shutil.copy2(importer.parent / "importer.json", package / "importer.json")
        (package / "dependency.dll").write_bytes(b"Must not be loaded as an importer")
        broken = deployment / "importers" / "00-broken"
        broken.mkdir()
        (broken / "importer.json").write_text('{"schema":99,"library":"missing.dll"}')
        model = root / "triangle.off"
        model.write_text("OFF\n3 1 0\n0 0 0\n1 0 0\n0 1 0\n3 0 1 2\n")
        # Keep the working directory outside the deployment. XDG_DATA_HOME
        # isolates Linux preferences; SDL uses the Windows user's known folder.
        env = os.environ.copy()
        preferences = root / "preferences"
        preferences.mkdir()
        env["XDG_DATA_HOME"] = str(preferences)
        for launch in range(2):
            executable = deployment / binary.name
            with session(executable, root, env, "--file", model) as (ctl, viewer):
                loaded = ctl("importers", "list")
                entries = [entry for entry in loaded["loaded"] if entry["id"] == "org.woby.example.off"]
                assert len(entries) == 1, loaded
                entry = entries[0]
                assert Path(entry["path"]) == (deployment / "importers/off" / importer.name).resolve(), entry
                assert ctl("scene", "info")["fileCount"] == 1
                ctl("quit", "--on-dirty", "discard")
                assert viewer.wait(timeout=15) == 0
            if launch == 0:
                moved = root / "relocated deployment"
                deployment.rename(moved)
                deployment = moved
        print("Portable startup, isolated failure, and relocation: passed")


if __name__ == "__main__":
    main()

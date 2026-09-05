"""Exercise the real viewer/CLI lifecycle. Requires a desktop and built Debug woby.

Run: python tests/ctl_lifecycle_smoke.py build/vs2026-vcpkg/bin/Debug/woby.exe
"""

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import uuid


def main():
    executable = Path(sys.argv[1]).resolve()
    instance = "lifecycle-" + uuid.uuid4().hex[:12]
    with tempfile.TemporaryDirectory(prefix="woby-lifecycle-smoke-") as directory:
        root = Path(directory)
        model = root / "triangle.obj"
        model.write_text("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n", encoding="utf-8")
        startup = None
        if os.name == "nt":
            startup = subprocess.STARTUPINFO()
            startup.dwFlags = subprocess.STARTF_USESHOWWINDOW
            startup.wShowWindow = 0
        with (root / "viewer.log").open("w", encoding="utf-8") as log:
            viewer = subprocess.Popen(
                [str(executable), "--instance", instance, "--file", str(model)],
                cwd=root, stdout=log, stderr=log, startupinfo=startup,
            )

            def ctl(*arguments, reason=None):
                completed = subprocess.run(
                    [str(executable), "ctl", "--instance", instance, *map(str, arguments), "--json"],
                    cwd=root, capture_output=True, text=True, timeout=30,
                )
                result = json.loads(completed.stdout)
                if reason is None:
                    assert completed.returncode == 0, result
                else:
                    assert completed.returncode == 1, result
                    assert result["data"]["reason"] == reason, result
                return result

            try:
                deadline = time.monotonic() + 30
                while True:
                    assert viewer.poll() is None, (root / "viewer.log").read_text()
                    discovered = subprocess.run(
                        [str(executable), "ctl", "instances", "--json"],
                        capture_output=True, text=True, timeout=5,
                    )
                    if any(item["id"] == instance and item["ready"] for item in json.loads(discovered.stdout)):
                        break
                    assert time.monotonic() < deadline, "viewer did not become ready"
                    time.sleep(0.05)

                original = ctl("objects")["objects"]
                assert original
                saved = root / "before.woby"
                ctl("scene", "open", saved, reason="dirty_scene")
                ctl("scene", "new", reason="dirty_scene")
                ctl("quit", reason="dirty_scene")
                ctl("scene", "save", reason="save_path_required")
                ctl("scene", "new", "--on-dirty", "save", reason="save_path_required")
                ctl("scene", "save-as", root / "missing" / "failed.woby", reason="save_failed")
                assert ctl("objects")["objects"] == original

                # Save before new is a real two-step operation; opening recovers it.
                cleared = ctl("scene", "new", "--on-dirty", "save", "--save-path", saved,
                              "--request-key", "new-1")
                assert cleared["path"] is None and not cleared["dirty"]
                assert saved.exists() and not ctl("objects")["objects"]
                opened = ctl("scene", "open", saved, "--request-key", "open-1")
                loaded = ctl("objects")["objects"]
                assert loaded and {item["id"] for item in loaded}.isdisjoint(item["id"] for item in original)
                assert ctl("scene", "open", saved, "--request-key", "open-1") == opened
                assert ctl("objects")["objects"] == loaded
                ctl("screenshot", root / "opened.png")
                assert (root / "opened.png").stat().st_size > 0

                # Failed parse/import/hierarchy replacement keeps old live resources.
                malformed = root / "malformed.woby"
                malformed.write_text("invalid scene", encoding="utf-8")
                bad_hierarchy = root / "hierarchy.woby"
                bad_hierarchy.write_text(
                    'version = 2\n[[nodes]]\nkind = "file"\nfile_index = 999\n', encoding="utf-8")
                missing_model = root / "missing-model.woby"
                missing_model.write_text(
                    'version = 2\n[[files]]\npath = "missing.obj"\n', encoding="utf-8")
                for bad_path in (root / "nonexistent.woby", malformed, bad_hierarchy, missing_model):
                    ctl("scene", "open", bad_path, reason="open_failed")
                    assert ctl("objects")["objects"] == loaded
                ctl("screenshot", root / "still-loaded.png")

                ctl("scene", "save-as", saved, reason="destination_exists")
                ctl("scene", "save-as", saved, "--overwrite")
                ctl("scene", "save")
                assert ctl("objects")["objects"] == loaded
                copy = root / "copy.woby"
                saved_result = ctl("scene", "save-as", copy, "--request-key", "save-1")
                copy.write_text("external edit", encoding="utf-8")
                assert ctl("scene", "save-as", copy, "--request-key", "save-1") == saved_result
                assert copy.read_text(encoding="utf-8") == "external edit"

                # Replaying old new after opening must not clear a newer scene.
                assert ctl("scene", "new", "--on-dirty", "save", "--save-path", saved,
                           "--request-key", "new-1") == cleared
                assert ctl("objects")["objects"] == loaded
                ctl("scene", "new", "--on-dirty", "discard")
                ctl("scene", "open", saved)
                reopened = ctl("objects")["objects"]
                assert {item["id"] for item in reopened}.isdisjoint(item["id"] for item in loaded)
                assert model.exists()
                result = ctl("quit", "--request-key", "quit-1")
                assert result["quitAccepted"]
                assert viewer.wait(timeout=15) == 0
                print("Lifecycle smoke passed: real CLI, dirty policies, save/open failures, GPU capture, IDs, retries, quit.")
            finally:
                if viewer.poll() is None:
                    viewer.terminate()
                    viewer.wait(timeout=10)


if __name__ == "__main__":
    main()

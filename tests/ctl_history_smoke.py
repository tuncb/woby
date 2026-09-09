"""Exercise CLI history against a temporary real viewer (requires a desktop).

uv run tests/ctl_history_smoke.py build/vs2026-vcpkg/bin/Debug/woby.exe
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
    instance = "history-" + uuid.uuid4().hex[:12]
    with tempfile.TemporaryDirectory(prefix="woby-history-cli-") as directory:
        root = Path(directory)
        model = root / "triangle.obj"

        def write_model(width):
            model.write_text(f"o triangle\nv 0 0 0\nv {width} 0 0\nv 0 1 0\nf 1 2 3\n", encoding="utf-8")

        startup = None
        if os.name == "nt":
            startup = subprocess.STARTUPINFO()
            startup.dwFlags = subprocess.STARTF_USESHOWWINDOW
            startup.wShowWindow = 0
        with (root / "viewer.log").open("w", encoding="utf-8") as log:
            viewer = subprocess.Popen([str(executable), "--instance", instance], cwd=root,
                                      stdout=log, stderr=log, startupinfo=startup)

            def ctl(*args, code=None):
                process = subprocess.run([str(executable), "ctl", "--instance", instance,
                                          *map(str, args), "--json"], cwd=root,
                                         capture_output=True, text=True, timeout=75)
                assert process.stdout, (args, process.returncode, process.stderr)
                result = json.loads(process.stdout)
                if code is None:
                    assert process.returncode == 0, (args, result)
                else:
                    assert process.returncode == 1 and result["code"] == code, (args, result)
                return result

            try:
                deadline = time.monotonic() + 30
                while True:
                    assert viewer.poll() is None, (root / "viewer.log").read_text()
                    discovered = subprocess.run([str(executable), "ctl", "instances", "--json"],
                                                capture_output=True, text=True, timeout=5)
                    if any(item["id"] == instance and item["ready"] for item in json.loads(discovered.stdout)):
                        break
                    assert time.monotonic() < deadline, "viewer did not become ready"
                    time.sleep(0.05)

                assert not ctl("scene", "undo")["applied"]
                assert not ctl("scene", "redo")["applied"]
                ctl("grid", "set", "--visible", "true")
                ctl("origin", "set", "--visible", "true")
                camera = ctl("camera", "move", "--forward", "1")["camera"]
                undone = ctl("scene", "undo", "--request-key", "undo-once")
                assert undone["applied"] and undone["action"] == "undo"
                assert ctl("scene", "undo", "--request-key", "undo-once") == undone
                info = ctl("scene", "info")
                assert info["showGrid"] and not info["showOrigin"]
                assert ctl("camera", "get")["camera"] == camera
                assert ctl("scene", "undo")["applied"]
                assert not ctl("scene", "info")["showGrid"]
                assert not ctl("scene", "undo")["applied"]
                redone = ctl("scene", "redo", "--request-key", "redo-once")
                assert redone["applied"] and redone["action"] == "redo"
                assert ctl("scene", "redo", "--request-key", "redo-once") == redone
                assert not ctl("scene", "info")["showOrigin"]
                assert ctl("scene", "redo")["applied"]
                assert ctl("scene", "info")["showOrigin"]
                assert not ctl("scene", "redo")["applied"]
                ctl("scene", "redo", "--request-key", "undo-once", code=-32006)

                # Saving retains history and restores dirty state against the saved version.
                saved = root / "saved.woby"
                ctl("scene", "save-as", saved)
                saved_camera = ctl("camera", "get")["camera"]
                assert ctl("scene", "undo")["dirty"]
                assert not ctl("scene", "redo")["dirty"]
                ctl("scene", "undo")
                ctl("vertex-size", "set", "scene", "--pixels", "12")
                assert not ctl("scene", "redo")["applied"]
                ctl("scene", "new", "--on-dirty", "discard")
                assert not ctl("scene", "undo")["applied"]
                ctl("scene", "open", saved)
                assert ctl("camera", "get")["camera"] == saved_camera
                assert not ctl("scene", "undo")["applied"]
                assert not ctl("scene", "redo")["applied"]
                # Camera-only edits remain clean, but an explicit Save persists them.
                saved_camera = ctl("camera", "orbit", "--yaw-degrees", "25", "--pitch-degrees", "-10")["camera"]
                assert not ctl("scene", "info")["dirty"]
                assert not ctl("scene", "undo")["applied"]
                ctl("scene", "save")
                ctl("camera", "frame")
                ctl("scene", "open", saved)
                assert ctl("camera", "get")["camera"] == saved_camera
                ctl("scene", "new", "--on-dirty", "discard")

                # Add/remove history reloads geometry but preserves the object's identity.
                write_model(1)
                added = ctl("model", "add", model)
                file_id = added["addedIds"][0]
                assert ctl("scene", "undo")["applied"]
                assert ctl("scene", "info")["fileCount"] == 0
                write_model(2)
                assert ctl("scene", "redo")["applied"]
                assert ctl("object", file_id)["object"]["triangleCount"] == 1
                assert ctl("scene", "bounds")["bounds"]["max"][0] == 2
                ctl("model", "remove", file_id)
                assert ctl("scene", "undo")["applied"]
                assert ctl("object", file_id)["object"]["triangleCount"] == 1
                ctl("model", "remove", file_id)
                model.unlink()
                failed = ctl("scene", "undo", "--request-key", "failed-undo", code=-32004)
                assert "Undo skipped:" in failed["error"]
                assert ctl("scene", "undo", "--request-key", "failed-undo", code=-32004) == failed
                assert ctl("scene", "info")["fileCount"] == 0
                assert ctl("scene", "undo")["applied"]
                assert not ctl("scene", "undo")["applied"]

                # A failed redo is consumed, including on a keyed retry.
                ctl("scene", "new", "--on-dirty", "discard")
                write_model(1)
                ctl("model", "add", model)
                ctl("scene", "undo")
                model.unlink()
                failed = ctl("scene", "redo", "--request-key", "failed-redo", code=-32004)
                assert "Redo skipped:" in failed["error"]
                assert ctl("scene", "redo", "--request-key", "failed-redo", code=-32004) == failed
                assert not ctl("scene", "redo")["applied"]
                assert ctl("scene", "info")["fileCount"] == 0
                ctl("quit", "--on-dirty", "discard")
                assert viewer.wait(timeout=15) == 0
                print("History CLI smoke passed: triggers, empty history, retries, camera, save/open/new, branching, reloads, and consumed failures.")
            finally:
                if viewer.poll() is None:
                    viewer.kill()
                    viewer.wait(timeout=10)


if __name__ == "__main__":
    main()

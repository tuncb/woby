"""Exercise every scene-control family through a temporary real viewer.

python tests/ctl_controls_smoke.py build/vs2026-vcpkg/bin/Debug/woby.exe \
    build/vs2026-vcpkg/example-plugins/Debug/woby_off_importer.dll
"""
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import uuid


def main():
    executable = Path(sys.argv[1]).resolve()
    importer = Path(sys.argv[2]).resolve()
    instance = "controls-" + uuid.uuid4().hex[:12]
    with tempfile.TemporaryDirectory(prefix="woby-controls-") as directory:
        root = Path(directory)
        model = root / "triangle.obj"
        model.write_text("o triangle\nv 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n", encoding="utf-8")
        folder = root / "models"
        (folder / "sub").mkdir(parents=True)
        (folder / "sub" / "second.obj").write_bytes(model.read_bytes())
        (folder / "bad.obj").write_text("invalid", encoding="utf-8")
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
                assert ctl("status")["ready"]
                capabilities = ctl("capabilities")
                assert len(capabilities["methods"]) == 44
                assert ctl("scene", "info")["fileCount"] == 0
                assert ctl("scene", "tree")["nodes"] == []
                assert "bounds" in ctl("scene", "bounds")
                added = ctl("model", "add", model, "--request-key", "add-once")
                assert added["addedCount"] == 1 and added["outcomes"][0]["state"] == "added", added
                assert ctl("model", "add", model, "--request-key", "add-once") == added
                file_id = added["addedIds"][0]
                objects = ctl("objects")["objects"]
                group_id = next(item["id"] for item in objects if item["kind"] == "group")
                assert ctl("object", group_id)["object"]["settings"]["visible"]
                assert ctl("object", file_id)["object"]["triangleCount"] == 1
                ctl("visibility", "set", file_id, "--visible", "false")
                assert not ctl("object", group_id)["object"]["settings"]["visible"]
                ctl("visibility", "set", group_id, "--visible", "true")
                ctl("render", "set", "scene", "--solid", "true", "--triangles", "false", "--vertices", "true")
                ctl("transform", "set", file_id, "--translation", "10", "0", "0", "--rotation-degrees", "0", "0", "20", "--scale", "2")
                assert ctl("transform", "get", file_id)["settings"]["scale"] == 2
                ctl("opacity", "set", file_id, "--value", "0.3")
                assert ctl("transform", "reset", file_id)["applied"]["opacity"] == 1
                ctl("opacity", "set", group_id, "--value", "0.5")
                ctl("color", "set", group_id, "--rgb", "1", "0", "0")
                ctl("color", "reset", group_id)
                ctl("vertex-size", "set", "scene", "--pixels", "8")
                ctl("vertex-size", "set", file_id, "--scale", "2")
                ctl("vertex-size", "set", group_id, "--scale", "3")
                ctl("grid", "set", "--visible", "false")
                ctl("origin", "set", "--visible", "false")
                ctl("up-axis", "set", "y")
                ctl("scene", "save-as", root / "edited.woby")
                assert not ctl("status")["dirty"]
                ctl("camera", "frame")
                camera = ctl("camera", "get")["camera"]
                orbit = ctl("camera", "orbit", "--yaw-degrees", "30", "--pitch-degrees", "-10")["camera"]
                assert math.isclose(orbit["yawDegrees"] - camera["yawDegrees"], 30, abs_tol=0.001)
                ctl("camera", "roll", "--roll-degrees", "15")
                ctl("camera", "pan", "--right", "1", "--up", "2")
                ctl("camera", "dolly", "--factor", "0.5")
                moved = ctl("camera", "move", "--forward", "1", "--request-key", "move-once")
                assert ctl("camera", "move", "--forward", "1", "--request-key", "move-once") == moved
                assert ctl("camera", "get")["camera"] == moved["camera"]
                ctl("camera", "move", "--forward", "2", "--request-key", "move-once", code=-32006)
                assert ctl("command", "--request-key", "move-once")["state"] == "succeeded"
                ctl("pane", "set", "--visible", "false", "--width", "500")
                assert not ctl("status")["dirty"]
                ctl("pane", "set", "--visible", "true")
                assert ctl("stats")["triangleCount"] == 1
                assert "stagesMilliseconds" in ctl("performance", "get")
                ctl("camera", "frame")
                ctl("screenshot", root / "edited.png")
                assert (root / "edited.png").stat().st_size > 100
                ctl("color", "reset", file_id, code=-32602)
                ctl("model", "remove", group_id, code=-32602)
                batch = ctl("folder", "add", folder, "--tree")
                assert batch["addedCount"] == 1 and batch["failedCount"] == 1, batch
                folder_id = next(item["id"] for item in ctl("objects")["objects"] if item["kind"] == "folder")
                ctl("transform", "set", folder_id, "--scale", "2")
                ctl("opacity", "set", folder_id, "--value", "0.5")
                ctl("render", "set", folder_id, "--vertices", "false")
                ctl("visibility", "set", folder_id, "--visible", "false")
                ctl("visibility", "set", folder_id, "--visible", "true")
                ctl("transform", "reset", folder_id)
                assert ctl("folder", "add", folder)["addedCount"] == 1
                assert ctl("model", "add", root / "missing.obj")["failedCount"] == 1
                ctl("model", "remove", file_id)
                ctl("object", file_id, code=-32005)
                ctl("visibility", "set", file_id, "--visible", "true", code=-32005)
                assert "loaded" in ctl("importers", "list")
                assert ctl("importers", "scan", importer.parent)["failedCount"] == 0
                assert ctl("importers", "add", importer)["loadedCount"] == 1
                # No --remember in the live test: registration persistence uses isolated unit-test settings.
                forgotten = ctl("importers", "forget", root / "never-registered.dll")
                assert not forgotten["forgotten"]
                assert ctl("importers", "add", root / "missing.dll")["failedCount"] == 1
                ctl("scene", "open", root / "edited.woby", "--on-dirty", "discard")
                reopened = ctl("scene", "info")
                assert reopened["upAxis"] == "y" and not reopened["showGrid"] and reopened["fileCount"] == 1
                ctl("quit", "--on-dirty", "discard")
                assert viewer.wait(timeout=15) == 0
                print("Controls smoke passed: all 34 added commands, rich objects, persistence, captures, imports, retries, and errors.")
            finally:
                if viewer.poll() is None:
                    viewer.kill()
                    viewer.wait(timeout=10)


if __name__ == "__main__":
    main()

"""Capture annotated comparisons as the first command in fresh real viewers.

Requires a desktop. Usage:
python tests/ctl_startup_screenshot_smoke.py path/to/woby.exe build/startup-capture-qa
"""

import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import time
import uuid


def main():
    executable = Path(sys.argv[1]).resolve()
    root = Path(sys.argv[2]).resolve()
    root.mkdir(parents=True, exist_ok=True)
    scene = Path(__file__).resolve().parents[1] / "assets/samples/mesh-comparison/compare.woby"
    startup = None
    if os.name == "nt":
        startup = subprocess.STARTUPINFO()
        startup.dwFlags = subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = 0

    for attempt in range(4):
        instance = "startup-capture-" + uuid.uuid4().hex[:12]
        with (root / f"viewer-{attempt}.log").open("w", encoding="utf-8") as log:
            viewer = subprocess.Popen(
                [str(executable), "--instance", instance, "--scene", str(scene)],
                cwd=root, stdout=log, stderr=log, startupinfo=startup,
            )

            def ctl(*args):
                completed = subprocess.run(
                    [str(executable), "ctl", "--instance", instance, *map(str, args), "--json"],
                    cwd=root, capture_output=True, text=True, timeout=20,
                )
                assert completed.returncode == 0, (args, completed.stdout, completed.stderr)
                return json.loads(completed.stdout)

            try:
                deadline = time.monotonic() + 30
                while True:
                    assert viewer.poll() is None, f"Viewer exited during startup: {attempt}"
                    discovered = subprocess.run(
                        [str(executable), "ctl", "instances", "--json"],
                        capture_output=True, text=True, timeout=5,
                    )
                    if any(item["id"] == instance and item["ready"]
                           for item in json.loads(discovered.stdout)):
                        break
                    assert time.monotonic() < deadline, "Viewer did not become ready"
                    time.sleep(0.01)

                # No warm-up commands or sleeps: the first export must upload any
                # textures created by the annotation font size in the same frame.
                for capture in ("first", "repeat"):
                    output = root / f"{attempt}-{capture}.png"
                    result = ctl("screenshot", output)
                    assert result["state"] == "succeeded", result
                    png = output.read_bytes()
                    assert png[:8] == b"\x89PNG\r\n\x1a\n"
                    assert struct.unpack(">II", png[16:24]) == (1920, 1800)
                    assert len(png) > 10000, "Expected scene geometry and comparison annotations"
                ctl("quit")
                viewer.wait(timeout=10)
                assert viewer.returncode == 0
                print(f"Startup {attempt + 1}: initial and repeat annotated captures passed", flush=True)
            finally:
                # Also dismiss a crashed Debug viewer blocked in the CRT assertion
                # dialog; do not leave a hung process behind on test failure.
                if viewer.poll() is None:
                    viewer.terminate()
                    viewer.wait(timeout=10)

    print("Startup screenshot smoke passed: four fresh viewers, eight annotated exports.")


if __name__ == "__main__":
    main()

"""Measure a real viewer importing thousands of distinct tiny models.

Run manually with a Release executable; timings are observations, not assertions.
All source and scene fixtures share one unique temporary directory and are removed
even on failure. JSON measurements and captures are retained in the output folder.
"""

import json
import os
from pathlib import Path
import statistics
import subprocess
import sys
import tempfile
import time
import uuid


def main():
    executable = Path(sys.argv[1]).resolve()
    output = Path(sys.argv[2]).resolve()
    count = int(sys.argv[3]) if len(sys.argv) > 3 else 2500
    if not 1 <= count <= 3500:
        raise ValueError("Expected 1..3500 models (leave bgfx handles for the UI and display modes).")
    output.mkdir(parents=True, exist_ok=True)
    instance = "performance-" + uuid.uuid4().hex[:12]
    startup = None
    if os.name == "nt":
        startup = subprocess.STARTUPINFO()
        startup.dwFlags = subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = 0
    with tempfile.TemporaryDirectory(prefix="woby-performance-") as directory:
        root = Path(directory)
        models = root / "models"
        models.mkdir()
        for i in range(count):
            x, y = i % 50, i // 50
            (models / f"part-{i:04}.obj").write_text(
                f"o part\nv {x} {y} 0\nv {x + .75} {y} 0\nv {x} {y + .75} 0\nf 1 2 3\n",
                encoding="utf-8",
            )
        with (output / "viewer.log").open("w", encoding="utf-8") as log:
            viewer = subprocess.Popen([str(executable), "--instance", instance, "--log-level", "info",
                                       "--log-file", str(output / "performance.log"), "--log-performance"], cwd=root,
                                      stdout=log, stderr=log, startupinfo=startup)

            def ctl(*args):
                process = subprocess.run([str(executable), "ctl", "--instance", instance,
                                          *map(str, args), "--json", "--timeout", "180"],
                                         cwd=root, capture_output=True, text=True, timeout=190)
                assert process.returncode == 0, (args, process.stdout, process.stderr)
                return json.loads(process.stdout)

            try:
                deadline = time.monotonic() + 30
                while True:
                    assert viewer.poll() is None, "Viewer exited before readiness"
                    discovered = subprocess.run([str(executable), "ctl", "instances", "--json"],
                                                capture_output=True, text=True, timeout=5)
                    if any(item["id"] == instance and item["ready"] for item in json.loads(discovered.stdout)):
                        break
                    assert time.monotonic() < deadline, "Viewer did not become ready"
                    time.sleep(.05)
                ctl("pane", "set", "--visible", "true")
                start = time.perf_counter()
                added = ctl("folder", "add", models)
                import_seconds = time.perf_counter() - start
                assert added["addedCount"] == count and added["failedCount"] == 0, added
                assert len(added["addedIds"]) == count, added
                assert all(item["state"] == "added" for item in added["outcomes"]), added
                assert ctl("scene", "info")["fileCount"] == count
                ctl("up-axis", "set", "z")
                ctl("camera", "view", "top")
                ctl("camera", "frame")
                ctl("screenshot", output / "solid.png")
                samples = [ctl("performance", "get") for _ in range(20)]
                # Exercise demand allocation, retained buffers, and screenshot
                # preparation without exhausting the backend's per-buffer handles.
                for enabled in ("true", "false", "true"):
                    ctl("render", "set", added["addedIds"][0], "--triangles", enabled, "--vertices", enabled)
                ctl("screenshot", output / "display-modes.png")
                saved = root / "scene.woby"
                ctl("scene", "save-as", saved)
                # Free the old GPU resources before reopening this very large
                # scene: transactional replacement otherwise doubles handle use.
                ctl("scene", "new")
                ctl("scene", "open", saved)
                assert ctl("scene", "info")["fileCount"] == count
                ctl("screenshot", output / "reopened.png")
                report = {
                    "models": count, "import_seconds": import_seconds,
                    "median_frame_ms": statistics.median(s["frameMilliseconds"] for s in samples),
                    "median_stages_ms": {stage: statistics.median(s["stagesMilliseconds"][stage] for s in samples)
                                         for stage in samples[0]["stagesMilliseconds"]},
                    "samples": samples,
                }
                (output / "measurements.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
                print(json.dumps({k: v for k, v in report.items() if k != "samples"}, indent=2), flush=True)
                ctl("quit", "--on-dirty", "discard")
                assert viewer.wait(timeout=15) == 0
            finally:
                if viewer.poll() is None:
                    viewer.kill()
                    viewer.wait(timeout=10)


if __name__ == "__main__":
    main()

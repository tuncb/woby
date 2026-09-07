"""Offline integration test of the real helper, process handoff, and rollback."""
import importlib.util
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def load_packaging():
    path = Path(__file__).resolve().parents[1] / ".github/scripts/package_manifest.py"
    spec = importlib.util.spec_from_file_location("package_manifest", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def write_json(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data), encoding="utf-8")


def run_parent(helper, root, job):
    # The helper must observe its original parent exit before it modifies files.
    script = """
import os, subprocess, sys, time
from pathlib import Path
helper, root, job = sys.argv[1:]
child = subprocess.Popen([helper, '--apply', root, job, str(os.getpid())],
    creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
for _ in range(200):
    if (Path(job) / 'ready.json').exists():
        assert not (Path(job) / 'journal.json').exists(), 'helper wrote before parent exited'
        sys.exit(0)
    if child.poll() is not None:
        raise RuntimeError('helper exited before ready')
    time.sleep(.05)
raise RuntimeError('helper handshake timed out')
"""
    subprocess.run([sys.executable, "-c", script, str(helper), str(root), str(job)], check=True, timeout=20)
    for _ in range(600):
        status = json.loads((root / ".woby-update/status.json").read_text(encoding="utf-8"))
        if status["state"] not in ("pending", "applying"):
            return status
        time.sleep(.05)
    raise RuntimeError("helper did not finish")


def main():
    package = load_packaging()
    binary = Path(sys.argv[1]).resolve()
    platform = "windows-x64" if os.name == "nt" else "macos-arm64" if sys.platform == "darwin" else "linux-x64"
    suffix = ".exe" if os.name == "nt" else ""
    version = subprocess.check_output([str(binary), "--version"], text=True).strip()
    parts = list(map(int, version.split(".")))
    future = f"{parts[0] + 1}.0.0"
    with tempfile.TemporaryDirectory(prefix="woby-helper-test-") as temporary:
        base = Path(temporary)
        template = base / "template"
        template.mkdir()
        for source in binary.parent.iterdir():
            if source.name in ("woby" + suffix, "woby-update-helper" + suffix) or source.suffix in (".dll", ".dylib") or ".so" in source.name:
                shutil.copy2(source, template / source.name)
        shutil.copytree(binary.parent / "assets", template / "assets")
        # Use a metadata-only older version; this tests the installer with the
        # actual built binaries, without requiring a historical release/network.
        for scenario in ("success", "health-failure", "interrupted"):
            root = base / (scenario + " deployment ünicode")
            shutil.copytree(template, root)
            (root / "obsolete.dat").write_text("old managed file")
            old_manifest = package.collect_manifest(root, platform, "0.0.0")
            write_json(root / package.MANIFEST, old_manifest)
            (root / "my-scene.woby").write_text("user scene")
            (root / "plugins").mkdir()
            (root / "plugins/custom.dll").write_text("user plugin")
            job = root / ".woby-update/job-0123456789abcdef0123456789abcdef"
            shutil.copytree(template, job / "package")
            new_manifest = package.collect_manifest(job / "package", platform, future if scenario == "health-failure" else version)
            write_json(job / "package" / package.MANIFEST, new_manifest)
            (job / "helper").mkdir()
            for source in template.iterdir():
                if source.is_file():
                    shutil.copy2(source, job / "helper" / source.name)
            helper = job / "helper" / ("woby-update-helper" + suffix)
            write_json(root / ".woby-update/status.json", {"state": "pending", "job": job.name, "message": "integration test"})
            if scenario == "interrupted":
                names = [file["path"] for file in old_manifest["files"]] + [package.MANIFEST]
                for name in names:
                    destination = job / "backup" / name
                    destination.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(root / name, destination)
                write_json(job / "journal.json", {"schema": 1, "root": str(root),
                           "files": [{"path": name, "existed": True} for name in names]})
                write_json(job / "backup-complete.json", {"complete": True})
                (root / "obsolete.dat").unlink()
                write_json(root / package.MANIFEST, new_manifest)
                subprocess.run([str(helper), "--recover", str(root), str(job)], check=True, timeout=30)
                status = json.loads((root / ".woby-update/status.json").read_text())
            else:
                status = run_parent(helper, root, job)
            expected = "completed" if scenario == "success" else "failed"
            assert status["state"] == expected, status
            installed = json.loads((root / package.MANIFEST).read_text())
            assert installed["version"] == (version if scenario == "success" else "0.0.0"), status
            assert (root / "obsolete.dat").exists() == (scenario != "success")
            assert (root / "my-scene.woby").read_text() == "user scene"
            assert (root / "plugins/custom.dll").read_text() == "user plugin"
            # A completed transaction must never be rolled back by an old helper.
            completed = subprocess.run([str(helper), "--recover", str(root), str(job)], capture_output=True, timeout=30)
            assert completed.returncode == 1
            assert json.loads((root / package.MANIFEST).read_text()) == installed
            # Exercise the actual public CLI's result contract without network.
            query = subprocess.run([str(root / ("woby" + suffix)), "update", "--status", "--json"], capture_output=True, text=True, timeout=15)
            assert query.returncode == (0 if scenario == "success" else 1), query.stderr
            assert json.loads(query.stdout)["state"] == expected
            print(scenario + ": passed")
            time.sleep(.1)  # Allow the detached helper to finish exiting before cleanup.


if __name__ == "__main__":
    main()

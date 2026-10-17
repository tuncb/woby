"""Exercise difference CLI jobs in a headless viewer with synthetic models.

uv run tests/ctl_difference_smoke.py build/vs2026-vcpkg/bin/Release/woby.exe
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
    instance = "difference-" + uuid.uuid4().hex[:12]
    env = dict(os.environ, SDL_VIDEO_DRIVER="woby-test-no-video-driver")
    env.setdefault("VK_LOADER_LAYERS_DISABLE", "~implicit~")
    with tempfile.TemporaryDirectory(prefix="woby-difference-cli-") as directory:
        root = Path(directory).resolve()
        vertices = ("v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n"
                    "v 0 0 1\nv 1 0 1\nv 1 1 1\nv 0 1 1\n")
        # A is parametric. B has a missing triangle and a duplicate face.
        parametric = root / "parametric.obj"
        parametric.write_text(vertices + "cstype bezier\ndeg 1 1\n" + "".join(
            f"surf 0 1 0 1 {face}\nparm u 0 1\nparm v 0 1\nend\n"
            for face in ("1 4 2 3", "5 6 8 7", "1 2 5 6", "2 3 6 7", "3 4 7 8", "4 1 8 5")), encoding="utf-8")
        triangular = root / "defective.obj"
        triangular.write_text(vertices + "".join(f"f {face}\n" for face in (
            "1 4 3", "5 6 7", "5 7 8", "1 2 6", "1 6 5", "2 3 7", "2 7 6",
            "3 4 8", "3 8 7", "4 1 5", "4 5 8", "5 6 7")), encoding="utf-8")
        log_path = root / "viewer.log"
        with log_path.open("w", encoding="utf-8") as log:
            viewer = subprocess.Popen([str(executable), "--headless", "--instance", instance],
                                      cwd=root, env=env, stdout=log, stderr=log)
            clients = []

            def arguments(*args):
                return [str(executable), "ctl", "--instance", instance, *map(str, args), "--json"]

            def ctl(*args, success=True):
                result = subprocess.run(arguments(*args), cwd=root, env=env, capture_output=True,
                                        text=True, encoding="utf-8", timeout=75)
                assert result.stdout.strip(), (args, result.returncode, result.stderr)
                value = json.loads(result.stdout)
                assert (result.returncode == 0) == success, (args, value, result.stderr)
                return value

            def start(*args):
                client = subprocess.Popen(arguments(*args), cwd=root, env=env, stdout=subprocess.PIPE,
                                          stderr=subprocess.PIPE, text=True, encoding="utf-8")
                clients.append(client)
                return client

            try:
                deadline = time.monotonic() + 45
                while True:
                    assert viewer.poll() is None, log_path.read_text(encoding="utf-8")
                    found = subprocess.run([str(executable), "ctl", "instances", "--json"],
                                           env=env, capture_output=True, text=True, timeout=10)
                    if any(item["id"] == instance and item["ready"] for item in json.loads(found.stdout)):
                        break
                    assert time.monotonic() < deadline, "Viewer startup timed out"
                    time.sleep(.025)
                a = ctl("model", "add", parametric)["addedIds"][0]
                b = ctl("model", "add", triangular)["addedIds"][0]
                ctl("transform", "set", b, "--translation", ".5", "0", "0")
                analysis = ctl("analysis", "create", "--a", a, "--b", b,
                               "--alignment", "scene", "--lock-placement", "false")["target"]

                def difference(action, *args, **kwargs):
                    return ctl("analysis", "difference-" + action, analysis, *args, **kwargs)

                difference("check-resolution", success=False)
                difference("create", "--method", "direct", success=False)
                base = difference("rebuild", "--method", "approximate", "--cell-size", ".1", "--wait")["difference"]
                assert base["current"] and base["kind"] == "approximate_volume", base
                checked = difference("check-resolution", "--wait")["difference"]
                check = checked["resolutionCheck"]
                assert check["state"] == "ready" and check["cellSize"] == .05, checked
                assert check["baseCellSize"] == .1
                assert check["totalVolumeChange"] == check["totalVolume"] - base["totalVolume"]
                assert checked["totalVolume"] == base["totalVolume"]
                assert difference("get")["difference"]["resolutionCheck"] == check
                print("Parametric/defective comparison and half-cell check passed.", flush=True)
                difference("set", "--cell-size", ".2")
                assert difference("get")["difference"]["totalVolume"] is None
                difference("check-resolution", success=False)

                # Use small triangle inputs for the fine-grid lifecycle checks.
                # This keeps test time independent of freeform tessellation density.
                reference = root / "reference.obj"
                reference.write_bytes(triangular.read_bytes())
                reference_id = ctl("model", "add", reference)["addedIds"][0]
                ctl("analysis", "clear", analysis, "--side", "a")
                ctl("analysis", "add", analysis, "--side", "a", "--object", reference_id)
                # Keep the check busy long enough to send a separate cancellation call.
                base = difference("rebuild", "--cell-size", ".025")["difference"]
                client = start("analysis", "difference-check-resolution", analysis, "--request-key", "cancel-check")
                deadline = time.monotonic() + 30
                while True:
                    status = difference("get")["difference"]
                    if status["resolutionCheck"]["state"] == "running":
                        break
                    assert client.poll() is None, "Check finished before cancellation was sent"
                    assert time.monotonic() < deadline, status
                canceled = difference("cancel-check")["difference"]
                assert canceled["current"] and canceled["totalVolume"] == base["totalVolume"], canceled
                stdout, stderr = client.communicate(timeout=20)
                assert client.returncode == 1, (stdout, stderr)
                assert "canceled" in json.loads(stdout)["error"], stdout
                assert ctl("command", "--request-key", "cancel-check")["state"] == "failed"
                assert "totalVolume" not in difference("get")["difference"]["resolutionCheck"]
                print("Concurrent status and resolution cancellation passed.", flush=True)

                client = start("analysis", "difference-check-resolution", analysis)
                deadline = time.monotonic() + 30
                while difference("get")["difference"]["resolutionCheck"]["state"] != "running":
                    assert client.poll() is None, "Check finished before the source change"
                    assert time.monotonic() < deadline, "Check did not start"
                ctl("transform", "set", b, "--translation", ".6", "0", "0")
                stdout, stderr = client.communicate(timeout=20)
                assert client.returncode == 1, (stdout, stderr)
                assert "out of date" in json.loads(stdout)["error"], stdout
                assert not difference("get")["difference"]["current"]
                print("Source changes reject a pending check result.", flush=True)

                # A failed half-cell check returns an error and keeps the base result.
                difference("rebuild", "--cell-size", ".02")
                failure = difference("check-resolution", success=False)
                assert "grid limit" in failure["error"], failure
                status = difference("get")["difference"]
                assert status["current"] and status["resolutionCheck"]["state"] == "failed", status
                saved = root / "saved.woby"
                ctl("scene", "save-as", saved)
                ctl("scene", "open", saved)
                analysis = next(item["id"] for item in ctl("objects")["objects"] if item["kind"] == "analysis")
                rebuilt = difference("rebuild")["difference"]
                assert rebuilt["method"] == "approximate" and rebuilt["cellSize"] == .02, rebuilt
                ctl("quit", "--on-dirty", "discard")
                viewer.wait(timeout=15)
            except BaseException:
                print(log_path.read_text(encoding="utf-8"), file=sys.stderr)
                raise
            finally:
                if viewer.poll() is None:
                    viewer.kill()
                viewer.wait(timeout=15)
                for client in clients:
                    if client.poll() is None:
                        client.kill()
                    client.communicate(timeout=15)
    print("Difference CLI passed: parametric/defective inputs, build, resolution, status, cancellation, failure, and persistence.")


if __name__ == "__main__":
    main()

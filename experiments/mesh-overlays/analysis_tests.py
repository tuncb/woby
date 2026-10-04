# /// script
# requires-python = ">=3.11"
# dependencies = ["Pillow>=11", "psutil>=6"]
# ///
"""Small behavioral checks for the capture comparison used by the experiment."""
from pathlib import Path
import tempfile
from types import SimpleNamespace
from unittest.mock import patch

from PIL import Image

from analyze import compare
from run import overlapping_workloads


def test_identical_and_changed_pixels():
    with tempfile.TemporaryDirectory(prefix="woby overlay parity ") as directory:
        root = Path(directory).resolve()
        reference = Image.new("RGB", (4, 3), (10, 20, 30))
        reference.save(root / "reference.png")
        reference.save(root / "same.png")
        assert compare(root / "reference.png", root / "same.png") == dict(
            different_pixels=0, over_8_levels=0, max_channel_difference=0)
        changed = reference.copy()
        changed.putpixel((1, 1), (18, 20, 30))
        changed.putpixel((2, 2), (10, 29, 30))
        changed.save(root / "changed.png")
        assert compare(root / "reference.png", root / "changed.png") == dict(
            different_pixels=2, over_8_levels=1, max_channel_difference=9)


def test_dimensions_cannot_be_compared_as_equivalent():
    with tempfile.TemporaryDirectory(prefix="woby overlay dimensions ") as directory:
        root = Path(directory).resolve()
        Image.new("RGB", (4, 3)).save(root / "a.png")
        Image.new("RGB", (3, 4)).save(root / "b.png")
        try:
            compare(root / "a.png", root / "b.png")
        except ValueError:
            return
        raise AssertionError("Mismatched capture dimensions must be rejected")


def test_workload_guard_distinguishes_idle_build_workers():
    processes = [SimpleNamespace(info=dict(pid=pid, name=name, cmdline=command))
                 for pid, name, command in (
                     (1, "woby_point_prototype.exe", []),
                     (2, "MSBuild.exe", ["MSBuild", "/nodemode:1", "/nodeReuse:true"]),
                     (3, "MSBuild.exe", ["MSBuild", "woby.sln"]),
                     (4, "ctest.exe", []), (5, "cl.exe", []), (6, "unrelated.exe", []))]
    with patch("run.psutil.process_iter", return_value=processes):
        assert [item["pid"] for item in overlapping_workloads()] == [1, 3, 4, 5]
        assert [item["pid"] for item in overlapping_workloads(1)] == [3, 4, 5]


if __name__ == "__main__":
    test_identical_and_changed_pixels()
    test_dimensions_cannot_be_compared_as_equivalent()
    test_workload_guard_distinguishes_idle_build_workers()
    print("3 capture-analysis and workload-guard tests passed")

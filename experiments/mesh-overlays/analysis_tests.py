# /// script
# requires-python = ">=3.11"
# dependencies = ["Pillow>=11"]
# ///
"""Small behavioral checks for the capture comparison used by the experiment."""
from pathlib import Path
import tempfile

from PIL import Image

from analyze import compare


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


if __name__ == "__main__":
    test_identical_and_changed_pixels()
    test_dimensions_cannot_be_compared_as_equivalent()
    print("2 capture-analysis tests passed")

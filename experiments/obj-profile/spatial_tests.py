"""Test the optional key capture and replay tools with isolated temporary files."""
import argparse
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

from spatial_capture import capture

REPLAY = None


def test_capture_requires_fresh_output_and_validates_both_sources():
    checks = unittest.TestCase()
    with tempfile.TemporaryDirectory(prefix="woby spatial capture ") as directory:
        root = Path(directory).resolve()
        points, annotations = root / "point_cloud.cpp", root / "surface_annotation.cpp"
        points.write_text("        radix(keys,cancel,helpers);\n", encoding="utf-8")
        annotations.write_text("missing anchor\n", encoding="utf-8")
        original = points.read_text(encoding="utf-8")
        with checks.assertRaises(ValueError):
            capture(root, root / "keys")
        checks.assertEqual(points.read_text(encoding="utf-8"), original)
        checks.assertFalse((root / "keys").exists())
        annotations.write_text("        stableSpatialRadixSort(keys, morton, check);\n", encoding="utf-8")
        capture(root, root / "keys")
        for path in (points, annotations):
            text = path.read_text(encoding="utf-8")
            checks.assertIn("capture.close();", text)
            checks.assertIn(json.dumps(str(root / "keys"))[1:-1], text)
        before = points.read_text(encoding="utf-8")
        with checks.assertRaises(FileExistsError):
            capture(root, root / "keys")
        checks.assertEqual(points.read_text(encoding="utf-8"), before)
        with checks.assertRaises(ValueError):
            capture(root, root / "second capture")
        checks.assertFalse((root / "second capture").exists())
        checks.assertEqual(points.read_text(encoding="utf-8"), before)

def test_replay_checks_all_variants_and_rejects_invalid_inputs():
    checks = unittest.TestCase()
    if not REPLAY:
        checks.skipTest("Pass --replay to test the optional executable")
    with tempfile.TemporaryDirectory(prefix="woby spatial replay ") as directory:
        path = Path(directory).resolve() / "keys.bin"
        path.write_bytes(b"".join(struct.pack("<II", (i * 2654435761) & 0xffffffff, i)
                                  for i in range(8193)))
        for helpers in (0, 3):
            rows = [json.loads(line) for line in subprocess.check_output(
                [str(REPLAY), str(path), str(helpers), "1"], text=True, timeout=60).splitlines()]
            checks.assertEqual(len(rows), 13)
            checks.assertEqual(len({row["variant"] for row in rows}), 13)
            checks.assertTrue(all(row["count"] == 8193 and row["helpers"] == helpers for row in rows))
            checks.assertTrue(all(row["wall_ms"] >= 0 for row in rows))
        selected = [json.loads(line)["variant"] for line in subprocess.check_output(
            [str(REPLAY), str(path), "3", "1", "current-byte"], text=True, timeout=60).splitlines()]
        checks.assertEqual(selected, ["current", "byte"])
        for arguments in ([], [str(path), "3", "0"], [str(path), "3", "21"], [str(path), "3", "1", "unknown"]):
            checks.assertNotEqual(subprocess.run([str(REPLAY), *arguments], capture_output=True, timeout=30).returncode, 0)
        for contents in (b"", b"123"):
            path.write_bytes(contents)
            checks.assertNotEqual(subprocess.run([str(REPLAY), str(path), "3", "1"],
                                                capture_output=True, timeout=30).returncode, 0)

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--replay", type=Path)
    args = parser.parse_args()
    REPLAY = args.replay.resolve(strict=True) if args.replay else None
    suite = unittest.TestSuite([
        unittest.FunctionTestCase(test_capture_requires_fresh_output_and_validates_both_sources),
        unittest.FunctionTestCase(test_replay_checks_all_variants_and_rejects_invalid_inputs),
    ])
    raise SystemExit(not unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful())

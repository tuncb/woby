"""Verify smoke-test deployment layouts and asynchronous status polling."""
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from update_helper_smoke import run_parent, stage_template


def test_platform_layouts():
    for bundle in (False, True):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            binaries = base / "bin"
            app_directory = binaries / "woby.app/Contents/MacOS" if bundle else binaries
            resources = binaries / "woby.app/Contents/Resources" if bundle else binaries
            assets = resources / "assets"
            files = {
                app_directory / "woby": b"app",
                binaries / "woby-update-helper": b"helper",
                binaries / "libcrypto.dylib": b"helper dependency",
                app_directory / "libSDL3.dylib": b"app dependency",
                assets / "fonts/font.ttf": b"font",
                assets / "shaders/metal/vs_mesh.bin": b"shader",
            }
            for path, content in files.items():
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(content)
            template = base / "template"
            stage_template(app_directory / "woby", binaries / "woby-update-helper", assets, template)
            expected = {
                "woby": b"app", "woby-update-helper": b"helper",
                "libcrypto.dylib": b"helper dependency", "libSDL3.dylib": b"app dependency",
                "assets/fonts/font.ttf": b"font", "assets/shaders/metal/vs_mesh.bin": b"shader",
            }
            actual = {path.relative_to(template).as_posix(): path.read_bytes()
                      for path in template.rglob("*") if path.is_file()}
            assert actual == expected, (bundle, actual)


class HelperFixtureTests(unittest.TestCase):
    def test_platform_layouts(self):
        test_platform_layouts()

    def test_status_polling_retries_access_denied(self):
        for state in ("completed", "failed"):
            with self.subTest(state=state), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary).resolve()
                expected = {"state": state, "message": "helper result"}
                reads = [json.dumps({"state": "pending"}), PermissionError("status replacement"),
                         json.dumps({"state": "applying"}), PermissionError("status replacement"),
                         json.dumps(expected)]
                with patch("update_helper_smoke.subprocess.run"), \
                     patch.object(Path, "read_text", side_effect=reads), \
                     patch("update_helper_smoke.time.sleep"):
                    self.assertEqual(run_parent(root / "helper", root, root / "job"), expected)

    def test_status_polling_access_denied_has_bounded_retries(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            error = PermissionError("status remains inaccessible")
            with patch("update_helper_smoke.subprocess.run"), \
                 patch.object(Path, "read_text", side_effect=error) as read, \
                 patch("update_helper_smoke.time.sleep") as sleep:
                with self.assertRaisesRegex(RuntimeError, "helper did not finish") as raised:
                    run_parent(root / "helper", root, root / "job")
                self.assertIs(raised.exception.__cause__, error)
                self.assertEqual(read.call_count, 600)
                self.assertEqual(sleep.call_count, 600)

    def test_status_polling_does_not_hide_invalid_metadata(self):
        for contents, error in (("invalid JSON", json.JSONDecodeError), ("{}", KeyError)):
            with self.subTest(contents=contents), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary).resolve()
                with patch("update_helper_smoke.subprocess.run"), \
                     patch.object(Path, "read_text", return_value=contents), \
                     patch("update_helper_smoke.time.sleep") as sleep:
                    with self.assertRaises(error):
                        run_parent(root / "helper", root, root / "job")
                    sleep.assert_not_called()


if __name__ == "__main__":
    unittest.main()

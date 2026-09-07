"""Verify smoke-test deployments built from flat and macOS bundle layouts."""
from pathlib import Path
import tempfile

from update_helper_smoke import stage_template


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


if __name__ == "__main__":
    test_platform_layouts()
    print("Flat and macOS bundle template tests passed.")

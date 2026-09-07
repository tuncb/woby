import importlib.util
import json
import tempfile
import zipfile
from pathlib import Path

spec = importlib.util.spec_from_file_location('package_manifest', Path(__file__).with_name('package_manifest.py'))
package = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package)


def expect_error(function, *args):
    try:
        function(*args)
    except ValueError:
        return
    raise AssertionError('Expected invalid package metadata to be rejected')


def test_tag_must_match_built_version():
    package.validate_tag('v' + package.repository_version())
    for tag in ('v999.0.0', 'v1.2.3-rc1', '1.2.3', 'v01.2.3'):
        expect_error(package.validate_tag, tag)


def test_portable_paths():
    assert package.valid_path('assets/fonts/font.ttf')
    for path in ('../escape', 'a/../escape', '/root', 'a\\b', 'CON.txt', 'a/', 'a//b', 'file:stream'):
        assert not package.valid_path(path), path


def test_manifest_and_archive_validation():
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary) / 'package'
        names = ['woby.exe', 'woby-update-helper.exe', 'assets/fonts/RobotoMonoNerdFont-Regular.ttf']
        names += [f'assets/shaders/dx11/{stage}_{name}.bin' for stage in ('vs', 'fs')
                  for name in ('mesh', 'color', 'comparison', 'imgui', 'point_sprite')]
        for name in names:
            path = root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(name.encode())
        manifest = package.collect_manifest(root, 'windows-x64', '1.2.3')
        assert len(manifest['files']) == len(names)
        assert next(file for file in manifest['files'] if file['path'] == 'woby.exe')['executable']
        archive = Path(temporary) / 'package.zip'
        def write_zip(corrupt=False, unlisted=False):
            with zipfile.ZipFile(archive, 'w') as output:
                output.writestr('woby-windows-x64/' + package.MANIFEST, json.dumps(manifest))
                for name in names:
                    output.writestr('woby-windows-x64/' + name, b'corrupt' if corrupt else (root / name).read_bytes())
                if unlisted:
                    output.writestr('woby-windows-x64/unlisted', b'unexpected')
        write_zip()
        package.verify_archive(archive, 'windows-x64', '1.2.3')
        expect_error(package.verify_archive, archive, 'windows-x64', '1.2.4')
        write_zip(corrupt=True)
        expect_error(package.verify_archive, archive, 'windows-x64', '1.2.3')
        write_zip(unlisted=True)
        expect_error(package.verify_archive, archive, 'windows-x64', '1.2.3')
        (root / 'assets/shaders/dx11/vs_mesh.bin').unlink()
        expect_error(package.collect_manifest, root, 'windows-x64', '1.2.3')


if __name__ == '__main__':
    for test in (test_tag_must_match_built_version, test_portable_paths, test_manifest_and_archive_validation):
        test()
        print(test.__name__ + ': passed')

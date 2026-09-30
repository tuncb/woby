import hashlib
import importlib.util
import json
import tarfile
import tempfile
import zipfile
from pathlib import Path
from unittest.mock import patch

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
        names += [f'assets/shaders/spirv/{name}.bin' for name in package.NATIVE_SHADERS]
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
        # Correctly hashed archives must still contain the native renderer assets.
        for missing in ('assets/shaders/spirv/cs_marker_lookup_msaa.bin', 'woby.exe',
                        'woby-update-helper.exe', 'assets/fonts/RobotoMonoNerdFont-Regular.ttf'):
            original_files = manifest['files']
            manifest['files'] = [file for file in original_files if file['path'] != missing]
            names.remove(missing)
            write_zip()
            expect_error(package.verify_archive, archive, 'windows-x64', '1.2.3')
            manifest['files'] = original_files
            names.append(missing)
        for reserved in ('importers', 'importers/off/importer.json', 'IMPORTERS/off/plugin.dll'):
            path = root / reserved
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes((root / manifest['files'][0]['path']).read_bytes())
            expect_error(package.collect_manifest, root, 'windows-x64', '1.2.3')
            # Even a correctly hashed, listed entry cannot claim user importer files.
            entry = dict(manifest['files'][0], path=reserved)
            manifest['files'].append(entry)
            names.append(reserved)
            write_zip()
            expect_error(package.verify_archive, archive, 'windows-x64', '1.2.3')
            names.pop()
            manifest['files'].pop()
            path.unlink()
        (root / 'assets/shaders/spirv/cs_marker_lookup_msaa.bin').unlink()
        expect_error(package.collect_manifest, root, 'windows-x64', '1.2.3')


def write_fixture_package(root, platform):
    for name in package.required_release_files(platform):
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(name.encode())
    return package.collect_manifest(root, platform, '1.2.3')


def write_fixture_archive(root, manifest, archive):
    (root / package.MANIFEST).write_text(json.dumps(manifest), encoding='utf-8')
    names = [package.MANIFEST] + [file['path'] for file in manifest['files']]
    prefix = 'woby-' + manifest['platform'] + '/'
    if archive.suffix == '.zip':
        with zipfile.ZipFile(archive, 'w') as output:
            for name in names:
                output.write(root / name, prefix + name)
    else:
        with tarfile.open(archive, 'w:gz') as output:
            for name in names:
                output.add(root / name, arcname=prefix + name)


def test_native_packages_for_all_platforms():
    for platform, extension in package.PLATFORMS.items():
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve() / 'package'
            manifest = write_fixture_package(root, platform)
            before = {path.relative_to(root).as_posix(): path.read_bytes()
                      for path in root.rglob('*') if path.is_file()}
            assert manifest == package.collect_manifest(root, platform, '1.2.3')
            assert {file['path'] for file in manifest['files']} == package.required_release_files(platform)
            with (patch.object(package, 'repository_version', return_value='1.2.3'),
                  patch.object(package.subprocess, 'run',
                               return_value=package.subprocess.CompletedProcess([], 0, stdout='1.2.3\n'))):
                package.generate_manifest(root, platform)
            assert json.loads((root / package.MANIFEST).read_text()) == manifest
            assert {path.relative_to(root).as_posix(): path.read_bytes()
                    for path in root.rglob('*') if path.is_file() and path.name != package.MANIFEST} == before
            assert not (root / 'assets/shaders/dx11').exists()
            assert not (root / 'assets/shaders/glsl').exists()
            archive = Path(temporary) / ('package' + extension)
            write_fixture_archive(root, manifest, archive)
            package.verify_archive(archive, platform, '1.2.3')


def test_obsolete_shader_files_rejected_in_packages_and_archives():
    for platform, extension in package.PLATFORMS.items():
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve() / 'package'
            manifest = write_fixture_package(root, platform)
            archive = Path(temporary) / ('package' + extension)
            for name in ('assets/shaders/dx11/vs_mesh.bin', 'assets/shaders/glsl/fs_mesh.bin',
                         'ASSETS/SHADERS/DX11/vs_mesh.bin', 'assets/shaders/GLSL/fs_mesh.bin'):
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                content = b'obsolete compatibility copy'
                path.write_bytes(content)
                expect_error(package.collect_manifest, root, platform, '1.2.3')
                # Reused artifacts must be rejected even with correct ownership and hashes.
                manifest['files'].append({'path': name, 'size': len(content),
                                          'sha256': hashlib.sha256(content).hexdigest(), 'executable': False})
                write_fixture_archive(root, manifest, archive)
                expect_error(package.verify_archive, archive, platform, '1.2.3')
                manifest['files'].pop()
                path.unlink()


if __name__ == '__main__':
    for test in (test_tag_must_match_built_version, test_portable_paths, test_manifest_and_archive_validation,
                 test_native_packages_for_all_platforms,
                 test_obsolete_shader_files_rejected_in_packages_and_archives):
        test()
        print(test.__name__ + ': passed')

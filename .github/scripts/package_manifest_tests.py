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
        names += [f'assets/shaders/spirv/{name}.bin' for name in package.NATIVE_SHADERS]
        for name in names:
            path = root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(name.encode())
        manifest = package.prepare_manifest(root, 'windows-x64', '1.2.3')
        names += [f'assets/shaders/dx11/{name}.bin' for name in package.LEGACY_SHADERS]
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
        # Correctly hashed archives must still contain both the real renderer's
        # assets and the paths required by already-installed legacy updaters.
        for missing in ('assets/shaders/dx11/vs_mesh.bin', 'assets/shaders/spirv/cs_marker_lookup_msaa.bin', 'woby.exe'):
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


def test_compatibility_packages_for_all_platforms():
    for platform, legacy, native in (('windows-x64', 'dx11', 'spirv'),
                                     ('linux-x64', 'glsl', 'spirv'),
                                     ('macos-arm64', 'metal', 'metal')):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            for name in package.required_release_files(platform, compatibility=False):
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(name.encode())
            before = {path.relative_to(root).as_posix(): path.read_bytes()
                      for path in root.rglob('*') if path.is_file()}
            if legacy != native:
                expect_error(package.collect_manifest, root, platform, '1.2.3')
            manifest = package.prepare_manifest(root, platform, '1.2.3')
            assert manifest == package.prepare_manifest(root, platform, '1.2.3')
            assert {file['path'] for file in manifest['files']} == package.required_release_files(platform)
            for name, content in before.items():
                assert (root / name).read_bytes() == content
            for name in package.LEGACY_SHADERS:
                assert (root / f'assets/shaders/{legacy}/{name}.bin').read_bytes() == \
                       (root / f'assets/shaders/{native}/{name}.bin').read_bytes()
            if legacy != native:
                # Repackaging must refresh aliases after shader changes.
                (root / f'assets/shaders/{native}/vs_mesh.bin').write_bytes(b'changed shader')
                package.prepare_manifest(root, platform, '1.2.3')
                assert (root / f'assets/shaders/{legacy}/vs_mesh.bin').read_bytes() == b'changed shader'


if __name__ == '__main__':
    for test in (test_tag_must_match_built_version, test_portable_paths, test_manifest_and_archive_validation,
                 test_compatibility_packages_for_all_platforms):
        test()
        print(test.__name__ + ': passed')

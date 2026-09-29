"""Offline migration using a real pre-migration viewer and update helper.

Usage: update_legacy_smoke.py NEW_VIEWER NEW_HELPER NEW_ASSETS LEGACY_BIN_DIR
The historical binaries are supplied explicitly; this test never downloads or
updates a user's installation. Both local builds can have the same version, so
only the installed fixture manifest is labelled 0.0.0 to exercise the handoff.
The download/GitHub discovery step is not exercised by this offline test.
"""
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from update_helper_smoke import load_packaging, run_parent, stage_template, write_json


def fixture_manifest(root, platform, version):
    files = []
    for path in sorted(root.rglob('*')):
        if path.is_file():
            name = path.relative_to(root).as_posix()
            files.append({'path': name, 'size': path.stat().st_size,
                          'sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
                          'executable': name.endswith('.exe') if os.name == 'nt' else bool(path.stat().st_mode & 0o100)})
    return {'schema': 1, 'platform': platform, 'version': version, 'files': files}


def verify_files(root, manifest):
    for file in manifest['files']:
        data = (root / file['path']).read_bytes()
        assert len(data) == file['size'], file['path']
        assert hashlib.sha256(data).hexdigest() == file['sha256'], file['path']


def query_status(binary, state):
    result = subprocess.run([str(binary), 'update', '--status', '--json'],
                            capture_output=True, text=True, timeout=15)
    assert result.returncode == (1 if state == 'failed' else 0), result.stderr
    assert json.loads(result.stdout)['state'] == state, result.stdout


def apply_with_installed_helper(root, template, job, helper_name):
    (job / 'helper').mkdir(parents=True)
    for source in template.iterdir():
        if source.is_file() and (source.name == helper_name or source.suffix in ('.dll', '.dylib') or '.so' in source.name):
            shutil.copy2(source, job / 'helper' / source.name)
    write_json(root / '.woby-update/status.json',
               {'state': 'pending', 'job': job.name, 'message': 'offline migration test'})
    status = run_parent(job / 'helper' / helper_name, root, job)
    # Status is committed just before the detached helper exits.
    time.sleep(.2)
    return status


def main():
    package = load_packaging()
    binary, helper, assets, legacy_directory = [Path(arg).resolve() for arg in sys.argv[1:]]
    platform = 'windows-x64' if os.name == 'nt' else 'macos-arm64' if sys.platform == 'darwin' else 'linux-x64'
    legacy_shader = {'windows-x64': 'dx11', 'linux-x64': 'glsl', 'macos-arm64': 'metal'}[platform]
    version = subprocess.check_output([str(binary), '--version'], text=True, timeout=15).strip()
    old_version = subprocess.check_output([str(legacy_directory / binary.name), '--version'], text=True, timeout=15).strip()
    print(f'Historical viewer/helper: {legacy_directory} (viewer {old_version}); new viewer: {version}', flush=True)
    with tempfile.TemporaryDirectory(prefix='woby-legacy-update-') as temporary:
        base = Path(temporary).resolve()
        old_template, new_template = base / 'old', base / 'new'
        stage_template(legacy_directory / binary.name, legacy_directory / helper.name,
                       legacy_directory / 'assets', old_template)
        stage_template(binary, helper, assets, new_template)
        package.prepare_manifest(new_template, platform, version)
        for scenario in ('missing-compatibility', 'corrupt-download', 'health-failure', 'success'):
            root = base / (scenario + ' deployment ünicode')
            shutil.copytree(old_template, root)
            old_manifest = fixture_manifest(root, platform, '0.0.0')
            write_json(root / package.MANIFEST, old_manifest)
            for name in package.LEGACY_SHADERS:
                assert (root / f'assets/shaders/{legacy_shader}/{name}.bin').is_file()
            user_files = {'my-scene.woby': 'user scene', 'plugins/custom.dll': 'user plugin',
                          'importers/custom/settings.json': 'user settings'}
            for name, content in user_files.items():
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(content)
            query_status(root / binary.name, 'none')  # Actual legacy CLI, no network.
            job = root / '.woby-update/job-0123456789abcdef0123456789abcdef'
            shutil.copytree(new_template, job / 'package')
            new_manifest = package.collect_manifest(job / 'package', platform, version)
            if scenario == 'missing-compatibility':
                name = f'assets/shaders/{legacy_shader}/vs_mesh.bin'
                (job / 'package' / name).unlink()
                new_manifest['files'] = [file for file in new_manifest['files'] if file['path'] != name]
            elif scenario == 'health-failure':
                new_manifest['version'] = f'{int(version.split(".")[0]) + 1}.0.0'
            write_json(job / 'package' / package.MANIFEST, new_manifest)
            if scenario == 'corrupt-download':
                (job / 'package' / 'assets/fonts/RobotoMonoNerdFont-Regular.ttf').write_bytes(b'corrupt')
            status = apply_with_installed_helper(root, old_template, job, helper.name)
            success = scenario == 'success'
            expected = 'completed' if success else 'failed'
            assert status['state'] == expected, status
            if scenario == 'missing-compatibility':
                assert 'Package is missing assets/shaders/' in status['message'], status
                assert not (job / 'journal.json').exists()
            elif scenario == 'corrupt-download':
                assert 'missing or modified' in status['message'], status
                assert not (job / 'journal.json').exists()
            elif scenario == 'health-failure':
                assert (job / 'backup-complete.json').exists(), status
            installed = json.loads((root / package.MANIFEST).read_text())
            assert installed == (new_manifest if success else old_manifest)
            verify_files(root, installed)
            if not success:
                old_paths = {file['path'] for file in old_manifest['files']}
                for file in new_manifest['files']:
                    if file['path'] not in old_paths:
                        assert not (root / file['path']).exists(), file['path']
            query_status(root / binary.name, expected)
            for name, content in user_files.items():
                assert (root / name).read_text() == content
            if success and legacy_shader != 'metal':
                # The newly installed helper accepts a subsequent native-only
                # package and removes the now-obsolete compatibility files.
                installed['version'] = '0.0.0'
                write_json(root / package.MANIFEST, installed)
                next_job = root / '.woby-update/job-1123456789abcdef0123456789abcdef'
                shutil.copytree(new_template, next_job / 'package')
                for name in package.LEGACY_SHADERS:
                    (next_job / 'package' / f'assets/shaders/{legacy_shader}/{name}.bin').unlink()
                next_manifest = package.collect_manifest(next_job / 'package', platform, version, compatibility=False)
                write_json(next_job / 'package' / package.MANIFEST, next_manifest)
                status = apply_with_installed_helper(root, new_template, next_job, helper.name)
                assert status['state'] == 'completed', status
                verify_files(root, next_manifest)
                for name in package.LEGACY_SHADERS:
                    assert not (root / f'assets/shaders/{legacy_shader}/{name}.bin').exists()
                for name, content in user_files.items():
                    assert (root / name).read_text() == content
                query_status(root / binary.name, 'completed')
            print(scenario + ': passed', flush=True)


if __name__ == '__main__':
    main()

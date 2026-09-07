"""Exercise real GUI processes, inherited streams, and a hidden parent console."""
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

app, probe, parent, version = sys.argv[1:]
for executable in (app, probe):
    binary = Path(executable).read_bytes()
    pe = struct.unpack_from('<I', binary, 0x3C)[0]
    assert struct.unpack_from('<H', binary, pe + 24 + 68)[0] == 2, executable

def run(args, **kwargs):
    return subprocess.run(args, capture_output=True, text=True, timeout=30, **kwargs)

result = run([probe, '--detached-parent'])
assert result.returncode == 0, result

# A console parent with a hidden window exercises AttachConsole and verifies
# stdout/stderr actually reach the console's output buffer.
startup = subprocess.STARTUPINFO()
startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
startup.wShowWindow = subprocess.SW_HIDE
result = run([parent, probe], creationflags=subprocess.CREATE_NEW_CONSOLE, startupinfo=startup)
assert result.returncode == 0, result

result = run([probe, '--streams'], input='input marker\n')
assert result.returncode == 0, result
assert result.stdout == 'input marker\nstdout marker\n', result
assert result.stderr == 'stderr marker\n', result

with tempfile.TemporaryDirectory(prefix='woby streams ') as directory:
    output = Path(directory) / 'output.txt'
    error = Path(directory) / 'error.txt'
    with output.open('w') as stdout, error.open('w') as stderr:
        result = subprocess.run([probe, '--streams'], input='file marker\n', text=True,
                                stdout=stdout, stderr=stderr, timeout=30)
    assert result.returncode == 0, result
    assert output.read_text() == 'file marker\nstdout marker\n'
    assert error.read_text() == 'stderr marker\n'

result = run([app, '--version'])
assert result.returncode == 0 and result.stdout.strip() == version and not result.stderr, result
result = run([app, '--help'])
assert result.returncode == 0 and 'Usage:' in result.stdout and not result.stderr, result
result = run([app, '--unknown-option'])
assert result.returncode == 1 and result.stderr and not result.stdout, result
result = run([app, 'ctl', 'instances', '--json'])
assert result.returncode == 0 and isinstance(json.loads(result.stdout), list) and not result.stderr, result
result = run([app, 'ctl', '--instance', 'woby-launch-test-missing', 'objects', '--json'])
assert result.returncode == 1 and 'error' in json.loads(result.stdout) and not result.stderr, result
print('Windows GUI subsystem, console attachment, streams, and CLI checks passed.')

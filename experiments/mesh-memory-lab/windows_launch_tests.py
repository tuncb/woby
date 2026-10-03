"""Check console-free Windows launches while preserving redirected CLI output."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

app = str(Path(sys.argv[1]).resolve())
binary = Path(app).read_bytes()
pe = struct.unpack_from("<I", binary, 0x3C)[0]
assert binary[pe:pe + 4] == b"PE\0\0"
assert struct.unpack_from("<H", binary, pe + 24 + 68)[0] == 2, "Expected Windows GUI subsystem"

result = subprocess.run([app, "--help"], capture_output=True, text=True, timeout=10)
assert result.returncode == 0, result
assert "one internal folded-sheet example" in result.stdout, result
assert "--screenshot" in result.stdout and "--smoke" in result.stdout, result
assert not result.stderr, result
help_text = result.stdout
assert "--sample" not in help_text and "[file.obj]" not in help_text

result = subprocess.run([app, "--unknown-option"], capture_output=True, text=True, timeout=10)
assert result.returncode == 1, result
assert "Unknown argument: --unknown-option" in result.stderr, result
assert not result.stdout, result

# All fixture paths share one temporary root, independent of the build drive.
with tempfile.TemporaryDirectory(prefix="mesh memory lab launch ") as directory:
    root = Path(directory).resolve()
    output = root / "stdout.txt"
    error = root / "stderr.txt"
    with output.open("w") as stdout, error.open("w") as stderr:
        result = subprocess.run([app, "--help"], stdout=stdout, stderr=stderr,
                                cwd=root, timeout=10)
    assert result.returncode == 0, result
    assert output.read_text() == help_text
    assert error.read_text() == ""

print("Mesh memory lab GUI subsystem and redirected CLI output checks passed.")

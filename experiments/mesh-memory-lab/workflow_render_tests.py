"""Exercise saved workflows from a different working directory through the real UI."""
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile

app = Path(sys.argv[1]).resolve()
samples = Path(sys.argv[2]).resolve()
with tempfile.TemporaryDirectory(prefix="meshflow render ") as directory:
    root = Path(directory).resolve()
    library = root / "workflows"
    library.mkdir()
    for source in samples.glob("*.meshflow"):
        shutil.copyfile(source, library / source.name)

    if len(sys.argv) > 3 and sys.argv[3] == "--smoke":
        result = subprocess.run([str(app), "--workflows-dir", str(library), "--smoke"],
                                cwd=root, capture_output=True, text=True, timeout=45)
        assert result.returncode == 0, result
        print(result.stdout, end="")
        sys.exit(0)  # TemporaryDirectory still cleans up on exit.

    # A broken entry must not stop valid diagrams from being selected/rendered.
    (library / "broken.meshflow").write_text("{}", encoding="utf-8")
    cases = [
        ("01-obj-to-gpu.meshflow", "pack"),
        ("02-background-loading.meshflow", None),
        ("03-shared-quad.meshflow", "triangulate"),
        ("03-shared-quad.meshflow", "attributes"),
        ("03-shared-quad.meshflow", "upload"),
    ]
    for index, (filename, node) in enumerate(cases):
        image = root / f"workflow-{index}.png"
        args = [str(app), "--workflows-dir", str(library), "--workflow", filename,
                "--width", "1200", "--height", "860", "--screenshot", str(image)]
        if node:
            args += ["--node", node]
        result = subprocess.run(args, cwd=root, capture_output=True, text=True, timeout=30)
        assert result.returncode == 0, result
        data = image.read_bytes()
        assert data[:8] == b"\x89PNG\r\n\x1a\n"
        assert struct.unpack(">II", data[16:24]) == (1200, 860)
        assert len(data) > 10000, "Expected a rendered inspector, not an empty image"

    # Explicit invalid selection fails clearly without entering the render loop.
    for filename, message in (("missing.meshflow", "Workflow not found"),
                              ("broken.meshflow", "key 'format' not found")):
        result = subprocess.run([str(app), "--workflows-dir", str(library), "--workflow", filename,
                                 "--screenshot", str(root / "invalid.png")],
                                cwd=root, capture_output=True, text=True, timeout=10)
        assert result.returncode == 1 and message in result.stderr, result
        assert not (root / "invalid.png").exists()

print("Workflow UI: live mesh, diagram, missing attributes, custom folder, invalid entries passed.")

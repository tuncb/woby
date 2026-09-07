"""Generate verified portable-package manifests and validate release archives."""

import hashlib
import json
import re
import stat
import subprocess
import sys
import tarfile
import zipfile
from pathlib import Path, PurePosixPath

MANIFEST = "woby-manifest.json"
PLATFORMS = {"windows-x64": ".zip", "linux-x64": ".tar.gz", "macos-arm64": ".tar.gz"}
VERSION = re.compile(r"(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\Z")


def repository_version():
    root = Path(__file__).resolve().parents[2]
    match = re.search(r"project\(woby_obj_viewer VERSION ([0-9.]+)", (root / "CMakeLists.txt").read_text())
    if not match or not VERSION.fullmatch(match[1]):
        raise ValueError("Cannot determine CMake project version")
    if json.loads((root / "vcpkg.json").read_text())["version-string"] != match[1]:
        raise ValueError("CMake and vcpkg versions disagree")
    return match[1]


def validate_tag(tag):
    if tag != "v" + repository_version():
        raise ValueError("Release tag must match the CMake and vcpkg version")


def valid_path(name):
    if not name or len(name) > 240 or name.startswith("/"):
        return False
    for part in name.split("/"):
        if not part or part in (".", "..") or part.endswith((".", " ")):
            return False
        if any(ord(c) < 32 or ord(c) >= 127 or c in '\\:*?"<>|' for c in part):
            return False
        stem = part.split(".")[0].lower()
        if stem in ("con", "prn", "aux", "nul") or re.fullmatch(r"(?:com|lpt)[0-9]", stem):
            return False
    return True


def collect_manifest(root, platform, version):
    if platform not in PLATFORMS or not VERSION.fullmatch(version):
        raise ValueError("Invalid platform or version")
    files = []
    seen = set()
    for path in sorted(root.rglob("*")):
        if path.is_symlink():
            raise ValueError("Portable packages cannot contain symbolic links")
        if path.is_dir():
            continue
        name = path.relative_to(root).as_posix()
        if name == MANIFEST:
            continue
        if not valid_path(name) or name.lower().split("/")[0] == ".woby-update" or name.lower() in seen:
            raise ValueError("Invalid, reserved, or duplicate package path: " + name)
        if not path.is_file():
            raise ValueError("Package contains a special file")
        seen.add(name.lower())
        info = path.stat()
        files.append({"path": name, "size": info.st_size, "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                      "executable": bool(info.st_mode & stat.S_IXUSR) if platform != "windows-x64" else name.endswith(".exe")})
    suffix = ".exe" if platform == "windows-x64" else ""
    shader = {"windows-x64": "dx11", "linux-x64": "glsl", "macos-arm64": "metal"}[platform]
    required = {"woby" + suffix, "woby-update-helper" + suffix, "assets/fonts/RobotoMonoNerdFont-Regular.ttf"}
    required |= {f"assets/shaders/{shader}/{stage}_{name}.bin" for stage in ("vs", "fs")
                 for name in ("mesh", "color", "comparison", "imgui", "point_sprite")}
    if not {name.lower() for name in required}.issubset(seen):
        raise ValueError("Package is missing executable, font, or shader files")
    return {"schema": 1, "platform": platform, "version": version, "files": files}


def generate_manifest(root, platform):
    root = root.resolve()
    version = repository_version()
    suffix = ".exe" if platform == "windows-x64" else ""
    for binary in ("woby", "woby-update-helper"):
        result = subprocess.run([str(root / (binary + suffix)), "--version"], cwd=root,
                                check=True, capture_output=True, text=True, timeout=15)
        if result.stdout.strip() != version:
            raise ValueError(binary + " version does not match CMake")
    manifest = collect_manifest(root, platform, version)
    (root / MANIFEST).write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")


def verify_archive(path, platform, version):
    prefix = "woby-" + platform + "/"
    contents = {}
    def accept(name, stream, size):
        if not name.startswith(prefix) or not valid_path(name[len(prefix):]):
            raise ValueError("Unsafe archive path")
        name = name[len(prefix):]
        if name.lower() in {key.lower() for key in contents} or size > 512 * 1024 * 1024:
            raise ValueError("Duplicate or oversized archive entry")
        data = stream.read()
        if len(data) != size:
            raise ValueError("Truncated archive entry")
        contents[name] = data
    if path.suffix == ".zip":
        with zipfile.ZipFile(path) as archive:
            for entry in archive.infolist():
                if stat.S_ISLNK(entry.external_attr >> 16):
                    raise ValueError("Archive contains a link")
                if not entry.is_dir():
                    with archive.open(entry) as stream:
                        accept(entry.filename, stream, entry.file_size)
    else:
        with tarfile.open(path, "r:gz") as archive:
            for entry in archive:
                if entry.isdir():
                    continue
                if not entry.isfile():
                    raise ValueError("Archive contains a link or special file")
                with archive.extractfile(entry) as stream:
                    accept(entry.name, stream, entry.size)
    manifest = json.loads(contents[MANIFEST])
    if manifest["schema"] != 1 or manifest["platform"] != platform or manifest["version"] != version:
        raise ValueError("Archive manifest does not match the release")
    expected = {MANIFEST}
    for file in manifest["files"]:
        name = file["path"]
        if name in expected or not valid_path(name) or name.lower().split("/")[0] == ".woby-update":
            raise ValueError("Invalid manifest ownership")
        expected.add(name)
        if len(contents[name]) != file["size"] or hashlib.sha256(contents[name]).hexdigest() != file["sha256"]:
            raise ValueError("Archive file digest mismatch: " + name)
    if expected != contents.keys():
        raise ValueError("Archive contains unlisted files")


if __name__ == "__main__":
    if len(sys.argv) == 4 and sys.argv[1] == "generate":
        generate_manifest(Path(sys.argv[2]), sys.argv[3])
    elif len(sys.argv) == 3 and sys.argv[1] == "validate-tag":
        validate_tag(sys.argv[2])
    elif len(sys.argv) == 4 and sys.argv[1] == "verify-release":
        validate_tag(sys.argv[3])
        for platform, extension in PLATFORMS.items():
            verify_archive(Path(sys.argv[2]) / ("woby-" + platform + extension), platform, sys.argv[3][1:])
    else:
        raise SystemExit("Usage: package_manifest.py generate ROOT PLATFORM | validate-tag TAG | verify-release ROOT TAG")

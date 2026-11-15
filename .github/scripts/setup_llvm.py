"""Install the pinned upstream LLVM package, or select an existing installation."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import subprocess
import urllib.request


MANIFEST = Path(__file__).resolve().parents[2] / "cmake/llvm-toolchain.json"


def package_key(system, machine):
    architecture = {"amd64": "X64", "x86_64": "X64", "arm64": "ARM64", "aarch64": "ARM64"}.get(machine.lower())
    return f"{system}-{architecture}"


def compiler_paths(root, system):
    if system == "Windows":
        return {"CC": root / "bin/clang-cl.exe", "CXX": root / "bin/clang-cl.exe"}
    paths = {"CC": root / "bin/clang", "CXX": root / "bin/clang++"}
    if system == "Darwin":
        paths.update(OBJC=paths["CC"], OBJCXX=paths["CXX"])
    return paths


def verify_compilers(root, system, version):
    paths = compiler_paths(root, system)
    for compiler in set(paths.values()):
        output = subprocess.run([str(compiler), "--version"], check=True, capture_output=True, text=True).stdout
        match = re.match(r"clang version (\d+\.\d+\.\d+)(?:\s|$)", output)
        if not match or match[1] != version:
            raise RuntimeError(f"Expected upstream Clang {version} at {compiler}, found: {output.strip()}")
    return paths


def install_package(root, package, version):
    root.mkdir(parents=True, exist_ok=True)
    archive = root.parent / package["filename"]
    url = f"https://github.com/llvm/llvm-project/releases/download/llvmorg-{version}/{package['filename']}"
    try:
        urllib.request.urlretrieve(url, archive)
        with archive.open("rb") as stream:
            digest = hashlib.file_digest(stream, "sha256").hexdigest()
        if digest != package["sha256"]:
            raise RuntimeError(f"LLVM package checksum mismatch: {archive.name}")
        subprocess.run(["tar", "-xf", str(archive), "-C", str(root), "--strip-components=1"], check=True)
    finally:
        archive.unlink(missing_ok=True)


def platform_environment(system):
    if system != "Darwin":
        return {}
    sdk = subprocess.run(["xcrun", "--sdk", "macosx", "--show-sdk-path"],
                         check=True, capture_output=True, text=True).stdout.strip()
    if not Path(sdk).is_dir():
        raise RuntimeError(f"macOS SDK directory is missing: {sdk}")
    return {"SDKROOT": sdk}


def export_environment(root, compilers, environ, extra=None):
    values = {"WOBY_LLVM_ROOT": str(root), **{key: str(value) for key, value in compilers.items()}, **(extra or {})}
    if environ.get("GITHUB_ENV"):
        with Path(environ["GITHUB_ENV"]).open("a", encoding="utf-8") as output:
            for key, value in values.items():
                output.write(f"{key}={value}\n")
    if environ.get("GITHUB_PATH"):
        with Path(environ["GITHUB_PATH"]).open("a", encoding="utf-8") as output:
            output.write(f"{root / 'bin'}\n")
    return values


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    selection = parser.add_mutually_exclusive_group(required=True)
    selection.add_argument("--destination", type=Path, help="Download LLVM if this directory has no compiler")
    selection.add_argument("--root", type=Path, help="Verify an existing LLVM installation")
    args = parser.parse_args()
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    system = platform.system()
    key = package_key(system, platform.machine())
    if key not in manifest["packages"]:
        parser.error(f"Unsupported LLVM platform: {key}")
    root = (args.root or args.destination).resolve()
    package = manifest["packages"][key]
    stamp = root / ".woby-llvm-package"
    if args.destination and (not stamp.is_file() or stamp.read_text(encoding="utf-8").strip() != package["sha256"]):
        install_package(root, package, manifest["version"])
    compilers = verify_compilers(root, system, manifest["version"])
    if args.destination:
        stamp.write_text(package["sha256"] + "\n", encoding="utf-8")
    values = export_environment(root, compilers, os.environ, platform_environment(system))
    print(f"Selected LLVM {manifest['version']}")
    for name, value in values.items():
        print(f"{name}={value}")


if __name__ == "__main__":
    main()

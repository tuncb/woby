"""Test LLVM selection, package verification, and GitHub environment export."""

import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import setup_llvm


class LlvmSetupTests(unittest.TestCase):
    def test_platform_aliases_and_compiler_drivers(self):
        for machine in ("AMD64", "x86_64"):
            self.assertEqual(setup_llvm.package_key("Windows", machine), "Windows-X64")
        self.assertEqual(setup_llvm.package_key("Darwin", "aarch64"), "Darwin-ARM64")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.assertEqual(setup_llvm.compiler_paths(root, "Windows")["CXX"], root / "bin/clang-cl.exe")
            paths = setup_llvm.compiler_paths(root, "Darwin")
            self.assertEqual(paths["CXX"], paths["OBJCXX"])
            self.assertEqual(paths["CC"], paths["OBJC"])

    def test_accepts_only_the_exact_upstream_version(self):
        with tempfile.TemporaryDirectory() as directory:
            for output, accepted in [
                ("clang version 23.1.5 (revision)\nTarget: x86_64-pc-windows-msvc", True),
                ("clang version 23.1.4\n", False),
                ("clang version 23.1.50\n", False),
                ("Apple clang version 23.1.5\n", False),
                ("clang version 23.1.5git\n", False),
            ]:
                with self.subTest(output=output), patch.object(setup_llvm.subprocess, "run",
                        return_value=subprocess.CompletedProcess([], 0, stdout=output)):
                    if accepted:
                        setup_llvm.verify_compilers(Path(directory), "Windows", "23.1.5")
                    else:
                        with self.assertRaisesRegex(RuntimeError, "Expected upstream Clang"):
                            setup_llvm.verify_compilers(Path(directory), "Windows", "23.1.5")

    def test_checks_both_c_and_cxx_drivers(self):
        with tempfile.TemporaryDirectory() as directory, patch.object(setup_llvm.subprocess, "run",
                return_value=subprocess.CompletedProcess([], 0, stdout="clang version 23.1.5\n")) as run:
            root = Path(directory)
            setup_llvm.verify_compilers(root, "Darwin", "23.1.5")
            self.assertEqual({call.args[0][0] for call in run.call_args_list},
                             {str(root / "bin/clang"), str(root / "bin/clang++")})

    def test_checksum_failure_prevents_extraction_and_removes_archive(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "LLVM tools"
            def download(url, destination):
                destination.write_bytes(b"wrong package")
            with patch.object(setup_llvm.urllib.request, "urlretrieve", side_effect=download), \
                    patch.object(setup_llvm.subprocess, "run") as run:
                with self.assertRaisesRegex(RuntimeError, "checksum mismatch"):
                    setup_llvm.install_package(root, {"filename": "llvm.tar.zst", "sha256": "0" * 64}, "23.1.5")
                run.assert_not_called()
            self.assertFalse((root.parent / "llvm.tar.zst").exists())

    def test_verified_package_is_extracted_with_paths_containing_spaces(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "LLVM tools"
            payload = b"package fixture"
            def download(url, destination):
                self.assertIn("/llvmorg-23.1.5/", url)
                destination.write_bytes(payload)
            with patch.object(setup_llvm.urllib.request, "urlretrieve", side_effect=download), \
                    patch.object(setup_llvm.subprocess, "run") as run:
                setup_llvm.install_package(root, {"filename": "llvm.tar.zst", "sha256": hashlib.sha256(payload).hexdigest()}, "23.1.5")
                run.assert_called_once_with(["tar", "-xf", str(root.parent / "llvm.tar.zst"), "-C", str(root), "--strip-components=1"], check=True)
            self.assertFalse((root.parent / "llvm.tar.zst").exists())

    def test_export_keeps_paths_intact_and_appends_environment(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "LLVM tools"
            env_file = Path(directory) / "github-env"
            path_file = Path(directory) / "github-path"
            env_file.write_text("EXISTING=value\n", encoding="utf-8")
            values = setup_llvm.export_environment(root, setup_llvm.compiler_paths(root, "Darwin"),
                {"GITHUB_ENV": str(env_file), "GITHUB_PATH": str(path_file)})
            saved = dict(line.split("=", 1) for line in env_file.read_text(encoding="utf-8").splitlines())
            self.assertEqual(saved, {"EXISTING": "value", **values})
            self.assertEqual(path_file.read_text(encoding="utf-8").strip(), str(root / "bin"))

    def test_manifest_contains_every_ci_platform_with_a_checksum(self):
        manifest = json.loads(setup_llvm.MANIFEST.read_text(encoding="utf-8"))
        for key in ("Windows-X64", "Linux-X64", "Darwin-ARM64"):
            package = manifest["packages"][key]
            self.assertIn(manifest["version"], package["filename"])
            self.assertRegex(package["sha256"], r"^[a-f0-9]{64}$")


if __name__ == "__main__":
    unittest.main()

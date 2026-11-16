"""Exercise module dependency policy with isolated fixtures and the real scanner."""

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from check_systems import compiler_owner, load_inventory
from check_module_dependencies import depfile_paths, validate_headers, validate_imports


class ModuleCheckerTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="woby modules é ")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        for name in ["app/main.cpp", "tests/unit.cpp", "device/api/device.cppm",
                     "device/internal/device.cpp", "device/internal/detail.cppm",
                     "device/internal/private.h", "jobs/api/jobs.cppm", "jobs/internal/jobs.cpp"]:
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("", encoding="utf-8")
        (self.root / "systems.toml").write_text('''version = 1
[legacy]
roots = ["app", "tests"]
dependencies = ["device"]
[systems.device]
root = "device"
api = "api"
internal = "internal"
module = "example.device"
partitions = { detail = "internal/detail.cppm" }
implementation_dependencies = ["jobs", "legacy"]
tests = ["tests/unit.cpp"]
[systems.jobs]
root = "jobs"
api = "api"
internal = "internal"
module = "example.jobs"
''', encoding="utf-8")
        self.inventory = load_inventory(self.root)

    def check(self, source, *imports, provides=()):
        validate_imports(self.inventory, self.root / source,
                         {"requires": [{"logical-name": name} for name in imports],
                          "provides": [{"logical-name": name} for name in provides]})

    def test_allowed_direct_imports_and_owned_tests(self):
        self.check("app/main.cpp", "example.device")
        self.check("device/internal/device.cpp", "example.device", "example.device:detail", "example.jobs")
        self.check("tests/unit.cpp", "example.device:detail")

    def test_undeclared_and_transitive_imports_fail(self):
        for name in ["example.jobs", "unknown", "example.device:detail"]:
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, "undeclared"):
                self.check("app/main.cpp", name)

    def test_api_cannot_use_implementation_dependency_or_partition(self):
        for name in ["example.jobs", "example.device:detail"]:
            with self.subTest(name=name), self.assertRaises(ValueError):
                self.check("device/api/device.cppm", name, provides=["example.device"])

    def test_foreign_module_declaration_fails(self):
        with self.assertRaisesRegex(ValueError, "ownership"):
            self.check("app/main.cpp", provides=["example.device"])
        with self.assertRaisesRegex(ValueError, "ownership"):
            self.check("device/internal/detail.cppm", provides=["example.jobs"])

    def test_missing_module_declaration_fails(self):
        with self.assertRaisesRegex(ValueError, "declaration is missing"):
            self.check("device/api/device.cppm")

    def test_expected_compiler_names_include_test_ownership(self):
        self.assertEqual(compiler_owner(self.inventory, self.root / "tests/unit.cpp"), "example.device")
        self.assertEqual(compiler_owner(self.inventory, self.root / "device/internal/detail.cppm"), "example.device:detail")
        self.assertEqual(compiler_owner(self.inventory, self.root / "app/main.cpp"), "WobyLegacy")

    def test_duplicate_module_names_fail(self):
        manifest = self.root / "systems.toml"
        manifest.write_text(manifest.read_text().replace('module = "example.jobs"', 'module = "example.device"'))
        with self.assertRaisesRegex(ValueError, "duplicate module"):
            load_inventory(self.root)

    def test_partition_path_must_be_inside_a_classified_directory(self):
        path = self.root / "device/misplaced.cppm"
        path.write_text("module example.device:detail;\n")
        manifest = self.root / "systems.toml"
        manifest.write_text(manifest.read_text().replace('"internal/detail.cppm"', '"misplaced.cppm"'))
        with self.assertRaisesRegex(ValueError, "Invalid module partition"):
            load_inventory(self.root)

    def test_textual_includes_cannot_bypass_module_boundary(self):
        for name in ["device/internal/private.h", "device/api/device.cppm"]:
            with self.subTest(name=name), self.assertRaises(ValueError):
                validate_headers(self.inventory, self.root / "app/main.cpp", [self.root / name])
        validate_headers(self.inventory, self.root / "tests/unit.cpp", [self.root / "device/internal/private.h"])

    def test_legacy_headers_need_explicit_permission(self):
        validate_headers(self.inventory, self.root / "device/internal/device.cpp", [self.root / "app/main.cpp"])
        with self.assertRaisesRegex(ValueError, "legacy"):
            validate_headers(self.inventory, self.root / "device/api/device.cppm", [self.root / "app/main.cpp"])

    def test_make_paths_preserve_spaces_unicode_and_drive(self):
        source = self.root / "space é.h"
        spelling = source.as_posix().replace(" ", "\\ ")
        self.assertEqual(list(depfile_paths("woby_dependency: \\\n " + spelling, self.root)), [source])

    def test_real_scanner_reports_active_imports(self):
        llvm = Path(os.environ.get("WOBY_LLVM_ROOT", "C:/Program Files/LLVM")) / "bin"
        scanner = llvm / ("clang-scan-deps.exe" if os.name == "nt" else "clang-scan-deps")
        compiler = llvm / ("clang-cl.exe" if os.name == "nt" else "clang++")
        source = self.root / "app/main.cpp"
        source.write_text("#if ENABLE_JOBS\nimport example.jobs;\n#else\nimport example.device;\n#endif\n")
        for enabled in [0, 1]:
            args = [str(scanner), "-format=p1689", "--", str(compiler), "-std=c++20",
                    f"-DENABLE_JOBS={enabled}", "-c", str(source), "-o", str(self.root / "out.obj")]
            if os.name == "nt":
                args[args.index("-std=c++20")] = "/std:c++20"
            result = subprocess.run(args, cwd=self.root, capture_output=True, text=True, encoding="utf-8")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertNotIn("warning:", result.stderr)
            rule = json.loads(result.stdout)["rules"][0]
            if enabled:
                with self.assertRaisesRegex(ValueError, "undeclared"):
                    validate_imports(self.inventory, source, rule)
            else:
                validate_imports(self.inventory, source, rule)

    def test_compiler_rejects_forged_module_membership(self):
        llvm = Path(os.environ.get("WOBY_LLVM_ROOT", "C:/Program Files/LLVM")) / "bin"
        compiler = llvm / ("clang-cl.exe" if os.name == "nt" else "clang++")
        source = self.root / "app/main.cpp"
        source.write_text("export module example.device;\n")
        flags = (["/std:c++20", "/Zs", "/clang:-fmodule-name=WobyLegacy"] if os.name == "nt"
                 else ["-std=c++20", "-fsyntax-only", "-fmodule-name=WobyLegacy"])
        result = subprocess.run([str(compiler), *flags, str(source)], cwd=self.root, capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("does not match", result.stderr)

    def test_compiler_exposes_only_exported_names(self):
        llvm = Path(os.environ.get("WOBY_LLVM_ROOT", "C:/Program Files/LLVM")) / "bin"
        compiler = llvm / ("clang++.exe" if os.name == "nt" else "clang++")
        interface = self.root / "device/api/device.cppm"
        interface.write_text("export module example.device;\nint hidden() { return 1; }\nexport int visible() { return hidden(); }\n")
        bmi = self.root / "device.pcm"
        built = subprocess.run([str(compiler), "-std=c++20", "-Wall", "-Wextra", "-Werror", "--precompile",
                                str(interface), "-o", str(bmi)], capture_output=True, text=True)
        self.assertEqual(built.returncode, 0, built.stderr)
        source = self.root / "app/main.cpp"
        for function, succeeds in [("visible", True), ("hidden", False)]:
            source.write_text(f"import example.device;\nint main() {{ return {function}(); }}\n")
            result = subprocess.run([str(compiler), "-std=c++20", "-Werror", "-fsyntax-only",
                                     f"-fmodule-file=example.device={bmi}", str(source)], capture_output=True, text=True)
            self.assertEqual(result.returncode == 0, succeeds, result.stderr)


if __name__ == "__main__":
    unittest.main()

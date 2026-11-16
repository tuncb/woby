"""Exercise the system checker with isolated sources and the pinned Clang tools."""

import argparse
from contextlib import contextmanager
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


REPOSITORY = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("check_systems", REPOSITORY / "tools/check_systems.py")
checker = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = checker
spec.loader.exec_module(checker)
parser = argparse.ArgumentParser()
parser.add_argument("--llvm-root", type=Path, default=Path(os.environ.get("WOBY_LLVM_ROOT", "C:/Program Files/LLVM")))
options, remaining = parser.parse_known_args()
sys.argv = [sys.argv[0], *remaining]
LLVM = options.llvm_root.resolve() / "bin"
CL = os.name == "nt"
COMPILER = LLVM / ("clang-cl.exe" if CL else "clang++")
TIDY = LLVM / ("clang-tidy.exe" if CL else "clang-tidy")


@contextmanager
def fixture():
    with tempfile.TemporaryDirectory(prefix="woby systems é ") as directory:
        root = Path(directory).resolve()
        contents = {
            "systems.toml": '''version = 1
[legacy]
roots = ["app", "tests"]
dependencies = ["device"]
[systems.device]
root = "device"
api = "api"
internal = "internal"
api_dependencies = []
implementation_dependencies = ["legacy"]
tests = ["tests/unit.cpp"]
''',
            "device/api/device.h": "#pragma once\nnamespace device { struct State { int value; }; void run(State&); }\n",
            "device/internal/private.h": '#pragma once\n#include "../api/device.h"\nnamespace device { int secret(); struct Hidden { int n; }; inline constexpr int privateValue = 3; }\n',
            "device/internal/device.cpp": '#include "private.h"\nvoid device::run(State& state) { state.value = secret(); }\n',
            "app/main.cpp": '#include "../device/api/device.h"\nvoid use(device::State& state) { device::run(state); }\n',
            "tests/unit.cpp": '#include "../device/internal/private.h"\nint inspect() { return device::secret(); }\n',
        }
        for name, text in contents.items():
            path = root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="utf-8")
        yield root


def inventory(root):
    result = checker.load_inventory(root)
    checker.generate(result, root / "build/systems")
    return result


def record(root, source, owner=None):
    data = inventory(root)
    path = (root / source).resolve()
    args = [str(COMPILER)] + (["/nologo", "/std:c++20", "/W4", "/WX"] if CL else ["-std=c++20", "-Wall", "-Wextra", "-Werror"])
    args += ["-I" + str(root), "-I" + str(root / "device/api")]
    args += checker.boundary_flags(owner or data.files[path], root / "build/systems/systems.modulemap", CL)
    return {"directory": str(root / "build"), "file": str(path), "arguments": args + [str(path)]}


def compile_source(root, source, owner=None):
    command = record(root, source, owner)
    return checker.run_command(command["arguments"] + (["/Zs"] if CL else ["-fsyntax-only"]), root / "build")


def tidy_source(root, source):
    command = record(root, source)
    database = root / "build/compile_commands.json"
    database.write_text(json.dumps([command]), encoding="utf-8")
    scanner = LLVM / ("clang-scan-deps.exe" if CL else "clang-scan-deps")
    affected, error = checker.scan_inputs(checker.load_inventory(root), [command], scanner, database.parent,
                                          root / "build/systems", 1)
    assert not error, error
    return checker.run_command([str(TIDY), "--experimental-custom-checks", "--config-file=" + str(root / "build/systems/clang-tidy.json"),
                                "-p", str(database.parent), command["file"]], database.parent)


def test_public_api_and_own_internal_headers_compile():
    with fixture() as root:
        for source in ["app/main.cpp", "device/internal/device.cpp", "tests/unit.cpp"]:
            code, output = compile_source(root, source)
            assert code == 0, output
            assert "warning:" not in output, output


def test_foreign_internal_includes_fail_for_quotes_angles_macros_and_relative_paths():
    with fixture() as root:
        for directive in ['#include "../device/internal/private.h"', '#include <device/internal/private.h>',
                          '#define HEADER "../device/internal/private.h"\n#include HEADER',
                          '#include "../device/internal/../internal/private.h"']:
            (root / "app/main.cpp").write_text(directive + "\n", encoding="utf-8")
            code, output = compile_source(root, "app/main.cpp")
            assert code != 0 and "private header" in output, output


def test_api_dependency_must_be_declared():
    with fixture() as root:
        manifest = root / "systems.toml"
        manifest.write_text(manifest.read_text().replace('dependencies = ["device"]', 'dependencies = []'))
        code, output = compile_source(root, "app/main.cpp")
        assert code != 0 and "does not directly depend" in output, output


def test_foreign_implementation_source_cannot_be_included_as_a_header():
    with fixture() as root:
        (root / "app/main.cpp").write_text('#include "../device/internal/device.cpp"\n')
        code, output = compile_source(root, "app/main.cpp")
        assert code != 0 and "private header" in output, output


def test_api_isolation_rejects_internal_header_leaks():
    with fixture() as root:
        api = root / "device/api/device.h"
        api.write_text(api.read_text() + '#include "../internal/private.h"\n')
        data = inventory(root)
        commands = checker.api_checks(data, [record(root, "device/internal/device.cpp")],
                                      root / "build/systems", root / "build")
        code, output = checker.run_command(*commands[0])
        assert code != 0 and "private header" in output, output


def test_api_isolation_cannot_borrow_a_forced_include():
    with fixture() as root:
        api = root / "device/api/device.h"
        api.write_text("#pragma once\nvoid use(MissingType);\n")
        forced = root / "build/forced.h"
        data = inventory(root)
        forced.write_text("struct MissingType {};\n")
        command = record(root, "device/internal/device.cpp")
        command["arguments"] += (["/FI" + str(forced)] if CL else ["-include", str(forced)])
        commands = checker.api_checks(data, [command], root / "build/systems", root / "build")
        code, output = checker.run_command(*commands[0])
        assert code != 0 and "MissingType" in output, output


def test_tidy_rejects_leaked_internal_functions_addresses_types_fields_and_constants():
    with fixture() as root:
        api = root / "device/api/device.h"
        api.write_text(api.read_text() + '#include "../internal/private.h"\n')
        (root / "app/main.cpp").write_text('''#include "../device/api/device.h"
int use() {
    auto fn = &device::secret;
    device::Hidden hidden{1};
    return device::secret() + fn() + hidden.n + device::privateValue;
}
''')
        code, output = tidy_source(root, "app/main.cpp")
        assert code != 0, output
        for kind in ["declaration", "member", "type"]:
            assert f"custom-woby-device-{kind}" in output, output


def test_tidy_permits_api_calls_and_explicitly_owned_unit_tests():
    with fixture() as root:
        for source in ["app/main.cpp", "tests/unit.cpp", "device/internal/device.cpp"]:
            code, output = tidy_source(root, source)
            assert code == 0, output
            assert "custom-woby-" not in output, output


def test_other_test_files_do_not_get_internal_access():
    with fixture() as root:
        (root / "tests/other.cpp").write_text('#include "../device/internal/private.h"\n')
        code, output = compile_source(root, "tests/other.cpp")
        assert code != 0 and "private header" in output, output


def test_inactive_branches_are_not_claimed_as_checked():
    with fixture() as root:
        (root / "app/main.cpp").write_text('#if 0\n#include "../device/internal/private.h"\n#endif\n')
        code, output = compile_source(root, "app/main.cpp")
        assert code == 0, output


def test_unclassified_system_files_and_invalid_dependencies_are_errors():
    with fixture() as root:
        misplaced = root / "device/unclassified.h"
        misplaced.write_text("#pragma once\n")
        with unittest.TestCase().assertRaisesRegex(ValueError, "Unclassified"):
            checker.load_inventory(root)
        misplaced.unlink()
        manifest = root / "systems.toml"
        manifest.write_text(manifest.read_text().replace('api_dependencies = []', 'api_dependencies = ["missing"]'))
        with unittest.TestCase().assertRaisesRegex(ValueError, "dependency"):
            checker.load_inventory(root)


def test_api_cycles_and_paths_outside_the_project_are_errors():
    with fixture() as root:
        manifest = root / "systems.toml"
        manifest.write_text(manifest.read_text().replace('api_dependencies = []', 'api_dependencies = ["legacy"]'))
        with unittest.TestCase().assertRaisesRegex(ValueError, "cycle"):
            checker.load_inventory(root)
        with unittest.TestCase().assertRaisesRegex(ValueError, "project root"):
            checker.project_path(root, "../outside")


def test_cross_drive_manifest_paths_are_rejected():
    if not CL:
        raise unittest.SkipTest("Windows drive syntax")
    with fixture() as root:
        other_drive = "Z" if root.drive.upper() != "Z:" else "Y"
        with unittest.TestCase().assertRaisesRegex(ValueError, "relative project path"):
            checker.project_path(root, other_drive + ":/outside/header.h")


def test_command_cleanup_preserves_defines_includes_and_target_but_removes_outputs():
    with fixture() as root:
        source = str(root / "app/main.cpp")
        command = {"directory": str(root), "file": source,
                   "arguments": [str(COMPILER), "--driver-mode=cl", "/DTEST=1", "-I" + str(root),
                                 "/clang:-gen-cdb-fragment-path", "/clang:" + str(root / "fragments"),
                                 "-o", str(root / "old.obj"), "/Fo" + str(root / "old.obj"),
                                 "/Yu" + str(root / "pch.h"), "/Fp" + str(root / "pch.pch"),
                                 "/FI" + str(root / "pch.h"), "/c", source]}
        clean = checker.clean_arguments(command)
        assert clean == [str(COMPILER), "--driver-mode=cl", "/DTEST=1", "-I" + str(root),
                         "/FI" + str(root / "pch.h")], clean
        assert checker.clean_arguments(command, api=True) == clean[:-1]
        command["arguments"] = [str(COMPILER), "-DTEST=1", "-Xclang", "-include-pch", "-Xclang",
                                str(root / "pch.pch"), "-Xclang", "-include", "-Xclang",
                                str(root / "pch.h"), "-c", source]
        assert checker.clean_arguments(command) == [str(COMPILER), "-DTEST=1", "-Xclang", "-include",
                                                     "-Xclang", str(root / "pch.h")]
        assert checker.clean_arguments(command, api=True) == [str(COMPILER), "-DTEST=1"]
        if CL:
            command["arguments"] = [str(COMPILER), "-xc++", "/TP", "-fno-rtti", "-ffp-model=strict",
                                    "-include", str(root / "pch.h"), source]
            assert checker.clean_arguments(command) == [str(COMPILER), "-xc++", "/clang:-fno-rtti",
                                                         "/clang:-ffp-model=strict", "/FI" + str(root / "pch.h")]


def test_real_clang_fragments_are_collected_without_stale_or_missing_sources():
    with fixture() as root:
        data = inventory(root)
        directory = root / "build/systems/Release"
        fragments = directory / "commands/device"
        fragments.mkdir(parents=True)
        source = root / "device/internal/device.cpp"
        (directory / "targets.txt").write_text("device\n")
        (directory / "sources-device.txt").write_text(str(source) + "\n", encoding="utf-8")
        with unittest.TestCase().assertRaisesRegex(ValueError, "Build device"):
            checker.load_commands(data, directory)
        command = record(root, "device/internal/device.cpp")
        prefix = "/clang:" if CL else ""
        args = command["arguments"] + [prefix + "-gen-cdb-fragment-path", prefix + str(fragments)]
        args += (["/c", "/Fo" + str(root / "build/device.obj")] if CL else ["-c", "-o", str(root / "build/device.o")])
        code, output = checker.run_command(args, root / "build")
        assert code == 0, output
        commands = checker.load_commands(data, directory)
        assert len(commands) == 1 and Path(commands[0]["file"]) == source
        assert not any("cdb-fragment" in arg for arg in commands[0]["arguments"])
        # A removed source and a generated PCH cannot re-enter the checked set.
        stale = dict(command, file=str(root / "build/deleted.cpp"))
        # clang-cl can emit two records for one PCH compilation.
        (fragments / "stale.json").write_text((json.dumps(stale) + ",\n") * 2)
        assert len(checker.load_commands(data, directory)) == 1
        newest = dict(command, arguments=command["arguments"] + ["-DNEWEST=1"])
        latest = fragments / "latest.json"
        latest.write_text(json.dumps(newest) + ",\n")
        stamp = max(path.stat().st_mtime_ns for path in fragments.glob("*.json")) + 1_000_000_000
        os.utime(latest, ns=(stamp, stamp))
        assert "-DNEWEST=1" in checker.load_commands(data, directory)[0]["arguments"]
        newest["arguments"] = [arg.replace("-fmodule-name=Woby_device_Impl", "-fmodule-name=WobyLegacy")
                               for arg in newest["arguments"]]
        latest.write_text(json.dumps(newest) + ",\n")
        os.utime(latest, ns=(stamp, stamp))
        with unittest.TestCase().assertRaisesRegex(ValueError, "source ownership changed"):
            checker.load_commands(data, directory)


def test_cli_fails_instead_of_succeeding_without_build_commands():
    with fixture() as root:
        result = subprocess.run([sys.executable, str(REPOSITORY / "tools/check_systems.py"), "check",
                                 "--root", str(root), "--build", str(root / "build"),
                                 "--llvm-root", str(LLVM.parent)], cwd=root, text=True, capture_output=True)
        assert result.returncode == 2 and "Configure CMake first" in result.stderr, result.stdout + result.stderr


def test_cli_checks_captured_commands_and_rejects_boundary_violations():
    with fixture() as root:
        directory = root / "build/systems/Release"
        fragments = directory / "commands/fixture"
        fragments.mkdir(parents=True)
        sources = ["app/main.cpp", "device/internal/device.cpp", "tests/unit.cpp"]
        (directory / "targets.txt").write_text("fixture\n")
        (directory / "sources-fixture.txt").write_text(
            "".join(str(root / source) + "\n" for source in sources), encoding="utf-8")
        for index, source in enumerate(sources):
            command = record(root, source)
            prefix = "/clang:" if CL else ""
            args = command["arguments"] + [prefix + "-gen-cdb-fragment-path", prefix + str(fragments)]
            obj = str(root / f"build/fixture{index}.obj")
            args += (["/c", "/Fo" + obj] if CL else ["-c", "-o", obj])
            code, output = checker.run_command(args, root / "build")
            assert code == 0, output
        args = [sys.executable, str(REPOSITORY / "tools/check_systems.py"), "check",
                "--root", str(root), "--build", str(root / "build"), "--llvm-root", str(LLVM.parent)]
        code, output = checker.run_command(args, root)
        assert code == 0 and "0 failed" in output, output
        api = root / "device/api/device.h"
        api.write_text(api.read_text() + '#include "../internal/private.h"\n')
        (root / "app/main.cpp").write_text(
            '#include "../device/api/device.h"\nint use() { return device::secret(); }\n')
        code, output = checker.run_command(args, root)
        assert code == 1 and "private header" in output and "custom-woby-device-declaration" in output, output
        (root / "app/main.cpp").write_text('#include "../device/internal/private.h"\n')
        code, output = checker.run_command(args, root)
        assert code == 1 and "private header" in output, output


def suite():
    return unittest.TestSuite(unittest.FunctionTestCase(function) for name, function in sorted(globals().items())
                              if name.startswith("test_") and callable(function))


if __name__ == "__main__":
    unittest.main(defaultTest="suite")

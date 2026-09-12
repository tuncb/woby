"""Check local/CI suite selection through real CMake discovery and CTest."""
from dataclasses import dataclass
import json
from pathlib import Path
import subprocess
import sys
import tempfile


CMAKE, CTEST, SOURCE, DOCTEST_SCRIPT, GENERATOR, PLATFORM = sys.argv[1:]
SOURCE = Path(SOURCE)


@dataclass
class SuiteFixture:
    root: Path
    build: Path
    executable: Path
    fake: Path
    local_presets: list[str]


def bracket(value):
    return f"[==[{value}]==]"


def make_fixture(root):
    executable = root / "fake executable"
    executable.write_text("discovery placeholder", encoding="utf-8")
    fake = root / "fake_doctest.cmake"
    fake.write_text(
        f'file(APPEND {bracket((root / "calls.txt").as_posix())} "called\\n")\n'
        'execute_process(COMMAND "${CMAKE_COMMAND}" -E echo "ordinary case")\n'
        'execute_process(COMMAND "${CMAKE_COMMAND}" -E echo "slow case")\n',
        encoding="utf-8",
    )
    # Exercise the checked-in local presets against a fixture with no compiler.
    # Use the parent generator for both presets: filtering is generator independent.
    presets = json.loads((SOURCE / "CMakePresets.json").read_text(encoding="utf-8"))
    local_tests = presets["testPresets"]
    fixture = SuiteFixture(root, root / "build", executable, fake,
                           [preset["name"] for preset in local_tests])
    fixture_presets = {
        "version": presets["version"],
        "configurePresets": [
            {"name": preset["name"], "generator": GENERATOR, "binaryDir": fixture.build.as_posix()}
            for preset in local_tests
        ],
        "testPresets": [dict(preset, configuration="Debug") for preset in local_tests],
    }
    (root / "CMakePresets.json").write_text(json.dumps(fixture_presets), encoding="utf-8")
    return fixture


def configure(fixture, slow_case="slow case"):
    (fixture.root / "CMakeLists.txt").write_text(
        "cmake_minimum_required(VERSION 3.24)\n"
        "project(suite_selection LANGUAGES NONE)\n"
        "set(CMAKE_BUILD_TYPE Debug)\n"
        "enable_testing()\n"
        f"set(_DOCTEST_DISCOVER_TESTS_SCRIPT {bracket(DOCTEST_SCRIPT)})\n"
        f"include({bracket((SOURCE / 'cmake/WobyTestDiscovery.cmake').as_posix())})\n"
        "add_executable(fake_tests IMPORTED)\n"
        "set_target_properties(fake_tests PROPERTIES\n"
        f"  IMPORTED_LOCATION {bracket(fixture.executable.as_posix())}\n"
        f"  CROSSCOMPILING_EMULATOR {bracket(CMAKE + ';-P;' + fixture.fake.as_posix())})\n"
        f"woby_discover_tests(fake_tests SLOW_TESTS {bracket(slow_case)})\n",
        encoding="utf-8",
    )
    command = [CMAKE, "-S", str(fixture.root), "-B", str(fixture.build), "-G", GENERATOR]
    if PLATFORM:
        command += ["-A", PLATFORM]
    result = subprocess.run(command, capture_output=True, text=True, timeout=60)
    assert result.returncode == 0, result.stdout + result.stderr


def listing(fixture, *arguments):
    result = subprocess.run(
        [CTEST, *arguments, "--show-only=json-v1"], cwd=fixture.root,
        capture_output=True, text=True, timeout=30,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert "CMake Warning" not in result.stderr, result.stderr
    return {test["name"]: test for test in json.loads(result.stdout)["tests"]}


def test_local_and_ci_suites_partition_discovered_tests_with_cached_labels():
    with tempfile.TemporaryDirectory(prefix="woby suite selection ") as temporary:
        fixture = make_fixture(Path(temporary))
        configure(fixture)
        arguments = ["--test-dir", str(fixture.build), "-C", "Debug"]
        all_tests = listing(fixture, *arguments)
        assert set(all_tests) == {"ordinary case", "slow case"}
        for name in fixture.local_presets:
            assert set(listing(fixture, "--preset", name)) == {"ordinary case"}, name
        regular = listing(fixture, *arguments, "-LE", "^slow$")
        slow = listing(fixture, *arguments, "-L", "^slow$")
        assert set(regular) == {"ordinary case"}
        assert set(slow) == {"slow case"}
        assert not regular.keys() & slow.keys()
        assert regular.keys() | slow.keys() == all_tests.keys()
        assert (fixture.root / "calls.txt").read_text().splitlines() == ["called"]

        # Classification changes must update selection without rebuilding an executable.
        configure(fixture, slow_case="ordinary case")
        assert set(listing(fixture, *arguments, "-L", "^slow$")) == {"ordinary case"}
        assert set(listing(fixture, "--preset", fixture.local_presets[0])) == {"slow case"}


def test_discovery_include_works_without_project_policy_settings():
    with tempfile.TemporaryDirectory(prefix="woby suite selection ") as temporary:
        fixture = make_fixture(Path(temporary))
        configure(fixture)
        standalone = fixture.root / "standalone"
        standalone.mkdir()
        # CTest evaluates includes independently of the project's CMake policy scope.
        # A bare test file exercises that contract, including on CMake 3.x runners.
        discovery = fixture.build / "test-discovery/fake_tests/include.cmake"
        (standalone / "CTestTestfile.cmake").write_text(
            f"include({bracket(discovery.as_posix())})\n", encoding="utf-8"
        )
        arguments = ["--test-dir", str(standalone), "-C", "Debug"]
        assert set(listing(fixture, *arguments, "-LE", "^slow$")) == {"ordinary case"}
        assert set(listing(fixture, *arguments, "-L", "^slow$")) == {"slow case"}


def test_renamed_or_missing_slow_case_fails_discovery():
    with tempfile.TemporaryDirectory(prefix="woby suite selection ") as temporary:
        fixture = make_fixture(Path(temporary))
        configure(fixture, slow_case="missing case")
        result = subprocess.run(
            [CTEST, "--test-dir", str(fixture.build), "-C", "Debug", "--show-only=json-v1"],
            capture_output=True, text=True, timeout=30,
        )
        assert result.returncode != 0
        assert "missing case" in result.stdout + result.stderr


if __name__ == "__main__":
    for test in (test_local_and_ci_suites_partition_discovered_tests_with_cached_labels,
                 test_discovery_include_works_without_project_policy_settings,
                 test_renamed_or_missing_slow_case_fails_discovery):
        test()
        print(test.__name__ + ": passed")

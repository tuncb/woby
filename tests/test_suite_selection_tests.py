"""Check file grouping, suite selection, cache invalidation, and CTest execution."""
from collections import Counter
from dataclasses import dataclass
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


CMAKE, CTEST, SOURCE, DOCTEST_SCRIPT, GENERATOR, PLATFORM = sys.argv[1:]
SOURCE = Path(SOURCE)
SLOW_CASES = ("slow case", "slow GPU case", "only slow case")
SPECIAL_CASE = 'ordinary, quoted "value"; [=[content]=] Ω'


@dataclass
class SuiteFixture:
    root: Path
    build: Path
    executable: Path
    fake: Path
    module: Path
    cases: list
    presets: list


def bracket(value):
    return f"[====[{value}]====]"


def write_cases(fixture):
    (fixture.root / "cases.json").write_text(json.dumps(fixture.cases), encoding="utf-8")


def make_fixture(root):
    executable = root / "fake executable"
    executable.write_text("discovery placeholder", encoding="utf-8")
    fake = root / "fake doctest.py"
    shutil.copyfile(SOURCE / "tests/fake_doctest.py", fake)
    module = root / "module"
    module.mkdir()
    for name in ["WobyTestDiscovery.cmake", "DiscoverTestsIfNeeded.cmake", "DiscoverTestGroups.py"]:
        shutil.copyfile(SOURCE / "cmake" / name, module / name)
    shutil.copyfile(DOCTEST_SCRIPT, module / "doctestAddTests.cmake")
    (root / "runs").mkdir()
    cases = [
        {"name": "ordinary case", "filename": str(root / "sources/ordinary_tests.cpp")},
        {"name": SPECIAL_CASE, "filename": str(root / "sources/ordinary_tests.cpp")},
        {"name": "slow case", "filename": str(root / "sources/ordinary_tests.cpp")},
        {"name": "other case", "filename": str(root / "sources/other_tests.cpp")},
        {"name": "GPU case", "filename": str(root / "sources/example_gpu_tests.cpp")},
        {"name": "slow GPU case", "filename": str(root / "sources/example_gpu_tests.cpp")},
        {"name": "peer GPU case", "filename": str(root / "sources/peer_gpu_tests.cpp")},
        {"name": "only slow case", "filename": str(root / "sources/only_slow_tests.cpp")},
        {"name": "benchmark", "filename": str(root / "sources/benchmark_tests.cpp"), "skipped": True},
    ]
    # Use the parent generator for both local configurations. Test filters are
    # independent of the generator, and this fixture does not invoke a compiler.
    presets = json.loads((SOURCE / "CMakePresets.json").read_text(encoding="utf-8"))
    local_tests = presets["testPresets"]
    fixture = SuiteFixture(root, root / "build", executable, fake, module, cases, local_tests)
    write_cases(fixture)
    fixture_presets = {
        "version": presets["version"],
        "configurePresets": [
            {"name": name, "generator": GENERATOR, "binaryDir": fixture.build.as_posix()}
            for name in ("vs2026-vcpkg", "ninja-vcpkg")
        ],
        "testPresets": [dict(preset, configuration="Debug") for preset in local_tests],
    }
    (root / "CMakePresets.json").write_text(json.dumps(fixture_presets), encoding="utf-8")
    return fixture


def configure(fixture, slow_cases=SLOW_CASES, configuration="Debug"):
    (fixture.root / "CMakeLists.txt").write_text(
        "cmake_minimum_required(VERSION 3.24)\n"
        "project(suite_selection LANGUAGES NONE)\n"
        f"set(CMAKE_BUILD_TYPE {bracket(configuration)})\n"
        "enable_testing()\n"
        f"set(Python3_EXECUTABLE {bracket(sys.executable)})\n"
        f"set(_DOCTEST_DISCOVER_TESTS_SCRIPT {bracket((fixture.module / 'doctestAddTests.cmake').as_posix())})\n"
        f"include({bracket((fixture.module / 'WobyTestDiscovery.cmake').as_posix())})\n"
        "add_executable(fake_tests IMPORTED)\n"
        "set_target_properties(fake_tests PROPERTIES\n"
        f"  IMPORTED_LOCATION {bracket(fixture.executable.as_posix())}\n"
        f"  CROSSCOMPILING_EMULATOR {bracket(sys.executable + ';' + fixture.fake.as_posix())})\n"
        "woby_discover_tests(fake_tests GROUP_BY_SOURCE SLOW_TESTS "
        + " ".join(bracket(name) for name in slow_cases) + ")\n",
        encoding="utf-8",
    )
    command = [CMAKE, "-S", str(fixture.root), "-B", str(fixture.build), "-G", GENERATOR]
    if PLATFORM:
        command += ["-A", PLATFORM]
    result = subprocess.run(command, capture_output=True, text=True, timeout=60)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "CMake Warning" not in result.stderr, result.stderr


def ctest(fixture, *arguments):
    return subprocess.run([CTEST, *arguments], cwd=fixture.root,
                          capture_output=True, text=True, encoding="utf-8", timeout=60)


def listing(fixture, *arguments):
    result = ctest(fixture, *arguments, "--show-only=json-v1")
    assert result.returncode == 0, result.stdout + result.stderr
    assert "CMake Warning" not in result.stderr, result.stderr
    return {test["name"]: test for test in json.loads(result.stdout)["tests"]}


def properties(test):
    return {property["name"]: property["value"] for property in test.get("properties", [])}


def execution_records(fixture):
    return [json.loads(path.read_text(encoding="utf-8")) for path in (fixture.root / "runs").glob("*.json")]


def test_local_and_ci_suites_partition_groups_and_preserve_case_coverage():
    with tempfile.TemporaryDirectory(prefix="woby suite selection ") as temporary:
        fixture = make_fixture(Path(temporary))
        configure(fixture)
        arguments = ["--test-dir", str(fixture.build), "-C", "Debug"]
        all_tests = listing(fixture, *arguments)
        cpu_names = {"fake_tests::ordinary_tests", "fake_tests::other_tests"}
        gpu_names = {"fake_tests::example_gpu_tests", "fake_tests::peer_gpu_tests"}
        assert set(all_tests) == cpu_names | gpu_names | set(SLOW_CASES)
        for preset in fixture.presets:
            name = preset["name"]
            expected = set(SLOW_CASES) if name.endswith("-slow") else gpu_names if name.endswith("-graphics") else cpu_names
            assert set(listing(fixture, "--preset", name)) == expected, name
        regular = listing(fixture, *arguments, "-LE", "^(slow|graphics)$")
        slow = listing(fixture, *arguments, "-L", "^slow$")
        graphics = listing(fixture, *arguments, "-L", "^graphics$", "-LE", "^slow$")
        selections = [set(regular), set(slow), set(graphics)]
        assert set.union(*selections) == set(all_tests)
        assert sum(map(len, selections)) == len(all_tests)
        assert (fixture.root / "calls.txt").read_text().splitlines() == ["called"]
        for name, test in all_tests.items():
            props = properties(test)
            assert "unit" in props["LABELS"]
            if name in gpu_names or name == "slow GPU case":
                assert "graphics" in props["LABELS"]
                assert props["RESOURCE_LOCK"] == ["woby_gpu"]
            else:
                assert "cpu" in props["LABELS"]
                assert "RESOURCE_LOCK" not in props
        result = ctest(fixture, *arguments, "-j", "4", "--output-on-failure")
        assert result.returncode == 0, result.stdout + result.stderr
        records = execution_records(fixture)
        actual = Counter(name for record in records for name in record["names"])
        expected = Counter(case["name"] for case in fixture.cases if not case.get("skipped"))
        assert actual == expected, (actual, expected)
        gpu_cases = {case["name"] for case in fixture.cases if "_gpu_tests" in case["filename"]}
        gpu_records = [record for record in records if set(record["names"]) & gpu_cases]
        for index, first in enumerate(gpu_records):
            for second in gpu_records[index + 1:]:
                assert first["end"] <= second["start"] or second["end"] <= first["start"]


def test_slow_reclassification_preserves_escaped_case_names_without_rebuild():
    with tempfile.TemporaryDirectory(prefix="woby suite selection ") as temporary:
        fixture = make_fixture(Path(temporary))
        configure(fixture)
        arguments = ["--test-dir", str(fixture.build), "-C", "Debug"]
        listing(fixture, *arguments)
        configure(fixture, slow_cases=(SPECIAL_CASE, "slow GPU case"))
        assert set(listing(fixture, *arguments, "-L", "^slow$")) == {SPECIAL_CASE, "slow GPU case"}
        result = ctest(fixture, *arguments, "-j", "4", "--output-on-failure")
        assert result.returncode == 0, result.stdout + result.stderr
        actual = Counter(name for record in execution_records(fixture) for name in record["names"])
        assert actual == Counter(case["name"] for case in fixture.cases if not case.get("skipped"))


def test_discovery_include_works_without_project_policy_settings():
    with tempfile.TemporaryDirectory(prefix="woby suite selection ") as temporary:
        fixture = make_fixture(Path(temporary))
        configure(fixture)
        standalone = fixture.root / "standalone"
        standalone.mkdir()
        discovery = fixture.build / "test-discovery/fake_tests/include.cmake"
        (standalone / "CTestTestfile.cmake").write_text(
            f"include({bracket(discovery.as_posix())})\n", encoding="utf-8")
        assert set(listing(fixture, "--test-dir", str(standalone), "-C", "Debug", "-L", "^slow$")) == set(SLOW_CASES)


def test_missing_or_skipped_slow_case_fails_discovery():
    for name in ("missing case", "benchmark"):
        with tempfile.TemporaryDirectory(prefix="woby suite selection ") as temporary:
            fixture = make_fixture(Path(temporary))
            configure(fixture, slow_cases=(name,))
            result = ctest(fixture, "--test-dir", str(fixture.build), "-C", "Debug", "--show-only=json-v1")
            assert result.returncode != 0
            assert "Slow test was not discovered: " + name in result.stdout + result.stderr


def test_discovery_cache_tracks_configurations_binary_and_parser_changes():
    with tempfile.TemporaryDirectory(prefix="woby suite selection ") as temporary:
        fixture = make_fixture(Path(temporary))
        configure(fixture)
        arguments = ["--test-dir", str(fixture.build)]
        multi_config = any(line.startswith("CMAKE_CONFIGURATION_TYPES:")
                           for line in (fixture.build / "CMakeCache.txt").read_text().splitlines())
        configured = "Debug"
        cached_calls = 0
        for configuration, count in [("Debug", 1), ("Debug", 1), ("Release", 2), ("Debug", 2)]:
            if not multi_config and configuration != configured:
                # A single-configuration build ignores CTest's -C option.
                # It must keep the current cache until CMake is reconfigured.
                listing(fixture, *arguments, "-C", configuration)
                actual = len((fixture.root / "calls.txt").read_text().splitlines())
                assert actual == cached_calls, (configuration, actual, cached_calls)
                configure(fixture, configuration=configuration)
                configured = configuration
            listing(fixture, *arguments, "-C", configuration)
            actual = len((fixture.root / "calls.txt").read_text().splitlines())
            assert actual == count, (configuration, actual, count, multi_config)
            cached_calls = count
        debug_cache = fixture.build / "test-discovery/fake_tests/Debug/tests.cmake"
        debug_cache.unlink()
        listing(fixture, *arguments, "-C", "Debug")
        assert len((fixture.root / "calls.txt").read_text().splitlines()) == 3
        # A new compiled case must appear in the next CTest invocation.
        fixture.cases.append({"name": "new case", "filename": str(fixture.root / "sources/new_tests.cpp")})
        write_cases(fixture)
        fixture.executable.write_text("rebuilt binary", encoding="utf-8")
        assert "fake_tests::new_tests" in listing(fixture, *arguments, "-C", "Debug")
        assert len((fixture.root / "calls.txt").read_text().splitlines()) == 4
        for name, count in [("DiscoverTestGroups.py", 5), ("DiscoverTestsIfNeeded.cmake", 6), ("doctestAddTests.cmake", 7)]:
            with (fixture.module / name).open("a", encoding="utf-8") as script:
                script.write("\n# parser changed\n")
            listing(fixture, *arguments, "-C", "Debug")
            assert len((fixture.root / "calls.txt").read_text().splitlines()) == count


def test_duplicate_source_basenames_have_separate_exact_filters():
    with tempfile.TemporaryDirectory(prefix="woby suite selection ") as temporary:
        fixture = make_fixture(Path(temporary))
        fixture.cases.append({"name": "duplicate basename case", "filename": str(fixture.root / "other sources/other_tests.cpp")})
        write_cases(fixture)
        configure(fixture)
        arguments = ["--test-dir", str(fixture.build), "-C", "Debug"]
        tests = listing(fixture, *arguments)
        assert len([name for name in tests if name.startswith("fake_tests::other_tests::")]) == 2
        result = ctest(fixture, *arguments, "-j", "4", "--output-on-failure")
        assert result.returncode == 0, result.stdout + result.stderr
        actual = Counter(name for record in execution_records(fixture) for name in record["names"])
        assert actual == Counter(case["name"] for case in fixture.cases if not case.get("skipped"))


def test_failed_rediscovery_preserves_cache_and_recovers_with_new_cases():
    with tempfile.TemporaryDirectory(prefix="woby suite selection ") as temporary:
        fixture = make_fixture(Path(temporary))
        configure(fixture)
        arguments = ["--test-dir", str(fixture.build), "-C", "Debug"]
        listing(fixture, *arguments)
        cache = fixture.build / "test-discovery/fake_tests/Debug/tests.cmake"
        previous = cache.read_bytes()
        failure = fixture.root / "query-error"
        failure.touch()
        fixture.executable.write_text("rebuilt binary", encoding="utf-8")
        result = ctest(fixture, *arguments, "--show-only=json-v1")
        assert result.returncode != 0
        assert "discovery executable failed" in result.stdout + result.stderr
        assert cache.read_bytes() == previous
        failure.unlink()
        fixture.cases.append({"name": "recovered case", "filename": str(fixture.root / "sources/recovered_tests.cpp")})
        write_cases(fixture)
        assert "fake_tests::recovered_tests" in listing(fixture, *arguments)


def test_group_failure_output_identifies_the_case_and_source():
    with tempfile.TemporaryDirectory(prefix="woby suite selection ") as temporary:
        fixture = make_fixture(Path(temporary))
        fixture.cases[0]["fail"] = True
        write_cases(fixture)
        configure(fixture)
        result = ctest(fixture, "--test-dir", str(fixture.build), "-C", "Debug", "--output-on-failure",
                       "-R", "^fake_tests::ordinary_tests$")
        assert result.returncode != 0
        assert "ordinary case" in result.stdout
        assert fixture.cases[0]["filename"] in result.stdout


if __name__ == "__main__":
    for test in (test_local_and_ci_suites_partition_groups_and_preserve_case_coverage,
                 test_slow_reclassification_preserves_escaped_case_names_without_rebuild,
                 test_discovery_include_works_without_project_policy_settings,
                 test_missing_or_skipped_slow_case_fails_discovery,
                 test_discovery_cache_tracks_configurations_binary_and_parser_changes,
                 test_duplicate_source_basenames_have_separate_exact_filters,
                 test_failed_rediscovery_preserves_cache_and_recovers_with_new_cases,
                 test_group_failure_output_identifies_the_case_and_source):
        test()
        print(test.__name__ + ": passed", flush=True)

"""Build a small fixture with the real toolchain and check compiler rejection."""

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


CMAKE, SOURCE, LLVM_ROOT, GENERATOR, PLATFORM = sys.argv[1:]
SOURCE = Path(SOURCE)
sys.argv = sys.argv[:1]


class CompilerToolchainTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="woby compiler ")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.source = self.root / "source"
        self.source.mkdir()
        self.build = self.root / "build"
        (self.source / "CMakeLists.txt").write_text(f'''
cmake_minimum_required(VERSION 3.24)
project(compiler_fixture LANGUAGES C CXX)
if(APPLE)
    enable_language(OBJCXX)
endif()
include("{SOURCE.as_posix()}/cmake/WobyCompiler.cmake")
include("{SOURCE.as_posix()}/cmake/WobyWarnings.cmake")
set(CMAKE_CXX_STANDARD 20)
find_package(Threads REQUIRED)
add_executable(complete complete.cpp multiply_add.cpp c_source.c)
file(GENERATE OUTPUT "${{CMAKE_BINARY_DIR}}/executable-$<CONFIG>.txt" CONTENT "$<TARGET_FILE:complete>")
target_link_libraries(complete PRIVATE Threads::Threads)
woby_enable_project_warnings(complete)
woby_enable_enum_switch_warnings(complete)
if(WIN32)
    target_sources(complete PRIVATE version.rc)
    target_compile_options(complete PRIVATE "$<$<COMPILE_LANGUAGE:RC>:/C65001>")
endif()
add_library(incomplete OBJECT incomplete.cpp)
woby_enable_enum_switch_warnings(incomplete)
''', encoding="utf-8")
        (self.source / "complete.cpp").write_text('''
#include <string>
#include <atomic>
#include <thread>
extern "C" int c_value(void);
double multiply_add(double a, double b, double c);
enum class Mode { first, second };
int select_mode(Mode mode) {
    switch (mode) { case Mode::first: return 0; case Mode::second: return 1; }
    return 2;
}
int main() {
    std::atomic<int> ready{0};
    std::jthread worker([&] { ready.store(1); ready.notify_one(); });
    ready.wait(0);
    // Separate rounding gives zero; a fused multiply-add gives -2^-104.
    if (multiply_add(0x1.0000000000001p0, 0x1.ffffffffffffep-1, -1.0) != 0.0) return 10;
    return select_mode(Mode::first) + c_value() + static_cast<int>(std::string{}.size());
}
''', encoding="utf-8")
        (self.source / "multiply_add.cpp").write_text(
            "double multiply_add(double a, double b, double c) { return a * b + c; }\n", encoding="utf-8")
        (self.source / "c_source.c").write_text("#include <stdlib.h>\nint c_value(void) { return abs(0); }\n", encoding="utf-8")
        (self.source / "version.rc").write_text('#include <windows.h>\n1 RCDATA { "compiler fixture" }\n', encoding="utf-8")
        (self.source / "incomplete.cpp").write_text('''
enum class Mode { first, second };
int select_mode(Mode mode) {
    switch (mode) { case Mode::first: return 0; default: return 1; }
}
''', encoding="utf-8")

    def configure(self, toolchain, generator=GENERATOR):
        args = [CMAKE, "-S", str(self.source), "-B", str(self.build), "-G", generator,
                f"-DCMAKE_TOOLCHAIN_FILE={toolchain}", f"-DWOBY_LLVM_ROOT={LLVM_ROOT}"]
        if generator.startswith("Visual Studio"):
            args += ["-A", PLATFORM, "-T", "ClangCL,host=x64"]
        else:
            args += ["-DCMAKE_BUILD_TYPE=Release"]
        return subprocess.run(args, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

    def test_c_and_cpp_build_without_implicit_fma_and_missing_enum_is_an_error(self):
        configured = self.configure(SOURCE / "cmake/toolchains/llvm.cmake")
        self.assertEqual(configured.returncode, 0, configured.stdout)
        for target, success in [("complete", True), ("incomplete", False)]:
            built = subprocess.run([CMAKE, "--build", str(self.build), "--config", "Release", "--target", target],
                                   text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            if success:
                self.assertEqual(built.returncode, 0, built.stdout)
                self.assertNotIn("warning:", built.stdout.lower(), built.stdout)
                executable = Path((self.build / "executable-Release.txt").read_text())
                ran = subprocess.run([str(executable)], capture_output=True, text=True)
                self.assertEqual(ran.returncode, 0, f"Compiler changed separate multiply-add rounding: {ran.stdout}{ran.stderr}")
            else:
                self.assertNotEqual(built.returncode, 0, built.stdout)
                self.assertIn("-Wswitch-enum", built.stdout)

    def test_wrong_llvm_version_fails_during_configuration(self):
        toolchain = self.root / "toolchains/llvm.cmake"
        toolchain.parent.mkdir()
        shutil.copyfile(SOURCE / "cmake/toolchains/llvm.cmake", toolchain)
        (self.root / "llvm-toolchain.json").write_text(json.dumps({"version": "0.0.1"}), encoding="utf-8")
        configured = self.configure(toolchain)
        self.assertNotEqual(configured.returncode, 0, configured.stdout)
        self.assertIn("requires upstream Clang 0.0.1", configured.stdout)

    @unittest.skipUnless(os.name == "nt" and os.environ.get("VCToolsInstallDir"),
                         "Windows Ninja resource check requires the Visual Studio environment")
    def test_ninja_windows_resources_use_the_llvm_preprocessor(self):
        configured = self.configure(SOURCE / "cmake/toolchains/llvm.cmake", "Ninja")
        self.assertEqual(configured.returncode, 0, configured.stdout)
        built = subprocess.run([CMAKE, "--build", str(self.build), "--target", "complete"],
                               text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.assertEqual(built.returncode, 0, built.stdout)
        self.assertNotIn("warning:", built.stdout.lower(), built.stdout)


if __name__ == "__main__":
    unittest.main()

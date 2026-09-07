cmake_minimum_required(VERSION 3.24)

# Build a small real bundle with an indirect dylib dependency, then install and
# relocate it. This exercises the same layout, staging, fixups, and signing as Woby.
string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef run_id)
set(work "${TEST_ROOT}/macos-bundle-${run_id}")
set(source "${work}/source")
set(binary "${work}/build")
set(prefix "${work}/install with spaces")
set(relocated "${work}/relocated")
file(MAKE_DIRECTORY "${source}/assets/fonts")
file(WRITE "${source}/assets/fonts/test.txt" "bundled font")
file(WRITE "${source}/leaf.cpp" "int leaf() { return 42; }\n")
file(WRITE "${source}/middle.cpp" "int leaf(); int answer() { return leaf(); }\n")
file(WRITE "${source}/main.cpp" [=[
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
int answer();
int main(int argc, char** argv)
{
    if (argc < 1) return 1;
    const auto resources = std::filesystem::path(argv[0]).parent_path().parent_path() / "Resources";
    std::ifstream font(resources / "assets/fonts/test.txt");
    std::string text;
    std::getline(font, text);
    if (text != "bundled font" || answer() != 42) return 1;
    std::ifstream shader(resources / "assets/shaders/test.bin");
    std::getline(shader, text);
    if (text != "bundled shader") return 1;
    std::cout << "bundle ok\n";
    return 0;
}
]=])
set(fixture [=[
cmake_minimum_required(VERSION 3.24)
project(bundle_fixture VERSION 1.2.3 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(runtime "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
if(CMAKE_CONFIGURATION_TYPES)
    string(APPEND runtime "/$<CONFIG>")
endif()
include("@BUNDLE_MODULE@")
include("@RUNTIME_MODULE@")
add_library(leaf SHARED leaf.cpp)
add_library(middle SHARED middle.cpp)
target_link_libraries(middle PRIVATE leaf)
add_executable(viewer main.cpp)
target_link_libraries(viewer PRIVATE middle)
target_compile_options(viewer PRIVATE -Wall -Wextra -Werror)
woby_configure_macos_bundle(viewer "${runtime}" resources
    "$<TARGET_FILE_DIR:middle>" "$<TARGET_FILE_DIR:leaf>")
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/assets/shaders")
file(WRITE "${CMAKE_BINARY_DIR}/assets/shaders/test.bin" "bundled shader")
set_property(TARGET viewer PROPERTY WOBY_SHADER_OUTPUTS "${CMAKE_BINARY_DIR}/assets/shaders/test.bin")
woby_stage_assets(viewer "${resources}")
]=])
string(CONFIGURE "${fixture}" fixture @ONLY)
file(WRITE "${source}/CMakeLists.txt" "${fixture}")

function(run_checked)
    execute_process(COMMAND ${ARGV}
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(NOT result STREQUAL "0")
        message(FATAL_ERROR "Command failed (${ARGV}): ${output}${error}")
    endif()
endfunction()

run_checked("${CMAKE_COMMAND}" -S "${source}" -B "${binary}"
    -G "${TEST_GENERATOR}" "-DCMAKE_BUILD_TYPE=${TEST_CONFIG}")
run_checked("${CMAKE_COMMAND}" --build "${binary}" --config "${TEST_CONFIG}" --parallel 2)
run_checked("${CMAKE_COMMAND}" --install "${binary}" --config "${TEST_CONFIG}"
    --prefix "${prefix}" --component Runtime)
file(RENAME "${prefix}" "${relocated}")
# Make the original dependency locations unavailable to catch accidental use of
# build-tree libraries even when the executable can otherwise launch.
file(RENAME "${binary}" "${work}/unavailable-build")
set(bundle "${relocated}/viewer.app")
foreach(path IN ITEMS Contents/Info.plist Contents/MacOS/viewer
        Contents/Resources/assets/fonts/test.txt
        Contents/Resources/assets/shaders/test.bin
        Contents/Frameworks/libleaf.dylib Contents/Frameworks/libmiddle.dylib)
    if(NOT EXISTS "${bundle}/${path}")
        message(FATAL_ERROR "Bundle is missing ${path}")
    endif()
endforeach()
file(READ "${bundle}/Contents/Info.plist" plist)
if(NOT plist MATCHES "<string>1.2.3</string>")
    message(FATAL_ERROR "Bundle version was not populated")
endif()
include(BundleUtilities)
verify_app("${bundle}")
run_checked(codesign --verify --deep --strict "${bundle}")
run_checked("${bundle}/Contents/MacOS/viewer")

#include "scene_file.h"

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace {

void writeText(const std::filesystem::path& path, const char* text)
{
    std::ofstream stream(path, std::ios::trunc);
    stream << text;
}

void checkReadThrowsContaining(const std::filesystem::path& path, const std::string& expected)
{
    try {
        (void)woby::readSceneDocument(path);
        FAIL("Expected readSceneDocument to throw.");
    } catch (const std::runtime_error& exception) {
        CHECK(std::string(exception.what()).find(expected) != std::string::npos);
    }
}

} // namespace

TEST_CASE("scene path helpers normalize saved and referenced paths")
{
    CHECK(woby::sceneSavePathWithExtension("scene") == std::filesystem::path("scene.woby"));
    CHECK(woby::sceneSavePathWithExtension("scene.txt") == std::filesystem::path("scene.txt.woby"));
    CHECK(woby::sceneSavePathWithExtension("scene.WOBY") == std::filesystem::path("scene.WOBY"));

    const std::filesystem::path root = std::filesystem::temp_directory_path()
        / "woby_scene_path_helpers";
    const std::filesystem::path scenePath = root / "scenes" / "scene.woby";
    const std::filesystem::path expected = std::filesystem::absolute(root / "models" / "part.obj")
        .lexically_normal();

    CHECK(woby::sceneAbsolutePath(scenePath, "../models/part.obj") == expected);
}

TEST_CASE("scene document reader reports malformed files")
{
    const std::filesystem::path root = std::filesystem::temp_directory_path()
        / "woby_scene_file_errors";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    const std::filesystem::path unsupportedVersion = root / "unsupported.woby";
    writeText(
        unsupportedVersion,
        "# comment\n"
        "version = 3\n");
    checkReadThrowsContaining(unsupportedVersion, ":2: Unsupported scene version.");

    const std::filesystem::path groupBeforeFile = root / "group_before_file.woby";
    writeText(
        groupBeforeFile,
        "version = 2\n"
        "[[files.groups]]\n");
    checkReadThrowsContaining(groupBeforeFile, ":2: Group table appeared before any file table.");

    const std::filesystem::path badArray = root / "bad_array.woby";
    writeText(
        badArray,
        "version = 2\n"
        "[[files]]\n"
        "path = \"mesh.obj\"\n"
        "translation = [1, 2]\n");
    checkReadThrowsContaining(badArray, ":4: Expected TOML array with 3 floats.");

    const std::filesystem::path missingPath = root / "missing_path.woby";
    writeText(
        missingPath,
        "version = 2\n"
        "[[files]]\n"
        "visible = true\n");
    checkReadThrowsContaining(missingPath, "Scene contains a file entry without a path.");

    std::filesystem::remove_all(root);
}

TEST_CASE("scene document reader reports invalid scalar and string values")
{
    const std::filesystem::path root = std::filesystem::temp_directory_path()
        / "woby_scene_file_scalar_errors";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    const std::filesystem::path invalidBool = root / "invalid_bool.woby";
    writeText(
        invalidBool,
        "version = 2\n"
        "show_grid = maybe\n");
    checkReadThrowsContaining(invalidBool, ":2: Expected TOML boolean value.");

    const std::filesystem::path invalidUpAxis = root / "invalid_up_axis.woby";
    writeText(
        invalidUpAxis,
        "version = 2\n"
        "up_axis = \"x\"\n");
    checkReadThrowsContaining(invalidUpAxis, ":2: Expected up axis \"z\" or \"y\".");

    const std::filesystem::path invalidEscape = root / "invalid_escape.woby";
    writeText(
        invalidEscape,
        "version = 2\n"
        "[[files]]\n"
        "path = \"bad\\q.obj\"\n");
    checkReadThrowsContaining(invalidEscape, ":3: Unsupported TOML string escape.");

    const std::filesystem::path invalidKeyValue = root / "invalid_key_value.woby";
    writeText(
        invalidKeyValue,
        "version = 2\n"
        "show_grid true\n");
    checkReadThrowsContaining(invalidKeyValue, ":2: Expected TOML key/value pair.");

    std::filesystem::remove_all(root);
}

TEST_CASE("scene document writer and reader preserve escaped strings")
{
    const std::filesystem::path root = std::filesystem::temp_directory_path()
        / "woby_scene_file_escaped_strings";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "models");
    const std::filesystem::path scenePath = root / "scene.woby";

    woby::SceneDocument document;
    woby::SceneFileRecord file;
    file.path = root / "models" / "part.obj";
    woby::SceneGroupRecord group;
    group.name = "part \"quoted\" \\ slash\nline";
    file.groups.push_back(group);
    document.files.push_back(file);

    woby::writeSceneDocument(scenePath, document);
    const woby::SceneDocument restored = woby::readSceneDocument(scenePath);

    REQUIRE(restored.files.size() == 1u);
    CHECK(restored.files[0].path == std::filesystem::path("models/part.obj"));
    REQUIRE(restored.files[0].groups.size() == 1u);
    CHECK(restored.files[0].groups[0].name == group.name);

    std::filesystem::remove_all(root);
}

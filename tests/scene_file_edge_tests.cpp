#include "scene_file.h"
#include "automation_registry.h"

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace {

struct CameraDirectory {
    std::filesystem::path root = std::filesystem::absolute(std::filesystem::temp_directory_path())
        / ("woby-camera-" + woby::automationRandomHex(8));
    CameraDirectory() { std::filesystem::create_directory(root); }
    ~CameraDirectory() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
};

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

TEST_CASE("camera records clamp finite ranges and use defaults for missing fields")
{
    const CameraDirectory directory;
    const auto path = directory.root / "ranges.woby";
    writeText(path, "version = 6\n[camera]\n");
    REQUIRE(woby::readSceneDocument(path).camera);
    woby::SceneCamera legacy;
    legacy.automaticNearPlane = false;
    CHECK(*woby::readSceneDocument(path).camera == legacy);
    writeText(path, "version = 6\n[camera]\ntarget = [3e38, -3e38, 2]\n"
        "pitch_radians = 30\ndistance = -1\nvertical_fov_degrees = 300\nnear_plane = -2\n");
    const auto camera = *woby::readSceneDocument(path).camera;
    CHECK(camera.target == std::array<float, 3>{1e15f, -1e15f, 2});
    CHECK(camera.pitchRadians == doctest::Approx(1.57079633f));
    CHECK(camera.distance == doctest::Approx(0.001f));
    CHECK(camera.verticalFovDegrees == 179);
    CHECK(camera.nearPlane == doctest::Approx(0.0001f));
    writeText(path, "version = 6\n[camera]\npitch_radians = -30\n"
        "distance = 3e38\nvertical_fov_degrees = -10\nnear_plane = 3e38\n");
    const auto other = *woby::readSceneDocument(path).camera;
    CHECK(other.pitchRadians == doctest::Approx(-1.57079633f));
    CHECK(other.distance == 1e15f);
    CHECK(other.verticalFovDegrees == 1);
    CHECK(other.nearPlane == 5e14f);
    writeText(path, "version = 6\n[camera]\nnear_plane = 100\ndistance = 1000\n");
    CHECK(woby::readSceneDocument(path).camera->nearPlane == 100);
    std::filesystem::remove(path);
}

TEST_CASE("camera records reject nonfinite and malformed values with line context")
{
    const CameraDirectory directory;
    const auto path = directory.root / "errors.woby";
    for (const auto* key : {"yaw_radians", "pitch_radians", "roll_radians", "distance",
        "vertical_fov_degrees", "near_plane"}) {
        for (const auto* value : {"nan", "inf", "-inf"}) {
            const auto text = std::string("version = 6\n[camera]\n") + key + " = " + value + "\n";
            writeText(path, text.c_str());
            checkReadThrowsContaining(path, ":3: Camera values must be finite.");
        }
    }
    for (const auto* value : {"[nan, 1, 2]", "[1, inf, 2]", "[1, 2, -inf]"}) {
        const auto text = std::string("version = 6\n[camera]\ntarget = ") + value + "\n";
        writeText(path, text.c_str());
        checkReadThrowsContaining(path, ":3: Camera values must be finite.");
    }
    writeText(path, "version = 6\n[camera]\ntarget = [1, 2]\n");
    checkReadThrowsContaining(path, ":3: Expected TOML array with 3 floats.");
    writeText(path, "version = 6\n[camera]\ndistance = 1.0bad\n");
    checkReadThrowsContaining(path, ":3: Expected TOML float value.");
    writeText(path, "version = 6\n[camera]\n[camera]\n");
    checkReadThrowsContaining(path, ":3: Duplicate camera table.");
    writeText(path, "version = 24\n[camera]\nautomatic_near_plane = 1\n");
    checkReadThrowsContaining(path, ":3: Expected TOML boolean value.");
    std::filesystem::remove(path);
}

TEST_CASE("camera clipping modes and retained manual distances round trip for scenes and named views")
{
    const CameraDirectory directory;
    const auto path = directory.root / "clipping.woby";
    for (const bool automatic : {false, true}) {
        woby::SceneDocument document;
        document.camera.emplace();
        document.camera->nearPlane = .025f;
        document.camera->automaticNearPlane = automatic;
        document.views.emplace_back();
        document.views.back().name = "Close view";
        document.views.back().scene.camera.nearPlane = .075f;
        document.views.back().scene.camera.automaticNearPlane = !automatic;
        woby::writeSceneDocument(path, document);
        const auto restored = woby::readSceneDocument(path);
        CHECK(restored.camera == document.camera);
        REQUIRE(restored.views.size() == 1);
        CHECK(restored.views[0].scene.camera == document.views[0].scene.camera);
    }
    writeText(path, "version = 19\n[camera]\nnear_plane = 0.1\n[[views]]\nname = \"Legacy\"\n[views.camera]\nnear_plane = 0.05\n");
    const auto legacy = woby::readSceneDocument(path);
    REQUIRE(legacy.camera);
    CHECK_FALSE(legacy.camera->automaticNearPlane);
    CHECK(legacy.camera->nearPlane == doctest::Approx(.1f));
    REQUIRE(legacy.views.size() == 1);
    CHECK_FALSE(legacy.views[0].scene.camera.automaticNearPlane);
    CHECK(legacy.views[0].scene.camera.nearPlane == doctest::Approx(.05f));
}

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
        "version = 999\n");
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

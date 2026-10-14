#include "file_discovery.h"
#include "utf8_path.h"

#include <doctest/doctest.h>

#include <chrono>
#include <random>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <vector>

namespace {

struct DiscoveryTestDirectory {
    std::filesystem::path path = std::filesystem::absolute(std::filesystem::temp_directory_path())
        / ("woby_discovery_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
            + "_" + std::to_string(std::random_device{}()));
    DiscoveryTestDirectory() { std::filesystem::create_directory(path); }
    ~DiscoveryTestDirectory() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
};

void writeEmptyFile(const std::filesystem::path& path)
{
    std::ofstream stream(path);
    stream << '\n';
}

void collectModelPathsAndDiscard(const std::filesystem::path& path)
{
    (void)woby::collectModelPathsRecursive(path);
}

} // namespace

TEST_CASE("path extension checks are case insensitive")
{
    CHECK(woby::isObjPath("mesh.OBJ"));
    CHECK(woby::isObjPath("mesh.obj"));
    CHECK(woby::isModelPath("mesh.OBJ"));
    CHECK(woby::isVtkPath("mesh.VTK"));
    CHECK(woby::isModelPath("mesh.vtk"));
    CHECK_FALSE(woby::isModelPath("mesh.stl"));
    CHECK_FALSE(woby::isModelPath("mesh.STL"));
    CHECK_FALSE(woby::isObjPath("mesh.woby"));
    CHECK_FALSE(woby::isModelPath("mesh.woby"));
    CHECK(woby::isWobyPath("scene.WOBY"));
    CHECK_FALSE(woby::isWobyPath("scene.obj"));
}

TEST_CASE("recursive model collection includes nested folders in sorted order")
{
    const DiscoveryTestDirectory fixture;
    const auto& root = fixture.path;
    std::filesystem::create_directories(root / "b");
    std::filesystem::create_directories(root / "a" / "nested");
    writeEmptyFile(root / "root.obj");
    writeEmptyFile(root / "b" / "two.OBJ");
    writeEmptyFile(root / "a" / "nested" / "one.obj");
    writeEmptyFile(root / "a" / "nested" / "part.STL");
    writeEmptyFile(root / "a" / "nested" / "ignored.txt");

    const std::vector<std::filesystem::path> paths = woby::collectModelPathsRecursive(root);

    REQUIRE(paths.size() == 3u);
    CHECK(paths[0].filename() == "one.obj");
    CHECK(paths[1].filename() == "two.OBJ");
    CHECK(paths[2].filename() == "root.obj");
}

TEST_CASE("model collection rejects non-folders and excludes unsupported extensions")
{
    const DiscoveryTestDirectory fixture;
    const auto& root = fixture.path;
    const std::filesystem::path filePath = root / "not_a_folder.txt";
    writeEmptyFile(filePath);
    writeEmptyFile(root / "one.obj");
    writeEmptyFile(root / "two.stl");

    CHECK_THROWS_WITH_AS(
        collectModelPathsAndDiscard(filePath),
        ("Folder path is not a folder: " + filePath.string()).c_str(),
        std::runtime_error);

    const std::vector<std::filesystem::path> objPaths = woby::collectModelPathsRecursive(root);

    REQUIRE(objPaths.size() == 1u);
    CHECK(objPaths[0].filename() == "one.obj");
}

TEST_CASE("folder discovery errors preserve UTF-8 paths for automation responses")
{
    const DiscoveryTestDirectory fixture;
    const auto path = fixture.path / woby::pathFromUtf8("caf\xc3\xa9 \xe6\xa8\xa1\xe5\x9e\x8b");
    try {
        (void)woby::collectModelPathsRecursive(path);
        FAIL("Expected a missing folder error.");
    } catch (const std::runtime_error& error) {
        CHECK(std::string(error.what()) == "Folder path is not a folder: " + woby::pathToUtf8(path));
        const nlohmann::json response = {{"error", error.what()}};
        CHECK_NOTHROW(response.dump());
    }
}

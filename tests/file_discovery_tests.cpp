#include "file_discovery.h"

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <vector>

namespace {

void writeEmptyFile(const std::filesystem::path& path)
{
    std::ofstream stream(path);
    stream << '\n';
}

} // namespace

TEST_CASE("path extension checks are case insensitive")
{
    CHECK(woby::isObjPath("mesh.OBJ"));
    CHECK(woby::isObjPath("mesh.obj"));
    CHECK_FALSE(woby::isObjPath("mesh.woby"));
    CHECK(woby::isWobyPath("scene.WOBY"));
    CHECK_FALSE(woby::isWobyPath("scene.obj"));
}

TEST_CASE("recursive OBJ collection includes nested folders in sorted order")
{
    const std::filesystem::path root = std::filesystem::temp_directory_path()
        / "woby_recursive_obj_collection_test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "b");
    std::filesystem::create_directories(root / "a" / "nested");
    writeEmptyFile(root / "root.obj");
    writeEmptyFile(root / "b" / "two.OBJ");
    writeEmptyFile(root / "a" / "nested" / "one.obj");
    writeEmptyFile(root / "a" / "nested" / "ignored.txt");

    const std::vector<std::filesystem::path> paths = woby::collectObjPathsRecursive(root);

    REQUIRE(paths.size() == 3u);
    CHECK(paths[0].filename() == "one.obj");
    CHECK(paths[1].filename() == "two.OBJ");
    CHECK(paths[2].filename() == "root.obj");

    std::filesystem::remove_all(root);
}

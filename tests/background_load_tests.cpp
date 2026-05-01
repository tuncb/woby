#include "background_load.h"

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <vector>

namespace {

void writeTriangleObj(const std::filesystem::path& path)
{
    std::ofstream stream(path, std::ios::trunc);
    stream << "o triangle\n";
    stream << "v 0 0 0\n";
    stream << "v 1 0 0\n";
    stream << "v 0 1 0\n";
    stream << "f 1 2 3\n";
}

void writeTriangleStl(const std::filesystem::path& path)
{
    std::ofstream stream(path, std::ios::trunc);
    stream << "solid triangle_stl\n";
    stream << "  facet normal 0 0 1\n";
    stream << "    outer loop\n";
    stream << "      vertex 0 0 0\n";
    stream << "      vertex 1 0 0\n";
    stream << "      vertex 0 1 0\n";
    stream << "    endloop\n";
    stream << "  endfacet\n";
    stream << "endsolid triangle_stl\n";
}

} // namespace

TEST_CASE("background model batch loader creates UI file states")
{
    const std::filesystem::path root = std::filesystem::temp_directory_path()
        / "woby_background_model_batch_loader";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const std::filesystem::path objPath = root / "triangle.obj";
    const std::filesystem::path stlPath = root / "triangle.stl";
    const std::filesystem::path skippedPath = root / "ignored.txt";
    writeTriangleObj(objPath);
    writeTriangleStl(stlPath);
    {
        std::ofstream stream(skippedPath, std::ios::trunc);
        stream << "ignored\n";
    }

    std::vector<woby::BackgroundLoadProgress> progressUpdates;
    const woby::ModelBatchCpuLoadResult result = woby::loadModelBatchCpu(
        {objPath, stlPath, skippedPath},
        5u,
        [&progressUpdates](const woby::BackgroundLoadProgress& progress) {
            progressUpdates.push_back(progress);
        },
        {});

    CHECK_FALSE(result.canceled);
    CHECK(result.requestedCount == 3u);
    CHECK(result.addedCount == 2u);
    CHECK(result.skippedCount == 1u);
    CHECK(result.failedCount == 0u);
    REQUIRE(result.files.size() == 2u);
    CHECK(result.files[0].path == objPath);
    CHECK(result.files[0].mesh.vertices.size() == 3u);
    REQUIRE(result.files[0].groupSettings.size() == 1u);
    CHECK(result.files[0].groupSettings[0].color == woby::defaultGroupColor(5u));
    CHECK(result.files[1].path == stlPath);
    REQUIRE(result.files[1].groupSettings.size() == 1u);
    CHECK(result.files[1].groupSettings[0].color == woby::defaultGroupColor(6u));
    REQUIRE_FALSE(progressUpdates.empty());
    CHECK(progressUpdates[0].currentPath == objPath);

    std::filesystem::remove_all(root);
}

TEST_CASE("background scene loader applies persisted file settings")
{
    const std::filesystem::path root = std::filesystem::temp_directory_path()
        / "woby_background_scene_loader";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const std::filesystem::path objPath = root / "triangle.stl";
    const std::filesystem::path scenePath = root / "scene.woby";
    writeTriangleStl(objPath);

    woby::SceneDocument document;
    document.showGrid = false;
    document.upAxis = woby::SceneUpAxis::y;
    woby::SceneFileRecord fileRecord;
    fileRecord.path = objPath;
    fileRecord.settings.visible = false;
    document.files.push_back(fileRecord);
    woby::writeSceneDocument(scenePath, document);

    const woby::SceneCpuLoadResult result = woby::loadSceneCpu(scenePath, {}, {});

    CHECK_FALSE(result.canceled);
    CHECK_FALSE(result.document.showGrid);
    CHECK(result.document.upAxis == woby::SceneUpAxis::y);
    REQUIRE(result.files.size() == 1u);
    CHECK(result.files[0].path == objPath);
    CHECK_FALSE(result.files[0].fileSettings.visible);
    REQUIRE(result.files[0].groupSettings.size() == 1u);
    CHECK_FALSE(result.files[0].groupSettings[0].visible);

    std::filesystem::remove_all(root);
}

TEST_CASE("background model batch loader cancels between files")
{
    const std::filesystem::path root = std::filesystem::temp_directory_path()
        / "woby_background_model_batch_cancel";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const std::filesystem::path objPath = root / "triangle.obj";
    writeTriangleObj(objPath);

    const woby::ModelBatchCpuLoadResult result = woby::loadModelBatchCpu(
        {objPath},
        0u,
        {},
        []() {
            return true;
        });

    CHECK(result.canceled);
    CHECK(result.files.empty());
    CHECK(result.status.find("canceled") != std::string::npos);

    std::filesystem::remove_all(root);
}

TEST_CASE("background model batch loader reports skipped and failed files")
{
    const std::filesystem::path root = std::filesystem::temp_directory_path()
        / "woby_background_model_batch_failures";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const std::filesystem::path validPath = root / "triangle.stl";
    const std::filesystem::path invalidPath = root / "empty.obj";
    const std::filesystem::path skippedPath = root / "ignored.txt";
    writeTriangleStl(validPath);
    {
        std::ofstream stream(invalidPath, std::ios::trunc);
        stream << "o empty\n";
        stream << "v 0 0 0\n";
        stream << "v 1 0 0\n";
        stream << "v 0 1 0\n";
    }
    {
        std::ofstream stream(skippedPath, std::ios::trunc);
        stream << "ignored\n";
    }

    const woby::ModelBatchCpuLoadResult result = woby::loadModelBatchCpu(
        {validPath, invalidPath, skippedPath},
        0u,
        {},
        {});

    CHECK_FALSE(result.canceled);
    CHECK(result.requestedCount == 3u);
    CHECK(result.addedCount == 1u);
    CHECK(result.failedCount == 1u);
    CHECK(result.skippedCount == 1u);
    CHECK(result.files.size() == 1u);
    CHECK(result.lastError.find("OBJ did not contain renderable triangles") != std::string::npos);
    CHECK(result.status.find("Added 1 model file") != std::string::npos);
    CHECK(result.status.find("skipped 1 non-model") != std::string::npos);
    CHECK(result.status.find("failed 1") != std::string::npos);

    std::filesystem::remove_all(root);
}

TEST_CASE("background model batch loader cancels after completed files")
{
    const std::filesystem::path root = std::filesystem::temp_directory_path()
        / "woby_background_model_batch_partial_cancel";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const std::filesystem::path firstPath = root / "one.stl";
    const std::filesystem::path secondPath = root / "two.stl";
    writeTriangleStl(firstPath);
    writeTriangleStl(secondPath);

    size_t cancelChecks = 0u;
    const woby::ModelBatchCpuLoadResult result = woby::loadModelBatchCpu(
        {firstPath, secondPath},
        0u,
        {},
        [&cancelChecks]() {
            return cancelChecks++ > 0u;
        });

    CHECK(result.canceled);
    CHECK(result.requestedCount == 2u);
    CHECK(result.addedCount == 1u);
    CHECK(result.files.size() == 1u);
    CHECK(result.status.find("canceled") != std::string::npos);

    std::filesystem::remove_all(root);
}

TEST_CASE("background scene loader cancels after completed files")
{
    const std::filesystem::path root = std::filesystem::temp_directory_path()
        / "woby_background_scene_partial_cancel";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const std::filesystem::path firstPath = root / "one.stl";
    const std::filesystem::path secondPath = root / "two.stl";
    const std::filesystem::path scenePath = root / "scene.woby";
    writeTriangleStl(firstPath);
    writeTriangleStl(secondPath);

    woby::SceneDocument document;
    woby::SceneFileRecord firstRecord;
    firstRecord.path = firstPath;
    document.files.push_back(firstRecord);
    woby::SceneFileRecord secondRecord;
    secondRecord.path = secondPath;
    document.files.push_back(secondRecord);
    woby::writeSceneDocument(scenePath, document);

    size_t cancelChecks = 0u;
    const woby::SceneCpuLoadResult result = woby::loadSceneCpu(
        scenePath,
        {},
        [&cancelChecks]() {
            return cancelChecks++ > 0u;
        });

    CHECK(result.canceled);
    REQUIRE(result.files.size() == 1u);
    CHECK(result.files[0].path == firstPath);

    std::filesystem::remove_all(root);
}

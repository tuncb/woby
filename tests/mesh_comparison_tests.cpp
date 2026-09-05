#include "mesh_comparison.h"
#include "comparison_scene.h"
#include "ui_operations.h"
#include "obj_mesh.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <random>

namespace
{
woby::Mesh mesh(std::initializer_list<std::array<float, 3>> positions, std::vector<uint32_t> indices)
{
    woby::Mesh result;
    for (const auto &p : positions)
    {
        woby::Vertex v;
        v.position = p;
        result.vertices.push_back(v);
    }
    result.indices = std::move(indices);
    result.nodes.push_back({"surface", 0, static_cast<uint32_t>(result.indices.size())});
    result.bounds = woby::calculateBounds(result.vertices);
    return result;
}
woby::Mesh square(float z = 0)
{
    return mesh({{0, 0, z}, {1, 0, z}, {1, 1, z}, {0, 1, z}}, {0, 1, 2, 0, 2, 3});
}
woby::Mesh grid(bool hole)
{
    woby::Mesh result;
    for (int j = 0; j < 4; ++j)
        for (int i = 0; i < 4; ++i)
        {
            woby::Vertex vertex;
            vertex.position = {static_cast<float>(i), static_cast<float>(j), 0};
            result.vertices.push_back(vertex);
        }
    for (uint32_t j = 0; j < 3; ++j)
        for (uint32_t i = 0; i < 3; ++i)
        {
            if (hole && i == 1 && j == 1)
                continue;
            const uint32_t a = j * 4 + i;
            result.indices.insert(result.indices.end(), {a, a + 1, a + 5, a, a + 5, a + 4});
        }
    result.bounds = woby::calculateBounds(result.vertices);
    result.nodes.push_back({"grid", 0, static_cast<uint32_t>(result.indices.size())});
    return result;
}
woby::UiState stateWithFiles(size_t count)
{
    woby::UiState state;
    for (size_t i = 0; i < count; ++i)
    {
        state.files.push_back(woby::createUiFileState(std::to_string(i) + ".obj", square(), 0));
    }
    woby::appendDefaultSceneNodesForFiles(state, 0);
    return state;
}
} // namespace

TEST_CASE("point triangle distances cover face edge vertex and degenerate regions")
{
    const std::array<float, 3> a = {0, 0, 0}, b = {2, 0, 0}, c = {0, 2, 0};
    CHECK(woby::pointTriangleDistance({.5f, .5f, 3}, a, b, c) == doctest::Approx(3));
    CHECK(woby::pointTriangleDistance({1, -1, 0}, a, b, c) == doctest::Approx(1));
    CHECK(woby::pointTriangleDistance({2, 2, 0}, a, b, c) == doctest::Approx(std::sqrt(2.0)));
    CHECK(woby::pointTriangleDistance({-1, -1, 0}, a, b, c) == doctest::Approx(std::sqrt(2.0)));
    CHECK(woby::pointTriangleDistance({1, 1, 0}, a, b, b) == doctest::Approx(1));
    CHECK(woby::pointTriangleDistance({0, 0, 2}, a, a, a) == doctest::Approx(2));
    CHECK_THROWS((void)woby::pointTriangleDistance({NAN, 0, 0}, a, b, c));
}

TEST_CASE("comparison handles identical translated and retessellated surfaces")
{
    auto a = square();
    const auto same = woby::compareMeshes(a, a);
    CHECK(same.original.maximum < 1e-12);
    CHECK(same.repaired.maximum < 1e-12);
    CHECK(same.original.distances.size() == 8);
    auto b = square(.25f);
    const auto translated = woby::compareMeshes(a, b);
    CHECK(translated.original.maximum == doctest::Approx(.25));
    CHECK(translated.repaired.mean == doctest::Approx(.25));
    CHECK(translated.repaired.percentile95 == doctest::Approx(.25));
    CHECK(woby::surfacePercentAboveTolerance(translated.repaired, .1) == doctest::Approx(100));
    CHECK(woby::surfacePercentAboveTolerance(translated.repaired, .25) == doctest::Approx(0));
    auto retriangulated =
        mesh({{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {.5f, .5f, 0}}, {0, 1, 4, 1, 2, 4, 2, 3, 4, 3, 0, 4});
    const auto unchanged = woby::compareMeshes(a, retriangulated);
    CHECK(unchanged.original.maximum < 1e-12);
    CHECK(unchanged.repaired.maximum < 1e-12);
    CHECK(a.indices == std::vector<uint32_t>{0, 1, 2, 0, 2, 3});
}

TEST_CASE("interior sampling detects a filled hole and reverse comparison detects removal")
{
    const auto result = woby::compareMeshes(grid(true), grid(false));
    CHECK(result.original.maximum < 1e-12);
    CHECK(result.repaired.maximum > .15);
    CHECK(result.original.diagnostics.boundaryEdges.size() == 16);
    CHECK(result.repaired.diagnostics.boundaryEdges.size() == 12);
    CHECK(woby::surfacePercentAboveTolerance(result.repaired, .01) == doctest::Approx(100.0 / 9.0));
    const auto reversed = woby::compareMeshes(grid(false), grid(true));
    CHECK(reversed.original.maximum == doctest::Approx(result.repaired.maximum));
    CHECK(reversed.repaired.maximum < 1e-12);
}

TEST_CASE("BVH distances match exhaustive triangle queries")
{
    auto a = grid(false), b = grid(false);
    std::mt19937 random(912);
    std::uniform_real_distribution<float> elevation(-.6f, .6f);
    for (auto &vertex : a.vertices)
        vertex.position[2] = elevation(random);
    for (auto &vertex : b.vertices)
        vertex.position[2] = elevation(random);
    const auto result = woby::compareMeshes(a, b);
    for (size_t i = 0; i < result.original.distances.size(); ++i)
    {
        std::array<float, 3> p{};
        for (size_t k = 0; k < 3; ++k)
            for (size_t v = 0; v < 3; ++v)
                p[k] += result.original.sampled.vertices[i * 3 + v].position[k] / 3;
        double expected = std::numeric_limits<double>::infinity();
        for (size_t t = 0; t < b.indices.size(); t += 3)
            expected = std::min(expected, woby::pointTriangleDistance(p, b.vertices[b.indices[t]].position,
                                                                      b.vertices[b.indices[t + 1]].position,
                                                                      b.vertices[b.indices[t + 2]].position));
        CHECK(result.original.distances[i] == doctest::Approx(expected).epsilon(1e-5).scale(1));
    }
}

TEST_CASE("diagnostics weld identical seam positions and distinguish edge defects")
{
    const auto seams = mesh({{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 0, 0}, {1, 1, 0}, {0, 1, 0}}, {0, 1, 2, 3, 4, 5});
    CHECK(woby::inspectMesh(seams).boundaryEdges.size() == 4);
    CHECK(woby::inspectMesh(seams).inconsistentWindingEdges.empty());
    auto flipped = seams;
    std::swap(flipped.indices[4], flipped.indices[5]);
    CHECK(woby::inspectMesh(flipped).inconsistentWindingEdges.size() == 1);
    auto duplicate = square();
    duplicate.indices.insert(duplicate.indices.end(), {0, 1, 2});
    const auto defects = woby::inspectMesh(duplicate);
    CHECK(defects.duplicateTriangles == 1);
    CHECK(defects.nonManifoldEdges.size() == 1);
    duplicate.indices.insert(duplicate.indices.end(), {0, 0, 1});
    CHECK(woby::inspectMesh(duplicate).degenerateTriangles == 1);
}

TEST_CASE("comparison validates empty invalid and excessive input")
{
    auto a = square();
    CHECK_THROWS((void)woby::compareMeshes(a, {}));
    auto invalid = a;
    invalid.indices[0] = 99;
    CHECK_THROWS((void)woby::compareMeshes(a, invalid));
    invalid = a;
    invalid.vertices[0].position[0] = std::numeric_limits<float>::infinity();
    CHECK_THROWS((void)woby::compareMeshes(a, invalid));
    invalid = a;
    invalid.indices.push_back(0);
    CHECK_THROWS((void)woby::compareMeshes(a, invalid));
    invalid = a;
    invalid.indices.resize((woby::comparisonTriangleLimit + 1) * 3, 0);
    CHECK_THROWS((void)woby::compareMeshes(a, invalid));
    CHECK_THROWS((void)woby::surfacePercentAboveTolerance({}, -1));
}

TEST_CASE("comparison area statistics do not count tessellation density")
{
    woby::SurfaceComparison surface;
    surface.distances = {0, 1, 1, 1};
    surface.sampleAreas = {97, 1, 1, 1};
    CHECK(woby::surfacePercentAboveTolerance(surface, .5) == doctest::Approx(3));
}

TEST_CASE("comparison accepts cancellation without modifying the meshes")
{
    const auto a = square();
    std::stop_source stop;
    stop.request_stop();
    CHECK_THROWS_WITH((void)woby::compareMeshes(a, a, stop.get_token()), "Comparison canceled.");
    CHECK(a.indices.size() == 6);
}

TEST_CASE("comparison snapshot follows folder file and group transforms without changing inputs")
{
    auto state = stateWithFiles(2);
    woby::setFileTranslation(state.files[0].fileSettings, {2, 0, 0});
    woby::setGroupTranslation(state.files[0].groupSettings[0], {1, 0, 0});
    woby::UiSceneNode folder;
    folder.name = "parent";
    folder.settings.translation = {3, 0, 0};
    folder.children.push_back(state.sceneNodes[0]);
    state.sceneNodes[0] = folder;
    state.files[0].fileSettings.visible = false;
    const auto transformed = woby::comparisonWorldMesh(state, 0);
    CHECK(transformed.vertices[0].position[0] == doctest::Approx(6));
    CHECK(state.files[0].mesh.vertices[0].position[0] == 0);
    state.comparison.originalFile = 0;
    state.comparison.repairedFile = 1;
    const auto signature = woby::comparisonGeometrySignature(state);
    state.files[0].groupSettings[0].color = {1, 1, 1, 1};
    state.files[0].fileSettings.visible = true;
    CHECK(signature == woby::comparisonGeometrySignature(state));
    woby::setGroupTranslation(state.files[0].groupSettings[0], {4, 0, 0});
    CHECK(signature != woby::comparisonGeometrySignature(state));
}

TEST_CASE("comparison settings clamp at operation boundaries and follow file removal")
{
    auto state = stateWithFiles(3);
    woby::ComparisonSettings settings;
    settings.enabled = true;
    settings.originalFile = 1;
    settings.repairedFile = 2;
    settings.tolerance = NAN;
    settings.colorRange = -4;
    woby::setComparisonSettings(state, settings);
    CHECK(state.comparison.tolerance == doctest::Approx(.05));
    CHECK(state.comparison.colorRange >= state.comparison.tolerance);
    CHECK(woby::removeFileFromState(state, 0));
    CHECK(state.comparison.enabled);
    CHECK(state.comparison.originalFile == 0);
    CHECK(state.comparison.repairedFile == 1);
    CHECK(woby::removeFileFromState(state, 0));
    CHECK_FALSE(state.comparison.enabled);
    CHECK(state.comparison.originalFile == -1);
    settings.originalFile = 0;
    settings.repairedFile = 0;
    woby::setComparisonSettings(state, settings);
    CHECK_FALSE(state.comparison.enabled);
}

TEST_CASE("tree selection replaces toggles and preserves a pair on context click")
{
    auto state = stateWithFiles(3);
    const auto clean = woby::createSceneDocument(state);
    const auto first = state.files[0].objectId;
    const auto second = state.files[1].objectId;
    const auto third = state.files[2].objectId;
    woby::selectSceneObject(state, first);
    CHECK(woby::sceneObjectSelected(state, first));
    woby::selectSceneObject(state, second, true);
    CHECK(state.selectedSceneObjects == std::vector<woby::SceneObjectId>{first, second});
    woby::selectSceneObject(state, first, false, true);
    CHECK(state.selectedSceneObjects == std::vector<woby::SceneObjectId>{first, second});
    woby::selectSceneObject(state, second, true, true);
    CHECK(state.selectedSceneObjects == std::vector<woby::SceneObjectId>{first, second});
    woby::selectSceneObject(state, first, true);
    CHECK(state.selectedSceneObjects == std::vector<woby::SceneObjectId>{second});
    woby::selectSceneObject(state, first);
    CHECK(state.selectedSceneObjects == std::vector<woby::SceneObjectId>{first});
    woby::selectSceneObject(state, second, true);
    woby::selectSceneObject(state, third, true, true);
    CHECK(state.selectedSceneObjects == std::vector<woby::SceneObjectId>{third});
    woby::selectSceneObject(state, woby::invalidSceneObjectId);
    woby::selectSceneObject(state, state.nextObjectId);
    CHECK(state.selectedSceneObjects == std::vector<woby::SceneObjectId>{third});
    woby::clearSceneSelection(state);
    CHECK(state.selectedSceneObjects.empty());
    woby::clearSceneSelection(state);
    CHECK(state.selectedSceneObjects.empty());
    woby::updateSceneDirty(state, clean);
    CHECK_FALSE(state.isDirty);
    CHECK(woby::createSceneDocument(state) == clean);
}

TEST_CASE("tree comparison uses click order and preserves comparison display settings")
{
    auto state = stateWithFiles(2);
    const auto clean = woby::createSceneDocument(state);
    auto settings = state.comparison;
    settings.mode = woby::ComparisonMode::overlay;
    settings.tolerance = .25f;
    woby::setComparisonSettings(state, settings);
    woby::selectSceneObject(state, state.files[1].objectId);
    woby::selectSceneObject(state, state.files[0].objectId, true);
    REQUIRE(woby::canCompareSceneSelection(state));
    REQUIRE(woby::compareSceneSelection(state));
    CHECK(state.comparison.enabled);
    CHECK(state.comparison.originalFile == 1);
    CHECK(state.comparison.repairedFile == 0);
    CHECK(state.comparison.mode == woby::ComparisonMode::overlay);
    CHECK(state.comparison.tolerance == doctest::Approx(.25));
    woby::updateSceneDirty(state, clean);
    CHECK(state.isDirty);
    const auto document = woby::createSceneDocument(state);
    CHECK(document.comparison == state.comparison);
    const auto replacement = woby::prepareSceneReplacement(state, state.files, document);
    CHECK(replacement.comparison == state.comparison);
    CHECK(replacement.selectedSceneObjects.empty());
}

TEST_CASE("tree comparison is unavailable for groups folders and invalid selection counts")
{
    auto state = stateWithFiles(3);
    const auto original = state.comparison;
    const auto unavailable = [&] {
        CHECK_FALSE(woby::canCompareSceneSelection(state));
        CHECK_FALSE(woby::compareSceneSelection(state));
        CHECK(state.comparison == original);
    };
    unavailable();
    woby::selectSceneObject(state, state.files[0].objectId);
    unavailable();
    woby::selectSceneObject(state, state.files[1].groupSettings[0].objectId, true);
    unavailable();
    woby::selectSceneObject(state, state.files[0].groupSettings[0].objectId);
    woby::selectSceneObject(state, state.files[1].groupSettings[0].objectId, true);
    unavailable();
    woby::UiSceneNode folder;
    folder.name = "folder";
    state.sceneNodes.push_back(folder);
    woby::assignSceneObjectIds(state);
    woby::selectSceneObject(state, state.sceneNodes.back().objectId);
    woby::selectSceneObject(state, state.files[1].objectId, true);
    unavailable();
    woby::selectSceneObject(state, state.files[0].objectId);
    woby::selectSceneObject(state, state.files[1].objectId, true);
    woby::selectSceneObject(state, state.files[2].objectId, true);
    unavailable();
    state.selectedSceneObjects = {state.files[0].objectId, state.files[0].objectId};
    unavailable();
    state.selectedSceneObjects = {state.files[0].objectId, state.nextObjectId};
    unavailable();
    state.selectedSceneObjects = {state.files[0].objectId, state.files[1].objectId};
    state.files[1].mesh.indices.clear();
    unavailable();
}

TEST_CASE("tree selection follows file identity through removal and prunes removed groups and folders")
{
    auto state = stateWithFiles(3);
    const auto first = state.files[1].objectId;
    const auto second = state.files[2].objectId;
    woby::selectSceneObject(state, first);
    woby::selectSceneObject(state, second, true);
    REQUIRE(woby::removeFileFromState(state, 0));
    CHECK(state.selectedSceneObjects == std::vector<woby::SceneObjectId>{first, second});
    REQUIRE(woby::compareSceneSelection(state));
    CHECK(state.comparison.originalFile == 0);
    CHECK(state.comparison.repairedFile == 1);
    woby::UiSceneNode folder;
    folder.name = "parent";
    folder.children.push_back(state.sceneNodes[0]);
    state.sceneNodes[0] = folder;
    woby::assignSceneObjectIds(state);
    woby::selectSceneObject(state, state.sceneNodes[0].objectId, true);
    woby::selectSceneObject(state, state.files[0].groupSettings[0].objectId, true);
    REQUIRE(woby::removeFileFromState(state, 0));
    CHECK(state.selectedSceneObjects == std::vector<woby::SceneObjectId>{second});
    CHECK_FALSE(woby::canCompareSceneSelection(state));
    CHECK_FALSE(state.comparison.enabled);
}

TEST_CASE("comparison settings round trip through scene save and replacement with dirty tracking")
{
    auto state = stateWithFiles(2);
    const auto clean = woby::createSceneDocument(state);
    auto settings = state.comparison;
    settings.enabled = true;
    settings.originalFile = 0;
    settings.repairedFile = 1;
    settings.mode = woby::ComparisonMode::overlay;
    settings.distanceOnOriginal = true;
    settings.tolerance = .12f;
    settings.colorRange = 1.5f;
    settings.showEdges = true;
    settings.showBoundaries = false;
    settings.showNonManifold = false;
    woby::setComparisonSettings(state, settings);
    woby::updateSceneDirty(state, clean);
    CHECK(state.isDirty);
    const auto document = woby::createSceneDocument(state);
    const auto path = std::filesystem::temp_directory_path() / "woby-comparison-roundtrip.woby";
    woby::writeSceneDocument(path, document);
    const auto read = woby::readSceneDocument(path);
    CHECK(read.comparison == settings);
    const auto replacement = woby::prepareSceneReplacement(state, state.files, read);
    CHECK(replacement.comparison == settings);
    CHECK_FALSE(replacement.isDirty);
    std::filesystem::remove(path);
}

TEST_CASE("prototype sample loads with the expected repairs and comparison settings")
{
    const auto root = std::filesystem::path(__FILE__).parent_path().parent_path() / "assets/samples/mesh-comparison";
    const auto original = woby::loadObjMesh(root / "original.obj");
    const auto repaired = woby::loadObjMesh(root / "repaired.obj");
    CHECK(original.indices.size() / 3 == 153);
    CHECK(repaired.indices.size() / 3 == 160);
    const auto result = woby::compareMeshes(original, repaired);
    CHECK(result.original.diagnostics.boundaryEdges.size() == 47);
    CHECK(result.repaired.diagnostics.boundaryEdges.size() == 36);
    CHECK(result.original.maximum > 1);
    CHECK(result.repaired.maximum > .4);
    CHECK(result.repaired.diagnostics.nonManifoldEdges.empty());
    const auto scene = woby::readSceneDocument(root / "compare.woby");
    CHECK(scene.comparison.enabled);
    CHECK(scene.comparison.originalFile == 0);
    CHECK(scene.comparison.repairedFile == 1);
}

TEST_CASE("scene comparison load normalizes invalid values and disables stale file selections")
{
    const auto path = std::filesystem::temp_directory_path() / "woby-comparison-invalid.woby";
    {
        std::ofstream out(path);
        out << "version = 2\ncomparison_enabled = true\ncomparison_original_file = 20\ncomparison_repaired_file = -2\n"
               "comparison_tolerance = nan\ncomparison_color_range = -10\n[[files]]\npath = \"mesh.obj\"\n";
    }
    const auto document = woby::readSceneDocument(path);
    CHECK_FALSE(document.comparison.enabled);
    CHECK(document.comparison.originalFile == -1);
    CHECK(document.comparison.repairedFile == -1);
    CHECK(document.comparison.tolerance == doctest::Approx(.05));
    CHECK(document.comparison.colorRange >= document.comparison.tolerance);
    std::filesystem::remove(path);
}

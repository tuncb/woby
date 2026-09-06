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

TEST_CASE("comparison groups assemble parts across files and compare combined surfaces")
{
    woby::UiState state;
    auto first = square(), second = square();
    first.indices = {0, 1, 2};
    second.indices = {0, 2, 3};
    first.nodes[0].indexCount = second.nodes[0].indexCount = 3;
    state.files.push_back(woby::createUiFileState("first.obj", first, 0));
    state.files.push_back(woby::createUiFileState("second.obj", second, 0));
    state.files.push_back(woby::createUiFileState("whole.obj", square(), 0));
    woby::appendDefaultSceneNodesForFiles(state, 0);
    woby::setComparisonObjects(state, {state.files[0].objectId, state.files[1].objectId}, woby::ComparisonSide::a, true);
    woby::setComparisonObjects(state, {state.files[2].objectId}, woby::ComparisonSide::b, true);
    REQUIRE(woby::canCompareGroups(state));
    const auto a = woby::comparisonWorldMesh(state, woby::ComparisonSide::a);
    const auto b = woby::comparisonWorldMesh(state, woby::ComparisonSide::b);
    CHECK(a.indices.size() == 6);
    CHECK(b.indices.size() == 6);
    const auto result = woby::compareMeshes(a, b);
    CHECK(result.original.maximum < 1e-12);
    CHECK(result.repaired.maximum < 1e-12);
}

TEST_CASE("folder and file comparison actions expand current parts without duplicate membership")
{
    auto state = stateWithFiles(2);
    woby::UiSceneNode folder;
    folder.name = "assembly";
    folder.children = std::move(state.sceneNodes);
    state.sceneNodes = {folder};
    woby::assignSceneObjectIds(state);
    const auto folderId = state.sceneNodes[0].objectId;
    const auto partId = state.files[0].groupSettings[0].objectId;
    const std::vector<woby::SceneObjectId> objects = {folderId, state.files[0].objectId, partId, partId};
    CHECK(woby::comparisonObjectParts(state, objects).size() == 2);
    woby::setComparisonObjects(state, objects, woby::ComparisonSide::a, true);
    woby::setComparisonObjects(state, objects, woby::ComparisonSide::a, true);
    CHECK(woby::comparisonPartCount(state, woby::ComparisonSide::a) == 2);
    CHECK(woby::comparisonWorldMesh(state, woby::ComparisonSide::a).indices.size() == 12);
    woby::setComparisonObjects(state, {partId}, woby::ComparisonSide::a, false);
    CHECK_FALSE(state.files[0].groupSettings[0].comparison.a);
    CHECK(state.files[1].groupSettings[0].comparison.a);
    state.files.push_back(woby::createUiFileState("new.obj", square(), 0));
    state.sceneNodes[0].children.push_back(woby::createFileSceneNode(state.files.back(), 2));
    woby::assignSceneObjectIds(state);
    CHECK_FALSE(state.files.back().groupSettings[0].comparison.a);
    woby::setComparisonObjects(state, {folderId}, woby::ComparisonSide::a, false);
    CHECK(woby::comparisonPartCount(state, woby::ComparisonSide::a) == 0);
}

TEST_CASE("parts in one file can belong to either or both comparison groups")
{
    woby::UiState state;
    auto model = square();
    model.nodes = {{"first", 0, 3}, {"second", 3, 3}};
    state.files.push_back(woby::createUiFileState("parts.obj", model, 0));
    woby::appendDefaultSceneNodesForFiles(state, 0);
    const auto first = state.files[0].groupSettings[0].objectId;
    const auto second = state.files[0].groupSettings[1].objectId;
    woby::setComparisonObjects(state, {first}, woby::ComparisonSide::a, true);
    woby::setComparisonObjects(state, {second}, woby::ComparisonSide::b, true);
    REQUIRE(woby::canCompareGroups(state));
    CHECK(woby::comparisonWorldMesh(state, woby::ComparisonSide::a).indices.size() == 3);
    CHECK(woby::comparisonWorldMesh(state, woby::ComparisonSide::b).indices.size() == 3);
    woby::setComparisonObjects(state, {first}, woby::ComparisonSide::b, true);
    CHECK(state.files[0].groupSettings[0].comparison.a);
    CHECK(state.files[0].groupSettings[0].comparison.b);
    CHECK(woby::comparisonPartCount(state, woby::ComparisonSide::b) == 2);
    woby::swapComparisonGroups(state);
    CHECK(woby::comparisonPartCount(state, woby::ComparisonSide::a) == 2);
    CHECK(woby::comparisonPartCount(state, woby::ComparisonSide::b) == 1);
    auto settings = state.comparison;
    settings.enabled = true;
    woby::setComparisonSettings(state, settings);
    REQUIRE(state.comparison.enabled);
    woby::clearComparisonGroup(state, woby::ComparisonSide::b);
    CHECK_FALSE(state.comparison.enabled);
    CHECK(woby::comparisonPartCount(state, woby::ComparisonSide::a) == 2);
    CHECK_THROWS((void)woby::comparisonWorldMesh(state, woby::ComparisonSide::b));
}

TEST_CASE("comparison selection ignores stale IDs empty folders and objects without triangles")
{
    auto state = stateWithFiles(2);
    state.files[1].mesh.indices.clear();
    woby::UiSceneNode folder;
    state.sceneNodes.push_back(folder);
    woby::assignSceneObjectIds(state);
    const auto clean = woby::createSceneDocument(state);
    const std::vector<woby::SceneObjectId> ids = {0, state.nextObjectId, state.sceneNodes.back().objectId,
        state.files[1].objectId, state.files[1].groupSettings[0].objectId};
    CHECK(woby::comparisonObjectParts(state, ids).empty());
    woby::setComparisonObjects(state, ids, woby::ComparisonSide::a, true);
    woby::updateSceneDirty(state, clean);
    CHECK_FALSE(state.isDirty);
    CHECK_FALSE(woby::canCompareGroups(state));
    auto settings = state.comparison;
    settings.enabled = true;
    woby::setComparisonSettings(state, settings);
    CHECK_FALSE(state.comparison.enabled);
    CHECK(woby::comparisonGeometrySignature(state) == 0);
}

TEST_CASE("comparison snapshots follow hierarchy transforms and ignore ordinary visibility")
{
    auto state = stateWithFiles(3);
    woby::setFileTranslation(state.files[0].fileSettings, {2, 0, 0});
    woby::setGroupTranslation(state.files[0].groupSettings[0], {1, 0, 0});
    woby::UiSceneNode folder;
    folder.name = "parent";
    folder.settings.translation = {3, 0, 0};
    folder.children.push_back(state.sceneNodes[0]);
    state.sceneNodes[0] = folder;
    woby::assignSceneObjectIds(state);
    woby::setComparisonObjects(state, {state.files[0].objectId}, woby::ComparisonSide::a, true);
    woby::setComparisonObjects(state, {state.files[1].objectId}, woby::ComparisonSide::b, true);
    state.files[0].fileSettings.visible = false;
    const auto transformed = woby::comparisonWorldMesh(state, woby::ComparisonSide::a);
    CHECK(transformed.vertices[0].position[0] == doctest::Approx(6));
    CHECK(state.files[0].mesh.vertices[0].position[0] == 0);
    const auto signature = woby::comparisonGeometrySignature(state);
    state.files[0].groupSettings[0].color = {1, 1, 1, 1};
    state.files[0].groupSettings[0].opacity = .2f;
    state.files[0].fileSettings.visible = true;
    state.sceneNodes[0].settings.visible = false;
    woby::setGroupTranslation(state.files[2].groupSettings[0], {100, 0, 0});
    woby::setFileTranslation(state.files[2].fileSettings, {100, 0, 0});
    CHECK(signature == woby::comparisonGeometrySignature(state));
    woby::setGroupTranslation(state.files[0].groupSettings[0], {4, 0, 0});
    CHECK(signature != woby::comparisonGeometrySignature(state));
    const auto changed = woby::comparisonGeometrySignature(state);
    state.sceneNodes[0].settings.scale = 2;
    CHECK(changed != woby::comparisonGeometrySignature(state));
    const auto scaled = woby::comparisonGeometrySignature(state);
    woby::setComparisonObjects(state, {state.files[2].objectId}, woby::ComparisonSide::a, true);
    CHECK(scaled != woby::comparisonGeometrySignature(state));
    const auto added = woby::comparisonGeometrySignature(state);
    woby::swapComparisonGroups(state);
    CHECK(added != woby::comparisonGeometrySignature(state));
}

TEST_CASE("comparison supports implicit scene trees and deduplicates repeated part references")
{
    auto state = stateWithFiles(1);
    woby::setComparisonObjects(state, {state.files[0].objectId}, woby::ComparisonSide::a, true);
    state.sceneNodes[0].children.push_back(state.sceneNodes[0].children[0]);
    CHECK(woby::comparisonWorldMesh(state, woby::ComparisonSide::a).indices.size() == 6);
    state.sceneNodes[0].children.clear();
    CHECK(woby::comparisonWorldMesh(state, woby::ComparisonSide::a).indices.size() == 6);
    state.sceneNodes.clear();
    CHECK(woby::comparisonObjectParts(state, {state.files[0].objectId}).size() == 1);
    CHECK(woby::comparisonWorldMesh(state, woby::ComparisonSide::a).indices.size() == 6);
}

TEST_CASE("comparison triangle limit applies to selected parts across the whole side")
{
    auto state = stateWithFiles(2);
    for (auto& file : state.files) {
        file.mesh.indices.resize(30000 * 3, 0);
        file.mesh.nodes[0].indexCount = 30000 * 3;
    }
    woby::setComparisonObjects(state, {state.files[0].objectId, state.files[1].objectId}, woby::ComparisonSide::a, true);
    CHECK_THROWS_WITH((void)woby::comparisonWorldMesh(state, woby::ComparisonSide::a),
        "Prototype comparison supports up to 50,000 triangles per comparison group.");
    auto& file = state.files[0];
    file.mesh.indices.resize(60000 * 3, 0);
    file.mesh.nodes[0].indexCount = 3;
    woby::setComparisonObjects(state, {state.files[1].objectId}, woby::ComparisonSide::a, false);
    CHECK(woby::comparisonWorldMesh(state, woby::ComparisonSide::a).indices.size() == 3);
    file.mesh.vertices[0].position[0] = std::numeric_limits<float>::infinity();
    CHECK_THROWS((void)woby::comparisonWorldMesh(state, woby::ComparisonSide::a));
}

TEST_CASE("comparison settings clamp and memberships survive unrelated file removal")
{
    auto state = stateWithFiles(3);
    const auto first = state.files[1].objectId, second = state.files[2].objectId;
    woby::selectSceneObject(state, first);
    woby::selectSceneObject(state, second, true);
    woby::setComparisonObjects(state, {first}, woby::ComparisonSide::a, true);
    woby::setComparisonObjects(state, {second}, woby::ComparisonSide::b, true);
    auto settings = state.comparison;
    settings.enabled = true;
    settings.tolerance = NAN;
    settings.colorRange = -4;
    woby::setComparisonSettings(state, settings);
    CHECK(state.comparison.tolerance == doctest::Approx(.05));
    CHECK(state.comparison.colorRange >= state.comparison.tolerance);
    REQUIRE(woby::removeFileFromState(state, 0));
    CHECK(state.comparison.enabled);
    CHECK(state.files[0].objectId == first);
    CHECK(state.files[0].groupSettings[0].comparison.a);
    CHECK(state.files[1].groupSettings[0].comparison.b);
    CHECK(state.selectedSceneObjects == std::vector<woby::SceneObjectId>{first, second});
    REQUIRE(woby::removeFileFromState(state, 0));
    CHECK_FALSE(state.comparison.enabled);
    CHECK(woby::comparisonPartCount(state, woby::ComparisonSide::a) == 0);
    CHECK(woby::comparisonPartCount(state, woby::ComparisonSide::b) == 1);
    CHECK(state.selectedSceneObjects == std::vector<woby::SceneObjectId>{second});
}

TEST_CASE("comparison memberships and settings round trip with fresh IDs and dirty tracking")
{
    const auto path = std::filesystem::temp_directory_path() / "woby-comparison-roundtrip.woby";
    auto state = stateWithFiles(3);
    for (auto& file : state.files) { file.path = path.parent_path() / file.path; }
    const auto clean = woby::createSceneDocument(state);
    woby::setComparisonObjects(state, {state.files[0].objectId, state.files[1].objectId}, woby::ComparisonSide::a, true);
    woby::setComparisonObjects(state, {state.files[1].objectId, state.files[2].objectId}, woby::ComparisonSide::b, true);
    auto settings = state.comparison;
    settings.enabled = true;
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
    woby::writeSceneDocument(path, document);
    auto read = woby::readSceneDocument(path);
    for (auto& file : read.files) { file.path = woby::sceneAbsolutePath(path, file.path); }
    CHECK(read.comparison == document.comparison);
    CHECK(read.nodes == document.nodes);
    REQUIRE(read.files.size() == document.files.size());
    for (size_t i = 0; i < read.files.size(); ++i) {
        INFO("file index: ", i);
        CHECK(read.files[i].path == document.files[i].path);
        CHECK(read.files[i].groups == document.files[i].groups);
        CHECK(read.files[i].settings == document.files[i].settings);
        CHECK(read.files[i].vertexSizeScale == document.files[i].vertexSizeScale);
    }
    CHECK(read == document);
    auto loadedFiles = state.files;
    for (size_t i = 0; i < loadedFiles.size(); ++i) {
        loadedFiles[i].groupSettings[0].comparison = {};
        woby::applySceneFileRecord(loadedFiles[i], read.files[i]);
        CHECK(loadedFiles[i].groupSettings[0].comparison == state.files[i].groupSettings[0].comparison);
    }
    const auto replacement = woby::prepareSceneReplacement(state, std::move(loadedFiles), read);
    CHECK(replacement.comparison == settings);
    CHECK(woby::createSceneDocument(replacement) == document);
    CHECK(replacement.files[0].groupSettings[0].objectId != state.files[0].groupSettings[0].objectId);
    CHECK(replacement.selectedSceneObjects.empty());
    CHECK_FALSE(replacement.isDirty);
    woby::setComparisonObjects(state, {state.files[0].objectId}, woby::ComparisonSide::a, false);
    woby::updateSceneDirty(state, document);
    CHECK(state.isDirty);
    woby::setComparisonObjects(state, {state.files[0].objectId}, woby::ComparisonSide::a, true);
    woby::updateSceneDirty(state, document);
    CHECK_FALSE(state.isDirty);
    const auto empty = woby::prepareSceneReplacement(state, {}, {});
    CHECK_FALSE(woby::canCompareGroups(empty));
    std::filesystem::remove(path);
}

TEST_CASE("sample loads with repairs and A B comparison membership")
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
    const auto state = woby::prepareSceneReplacement({}, {
        woby::createUiFileState(root / "original.obj", original, 0),
        woby::createUiFileState(root / "repaired.obj", repaired, 0)}, scene);
    CHECK(state.comparison.enabled);
    CHECK(woby::comparisonPartCount(state, woby::ComparisonSide::a) == original.nodes.size());
    CHECK(woby::comparisonPartCount(state, woby::ComparisonSide::b) == repaired.nodes.size());
}

TEST_CASE("comparison load normalizes invalid settings and disables empty sides")
{
    const auto path = std::filesystem::temp_directory_path() / "woby-comparison-invalid.woby";
    {
        std::ofstream out(path);
        out << "version = 4\ncomparison_enabled = true\n"
               "comparison_tolerance = nan\ncomparison_color_range = -10\n[[files]]\npath = \"mesh.obj\"\n";
    }
    const auto document = woby::readSceneDocument(path);
    CHECK_FALSE(document.comparison.enabled);
    CHECK(document.comparison.tolerance == doctest::Approx(.05));
    CHECK(document.comparison.colorRange >= document.comparison.tolerance);
    std::filesystem::remove(path);
}

TEST_CASE("comparison tree preserves folders files and parts and prunes removed children")
{
    auto state = stateWithFiles(3);
    woby::UiSceneNode assembly;
    assembly.name = "Assembly";
    woby::UiSceneNode subassembly;
    subassembly.name = "Subassembly";
    subassembly.children = {state.sceneNodes[0], state.sceneNodes[1]};
    assembly.children = {subassembly, state.sceneNodes[2]};
    state.sceneNodes = {assembly};
    woby::assignSceneObjectIds(state);
    const auto assemblyId = state.sceneNodes[0].objectId;
    woby::setComparisonObjects(state, {assemblyId}, woby::ComparisonSide::a, true);
    woby::setComparisonObjects(state, {state.files[1].objectId}, woby::ComparisonSide::b, true);
    auto tree = woby::comparisonTree(state, woby::ComparisonSide::a);
    REQUIRE(tree.size() == 1);
    CHECK(tree[0].objectId == assemblyId);
    CHECK(tree[0].kind == woby::UiSceneNodeKind::folder);
    CHECK(tree[0].name == "Assembly");
    CHECK(tree[0].partCount == 3);
    CHECK(tree[0].triangleCount == 6);
    REQUIRE(tree[0].children.size() == 2);
    const auto& nested = tree[0].children[0];
    CHECK(nested.name == "Subassembly");
    REQUIRE(nested.children.size() == 2);
    CHECK(nested.children[0].kind == woby::UiSceneNodeKind::file);
    CHECK(nested.children[0].objectId == state.files[0].objectId);
    REQUIRE(nested.children[0].children.size() == 1);
    CHECK(nested.children[0].children[0].kind == woby::UiSceneNodeKind::group);
    CHECK(nested.children[0].children[0].objectId == state.files[0].groupSettings[0].objectId);
    woby::setComparisonObjects(state, {state.files[0].groupSettings[0].objectId}, woby::ComparisonSide::a, false);
    tree = woby::comparisonTree(state, woby::ComparisonSide::a);
    REQUIRE(tree.size() == 1);
    REQUIRE(tree[0].children.size() == 2);
    REQUIRE(tree[0].children[0].children.size() == 1);
    CHECK(tree[0].children[0].children[0].objectId == state.files[1].objectId);
    CHECK(tree[0].partCount == 2);
    // Removing a nested branch affects only its membership on that side.
    woby::setComparisonObjects(state, {tree[0].children[0].objectId}, woby::ComparisonSide::a, false);
    tree = woby::comparisonTree(state, woby::ComparisonSide::a);
    REQUIRE(tree.size() == 1);
    REQUIRE(tree[0].children.size() == 1);
    CHECK(tree[0].children[0].objectId == state.files[2].objectId);
    CHECK(woby::comparisonPartCount(state, woby::ComparisonSide::b) == 1);
    CHECK(state.files.size() == 3);
    woby::clearComparisonGroup(state, woby::ComparisonSide::a);
    CHECK(woby::comparisonTree(state, woby::ComparisonSide::a).empty());
}

TEST_CASE("comparison trees show only included parts and retain hierarchy after scene reload")
{
    woby::UiState state;
    auto model = square();
    model.nodes = {{"Part one", 0, 3}, {"Part two", 3, 3}};
    state.files.push_back(woby::createUiFileState("assembly.obj", model, 0));
    woby::appendDefaultSceneNodesForFiles(state, 0);
    woby::UiSceneNode folder;
    folder.name = "Folder";
    folder.children = std::move(state.sceneNodes);
    state.sceneNodes = {folder};
    woby::assignSceneObjectIds(state);
    woby::setComparisonObjects(state, {state.sceneNodes[0].objectId}, woby::ComparisonSide::a, true);
    woby::setComparisonObjects(state, {state.files[0].groupSettings[0].objectId}, woby::ComparisonSide::a, false);
    state.files[0].fileSettings.visible = false;
    const auto document = woby::createSceneDocument(state);
    const auto loaded = woby::prepareSceneReplacement(state, state.files, document);
    const auto tree = woby::comparisonTree(loaded, woby::ComparisonSide::a);
    REQUIRE(tree.size() == 1);
    CHECK(tree[0].name == "Folder");
    REQUIRE(tree[0].children.size() == 1);
    REQUIRE(tree[0].children[0].children.size() == 1);
    const auto& part = tree[0].children[0].children[0];
    CHECK(part.name == "Part two");
    CHECK(part.objectId == loaded.files[0].groupSettings[1].objectId);
    CHECK(part.objectId != state.files[0].groupSettings[1].objectId);
    CHECK(tree[0].triangleCount == 1);
    CHECK(woby::comparisonTree(loaded, woby::ComparisonSide::b).empty());
}

TEST_CASE("comparison tree handles implicit hierarchy duplicate references and removed files")
{
    auto state = stateWithFiles(2);
    woby::setComparisonObjects(state, {state.files[0].objectId, state.files[1].objectId}, woby::ComparisonSide::a, true);
    state.sceneNodes[0].children.push_back(state.sceneNodes[0].children[0]);
    auto tree = woby::comparisonTree(state, woby::ComparisonSide::a);
    REQUIRE(tree.size() == 2);
    CHECK(tree[0].partCount == 1);
    CHECK(tree[0].children.size() == 1);
    state.sceneNodes[0].children.clear();
    tree = woby::comparisonTree(state, woby::ComparisonSide::a);
    CHECK(tree[0].children.size() == 1);
    state.sceneNodes.clear();
    tree = woby::comparisonTree(state, woby::ComparisonSide::a);
    REQUIRE(tree.size() == 2);
    CHECK(tree[0].children.size() == 1);
    REQUIRE(woby::removeFileFromState(state, 0));
    tree = woby::comparisonTree(state, woby::ComparisonSide::a);
    REQUIRE(tree.size() == 1);
    CHECK(tree[0].objectId == state.files[0].objectId);
}

TEST_CASE("comparison panel opens on adding objects and starting comparison but can stay hidden")
{
    auto state = stateWithFiles(2);
    CHECK_FALSE(state.comparisonPaneVisible);
    woby::setComparisonObjects(state, {state.nextObjectId}, woby::ComparisonSide::a, true);
    CHECK_FALSE(state.comparisonPaneVisible);
    woby::setComparisonObjects(state, {state.files[0].objectId}, woby::ComparisonSide::a, true);
    CHECK(state.comparisonPaneVisible);
    woby::setComparisonPaneVisible(state, false);
    woby::setComparisonObjects(state, {state.files[1].objectId}, woby::ComparisonSide::b, true);
    CHECK(state.comparisonPaneVisible);
    woby::setComparisonPaneVisible(state, false);
    auto settings = state.comparison;
    settings.enabled = true;
    woby::setComparisonSettings(state, settings);
    REQUIRE(state.comparison.enabled);
    CHECK(state.comparisonPaneVisible);
    const auto document = woby::createSceneDocument(state);
    const auto signature = woby::comparisonGeometrySignature(state);
    woby::setComparisonPaneVisible(state, false);
    woby::updateSceneDirty(state, document);
    CHECK_FALSE(state.isDirty);
    CHECK(state.comparison.enabled);
    CHECK(woby::comparisonGeometrySignature(state) == signature);
    // Editing display settings while hidden should not force the panel open again.
    settings.colorRange = 2;
    woby::setComparisonSettings(state, settings);
    CHECK_FALSE(state.comparisonPaneVisible);
    woby::setComparisonObjects(state, {state.files[0].objectId}, woby::ComparisonSide::a, false);
    CHECK_FALSE(state.comparisonPaneVisible);
    CHECK_FALSE(state.comparison.enabled);
    const auto restored = woby::prepareSceneReplacement(state, state.files, document);
    CHECK(restored.comparisonPaneVisible);
    CHECK(restored.comparison.enabled);
}

TEST_CASE("quick comparison assigns selected objects in click order and opens the panel")
{
    auto state = stateWithFiles(2);
    auto settings = state.comparison;
    settings.mode = woby::ComparisonMode::overlay;
    settings.tolerance = .25f;
    woby::setComparisonSettings(state, settings);
    const auto clean = woby::createSceneDocument(state);
    const auto first = state.files[1].objectId, second = state.files[0].objectId;
    woby::selectSceneObject(state, first);
    woby::selectSceneObject(state, second, true);
    woby::selectSceneObject(state, first, false, true);
    REQUIRE(woby::canCompareSceneSelection(state));
    REQUIRE(woby::compareSceneSelection(state));
    CHECK(state.comparison.enabled);
    CHECK(state.comparisonPaneVisible);
    CHECK(state.comparison.mode == woby::ComparisonMode::overlay);
    CHECK(state.comparison.tolerance == doctest::Approx(.25));
    CHECK(state.files[1].groupSettings[0].comparison.a);
    CHECK_FALSE(state.files[1].groupSettings[0].comparison.b);
    CHECK(state.files[0].groupSettings[0].comparison.b);
    CHECK_FALSE(state.files[0].groupSettings[0].comparison.a);
    CHECK(state.selectedSceneObjects == std::vector<woby::SceneObjectId>{first, second});
    woby::updateSceneDirty(state, clean);
    CHECK(state.isDirty);
    const auto document = woby::createSceneDocument(state);
    const auto restored = woby::prepareSceneReplacement(state, state.files, document);
    CHECK(restored.comparison.enabled);
    CHECK(restored.files[1].groupSettings[0].comparison.a);
    CHECK(restored.files[0].groupSettings[0].comparison.b);
}

TEST_CASE("quick comparison supports folders and mesh parts including shared descendants")
{
    auto state = stateWithFiles(2);
    woby::UiSceneNode folder;
    folder.name = "Assembly";
    folder.children = std::move(state.sceneNodes);
    state.sceneNodes = {folder};
    woby::assignSceneObjectIds(state);
    woby::selectSceneObject(state, state.sceneNodes[0].objectId);
    woby::selectSceneObject(state, state.files[0].groupSettings[0].objectId, true);
    REQUIRE(woby::compareSceneSelection(state));
    CHECK(woby::comparisonPartCount(state, woby::ComparisonSide::a) == 2);
    CHECK(woby::comparisonPartCount(state, woby::ComparisonSide::b) == 1);
    const auto tree = woby::comparisonTree(state, woby::ComparisonSide::a);
    REQUIRE(tree.size() == 1);
    CHECK(tree[0].name == "Assembly");
    CHECK(tree[0].children.size() == 2);
}

TEST_CASE("quick comparison leaves existing memberships and invalid selections untouched")
{
    auto state = stateWithFiles(3);
    const auto unchanged = [&] {
        const auto document = woby::createSceneDocument(state);
        const auto selection = state.selectedSceneObjects;
        const bool visible = state.comparisonPaneVisible;
        CHECK_FALSE(woby::canCompareSceneSelection(state));
        CHECK_FALSE(woby::compareSceneSelection(state));
        CHECK(woby::createSceneDocument(state) == document);
        CHECK(state.selectedSceneObjects == selection);
        CHECK(state.comparisonPaneVisible == visible);
    };
    unchanged();
    woby::selectSceneObject(state, state.files[0].objectId);
    unchanged();
    woby::selectSceneObject(state, state.files[1].objectId, true);
    woby::selectSceneObject(state, state.files[2].objectId, true);
    unchanged();
    state.selectedSceneObjects = {state.files[0].objectId, state.files[0].objectId};
    unchanged();
    state.selectedSceneObjects = {state.files[0].objectId, state.nextObjectId};
    unchanged();
    state.selectedSceneObjects = {state.files[0].objectId, 0};
    unchanged();
    woby::UiSceneNode empty;
    state.sceneNodes.push_back(empty);
    woby::assignSceneObjectIds(state);
    state.selectedSceneObjects = {state.files[0].objectId, state.sceneNodes.back().objectId};
    unchanged();
    state.selectedSceneObjects = {state.files[0].objectId, state.files[1].objectId};
    for (const auto side : {woby::ComparisonSide::a, woby::ComparisonSide::b}) {
        woby::setComparisonObjects(state, {state.files[2].objectId}, side, true);
        unchanged();
        woby::clearComparisonGroup(state, side);
    }
    state.files[1].mesh.indices.clear();
    unchanged();
}

TEST_CASE("comparison membership menu chooses add or remove independently for each side")
{
    auto state = stateWithFiles(2);
    const auto object = state.files[0].objectId;
    using Action = woby::ComparisonMembershipAction;
    CHECK(woby::comparisonMembershipAction(state, {object}, woby::ComparisonSide::a) == Action::add);
    CHECK(woby::comparisonMembershipAction(state, {object}, woby::ComparisonSide::b) == Action::add);
    woby::setComparisonObjects(state, {object}, woby::ComparisonSide::a, true);
    CHECK(woby::comparisonMembershipAction(state, {object}, woby::ComparisonSide::a) == Action::remove);
    CHECK(woby::comparisonMembershipAction(state, {object}, woby::ComparisonSide::b) == Action::add);
    woby::setComparisonObjects(state, {object}, woby::ComparisonSide::b, true);
    CHECK(woby::comparisonMembershipAction(state, {object}, woby::ComparisonSide::b) == Action::remove);
    woby::setComparisonObjects(state, {object}, woby::ComparisonSide::a, false);
    CHECK(woby::comparisonMembershipAction(state, {object}, woby::ComparisonSide::a) == Action::add);
    CHECK(woby::comparisonMembershipAction(state, {object}, woby::ComparisonSide::b) == Action::remove);
    // Other members on either side do not influence the selected object's action.
    CHECK(woby::comparisonMembershipAction(state, {state.files[1].objectId}, woby::ComparisonSide::b) == Action::add);
}

TEST_CASE("mixed comparison selections add missing parts before offering removal")
{
    auto state = stateWithFiles(3);
    woby::UiSceneNode folder;
    folder.name = "Assembly";
    folder.children = {state.sceneNodes[0], state.sceneNodes[1]};
    state.sceneNodes = {folder, state.sceneNodes[2]};
    woby::assignSceneObjectIds(state);
    const auto parent = state.sceneNodes[0].objectId;
    const auto child = state.files[0].groupSettings[0].objectId;
    const std::vector<woby::SceneObjectId> selection = {parent, child, state.files[0].objectId};
    using Action = woby::ComparisonMembershipAction;
    woby::setComparisonObjects(state, {child}, woby::ComparisonSide::a, true);
    CHECK(woby::comparisonMembershipAction(state, selection, woby::ComparisonSide::a) == Action::add);
    woby::setComparisonObjects(state, selection, woby::ComparisonSide::a, true);
    CHECK(woby::comparisonMembershipAction(state, selection, woby::ComparisonSide::a) == Action::remove);
    CHECK(woby::comparisonPartCount(state, woby::ComparisonSide::a) == 2);
    CHECK_FALSE(state.files[2].groupSettings[0].comparison.a);
    const std::vector<woby::SceneObjectId> multiple = {state.files[0].objectId, state.files[2].objectId};
    CHECK(woby::comparisonMembershipAction(state, multiple, woby::ComparisonSide::a) == Action::add);
    woby::setComparisonObjects(state, selection, woby::ComparisonSide::a, false);
    CHECK(woby::comparisonMembershipAction(state, selection, woby::ComparisonSide::a) == Action::add);
    CHECK(woby::comparisonPartCount(state, woby::ComparisonSide::a) == 0);
}

TEST_CASE("comparison membership menu is unavailable without any comparable parts")
{
    auto state = stateWithFiles(1);
    using Action = woby::ComparisonMembershipAction;
    CHECK(woby::comparisonMembershipAction(state, {}, woby::ComparisonSide::a) == Action::unavailable);
    CHECK(woby::comparisonMembershipAction(state, {0, state.nextObjectId}, woby::ComparisonSide::b) == Action::unavailable);
    state.files[0].mesh.indices.clear();
    CHECK(woby::comparisonMembershipAction(state, {state.files[0].objectId}, woby::ComparisonSide::a) == Action::unavailable);
}

#include "mesh_comparison.h"
#include "comparison_scene.h"
#include "ui_operations.h"
#include "obj_mesh.h"
#include "control_scene.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <future>
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
woby::Mesh largeGrid(uint32_t width, float z = 0)
{
    woby::Mesh result;
    for (uint32_t y = 0; y <= width; ++y)
        for (uint32_t x = 0; x <= width; ++x)
        {
            woby::Vertex vertex;
            vertex.position = {static_cast<float>(x), static_cast<float>(y), z};
            result.vertices.push_back(vertex);
        }
    for (uint32_t y = 0; y < width; ++y)
        for (uint32_t x = 0; x < width; ++x)
        {
            const auto a = y * (width + 1) + x;
            result.indices.insert(result.indices.end(), {a, a + 1, a + width + 2, a, a + width + 2, a + width + 1});
        }
    result.nodes.push_back({"grid", 0, static_cast<uint32_t>(result.indices.size())});
    result.bounds = woby::calculateBounds(result.vertices);
    return result;
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

TEST_CASE("comparison objects share inputs and keep independent settings and geometry signatures")
{
    auto state = stateWithFiles(3);
    const auto first = woby::createComparison(state);
    woby::setComparisonObjects(state, {state.files[0].objectId}, woby::ComparisonSide::a, true, first);
    woby::setComparisonObjects(state, {state.files[1].objectId}, woby::ComparisonSide::b, true, first);
    const auto second = woby::createComparison(state);
    woby::setComparisonObjects(state, {state.files[0].objectId}, woby::ComparisonSide::a, true, second);
    woby::setComparisonObjects(state, {state.files[2].objectId}, woby::ComparisonSide::b, true, second);
    REQUIRE(woby::canCompareGroups(state, first));
    REQUIRE(woby::canCompareGroups(state, second));
    const auto a = woby::comparisonGeometrySignature(state, first);
    const auto b = woby::comparisonGeometrySignature(state, second);
    woby::setComparisonTranslation(state, first, {20, -3, 2});
    auto settings = woby::comparisonSettings(state, first);
    settings.tolerance = .3f;
    settings.mode = woby::ComparisonMode::overlay;
    settings.enabled = false;
    woby::setComparisonSettings(state, settings, first);
    CHECK(woby::comparisonSettings(state, second).enabled);
    CHECK(woby::comparisonSettings(state, second).tolerance == doctest::Approx(.05));
    CHECK(woby::comparisonGeometrySignature(state, first) == a);
    CHECK(woby::comparisonGeometrySignature(state, second) == b);
    woby::setGroupTranslation(state.files[1].groupSettings[0], {0, 0, 1});
    CHECK(woby::comparisonGeometrySignature(state, first) != a);
    CHECK(woby::comparisonGeometrySignature(state, second) == b);
    const auto measured = woby::compareMeshes(woby::comparisonWorldMesh(state, woby::ComparisonSide::a, first),
        woby::comparisonWorldMesh(state, woby::ComparisonSide::b, first));
    CHECK(measured.original.maximum == doctest::Approx(1));
    woby::setGroupTranslation(state.files[0].groupSettings[0], {0, 0, 2});
    CHECK(woby::comparisonGeometrySignature(state, second) != b);
    woby::selectSceneObject(state, first);
    CHECK(state.activeComparisonId == first);
    woby::selectSceneObject(state, state.files[2].objectId);
    CHECK(state.activeComparisonId == first);
    CHECK(woby::comparisonObjectParts(state, {first, second}).empty());
}

TEST_CASE("comparison duplication deletion and selection preserve source ownership")
{
    auto state = stateWithFiles(2);
    const auto sourceIds = woby::sceneObjects(state);
    const auto id = woby::createComparison(state);
    woby::setComparisonObjects(state, {state.files[0].objectId}, woby::ComparisonSide::a, true, id);
    woby::setComparisonObjects(state, {state.files[1].objectId}, woby::ComparisonSide::b, true, id);
    woby::renameComparison(state, id, "Repair check");
    const auto copy = woby::duplicateComparison(state, id);
    REQUIRE(copy != id);
    REQUIRE(woby::findComparison(state, copy));
    CHECK(woby::findComparison(state, copy)->name == "Repair check copy");
    CHECK(woby::findComparison(state, copy)->a == woby::findComparison(state, id)->a);
    CHECK(woby::findComparison(state, copy)->translation != woby::findComparison(state, id)->translation);
    CHECK(woby::findSceneObject(state, copy)->kind == woby::SceneObjectKind::comparison);
    woby::clearComparisonGroup(state, woby::ComparisonSide::a, copy);
    CHECK(woby::canCompareGroups(state, id));
    CHECK_FALSE(woby::canCompareGroups(state, copy));
    woby::removeComparison(state, copy);
    CHECK(state.activeComparisonId == id);
    CHECK_FALSE(woby::findSceneObject(state, copy));
    woby::removeComparison(state, id);
    CHECK(state.activeComparisonId == woby::invalidSceneObjectId);
    CHECK(state.selectedSceneObjects.empty());
    CHECK(state.files.size() == 2);
    for (const auto& source : sourceIds) { CHECK(woby::findSceneObject(state, source.id).has_value()); }
    CHECK(woby::createComparison(state) > copy);
}

TEST_CASE("missing comparison inputs block partial results and survive save and reopen")
{
    auto state = stateWithFiles(3);
    const auto id = woby::createComparison(state);
    woby::setComparisonObjects(state, {state.files[0].objectId, state.files[1].objectId}, woby::ComparisonSide::a, true, id);
    woby::setComparisonObjects(state, {state.files[2].objectId}, woby::ComparisonSide::b, true, id);
    const auto missingName = woby::findComparison(state, id)->a[0].name;
    REQUIRE(woby::removeFileFromState(state, 0));
    CHECK(woby::findComparison(state, id)->settings.enabled);
    CHECK(woby::comparisonPartCount(state, woby::ComparisonSide::a, id) == 1);
    CHECK(woby::missingComparisonPartCount(state, id) == 1);
    CHECK_FALSE(woby::canCompareGroups(state, id));
    CHECK(woby::comparisonGeometrySignature(state, id) == 0);
    CHECK_THROWS((void)woby::comparisonWorldMesh(state, woby::ComparisonSide::a, id));
    CHECK_FALSE(woby::comparisonDisplayBounds(state, id));
    const auto document = woby::createSceneDocument(state);
    REQUIRE(document.comparisons[0].a.size() == 2);
    CHECK(document.comparisons[0].a[0].fileIndex == -1);
    CHECK(document.comparisons[0].a[0].name == missingName);
    auto restored = woby::prepareSceneReplacement(state, state.files, document);
    CHECK(woby::missingComparisonPartCount(restored) == 1);
    CHECK(woby::createSceneDocument(restored) == document);
    woby::removeMissingComparisonParts(restored, woby::ComparisonSide::a);
    CHECK(woby::canCompareGroups(restored));
    CHECK(woby::comparisonWorldMesh(restored, woby::ComparisonSide::a).indices.size() == 6);
}

TEST_CASE("comparison offsets affect framing but never measured source coordinates")
{
    auto state = stateWithFiles(2);
    const auto id = woby::createComparison(state);
    woby::setComparisonObjects(state, {state.files[0].objectId}, woby::ComparisonSide::a, true, id);
    woby::setComparisonObjects(state, {state.files[1].objectId}, woby::ComparisonSide::b, true, id);
    woby::setComparisonTranslation(state, id, {20, 30, 40});
    const auto bounds = woby::comparisonDisplayBounds(state, id);
    REQUIRE(bounds);
    CHECK(bounds->min == std::array<float, 3>{20, 30, 40});
    CHECK(bounds->max == std::array<float, 3>{21, 31, 40});
    CHECK(state.sceneBounds.max == bounds->max);
    woby::setFileVisible(state.files[0], false);
    woby::setFileVisible(state.files[1], false);
    woby::recalculateSceneBounds(state);
    CHECK(state.sceneBounds.min == bounds->min);
    CHECK(state.sceneBounds.max == bounds->max);
    woby::setFileVisible(state.files[0], true);
    woby::setFileVisible(state.files[1], true);
    woby::frameComparison(state, id);
    CHECK(state.camera.target == bounds->center);
    woby::setComparisonTranslation(state, id, {NAN, 0, 0});
    CHECK(woby::findComparison(state, id)->translation == std::array<float, 3>{20, 30, 40});
    CHECK(woby::comparisonWorldMesh(state, woby::ComparisonSide::a, id).bounds.min == std::array<float, 3>{0, 0, 0});
    auto settings = woby::comparisonSettings(state, id);
    settings.enabled = false;
    woby::setComparisonSettings(state, settings, id);
    CHECK(state.sceneBounds.max == std::array<float, 3>{1, 1, 0});
}

TEST_CASE("multiple comparison objects round trip with fresh identities and independent dirty tracking")
{
    auto state = stateWithFiles(3);
    const auto path = std::filesystem::temp_directory_path() / "woby-multiple-comparisons.woby";
    for (auto& file : state.files) { file.path = path.parent_path() / file.path; }
    const auto first = woby::createComparison(state);
    woby::setComparisonObjects(state, {state.files[0].objectId}, woby::ComparisonSide::a, true, first);
    woby::setComparisonObjects(state, {state.files[1].objectId}, woby::ComparisonSide::b, true, first);
    const auto second = woby::duplicateComparison(state, first);
    woby::setComparisonObjects(state, {state.files[2].objectId}, woby::ComparisonSide::b, true, second);
    woby::renameComparison(state, second, "Repair \"B\" \\ check");
    auto settings = woby::comparisonSettings(state, second);
    settings.enabled = false;
    settings.tolerance = .15f;
    woby::setComparisonSettings(state, settings, second);
    const auto document = woby::createSceneDocument(state);
    woby::writeSceneDocument(path, document);
    auto read = woby::readSceneDocument(path);
    for (auto& file : read.files) { file.path = woby::sceneAbsolutePath(path, file.path); }
    CHECK(read == document);
    const auto restored = woby::prepareSceneReplacement(state, state.files, read);
    REQUIRE(restored.comparisons.size() == 2);
    CHECK(restored.comparisons[0].objectId > second);
    CHECK(restored.comparisons[1].a[0].objectId == restored.files[0].groupSettings[0].objectId);
    CHECK(woby::createSceneDocument(restored) == document);
    woby::selectSceneObject(state, first);
    woby::setComparisonPaneVisible(state, false);
    woby::updateSceneDirty(state, document);
    CHECK_FALSE(state.isDirty);
    woby::setComparisonTranslation(state, second, {9, 8, 7});
    woby::updateSceneDirty(state, document);
    CHECK(state.isDirty);
    CHECK(woby::findComparison(state, first)->translation == document.comparisons[0].translation);
    std::filesystem::remove(path);
}

TEST_CASE("comparison control inspection visibility and translation use object identities")
{
    CHECK(woby::controlCapabilities()["objectKinds"].back() == "comparison");
    CHECK(woby::controlCapabilities()["comparisonTransformFields"] == nlohmann::json::array({"translation"}));
    auto state = stateWithFiles(2);
    const auto id = woby::createComparison(state);
    woby::setComparisonObjects(state, {state.files[0].objectId}, woby::ComparisonSide::a, true, id);
    woby::setComparisonObjects(state, {state.files[1].objectId}, woby::ComparisonSide::b, true, id);
    const auto format = [](woby::SceneObjectId object) { return std::to_string(object); };
    const auto clean = woby::createSceneDocument(state);
    const auto details = woby::controlObjectDetails(state, id, format);
    CHECK(details["valid"] == true);
    CHECK(details["a"][0]["id"] == format(state.files[0].groupSettings[0].objectId));
    CHECK(details["occurrences"].size() == 1);
    CHECK(woby::controlSceneTree(state, format).back()["kind"] == "comparison");
    woby::ControlOperation command;
    command.objectId = id;
    command.target = format(id);
    command.action = woby::ControlAction::transformSet;
    command.translation = std::array<float, 3>{3, 4, 5};
    woby::applyControlSceneOperation(state, clean, command, format, 100, 800);
    CHECK(woby::findComparison(state, id)->translation == *command.translation);
    command.scale = 2.0f;
    CHECK_THROWS(woby::applyControlSceneOperation(state, clean, command, format, 100, 800));
    CHECK(woby::findComparison(state, id)->translation == *command.translation);
    command = {};
    command.objectId = id;
    command.action = woby::ControlAction::visibility;
    command.visible = false;
    woby::applyControlSceneOperation(state, clean, command, format, 100, 800);
    CHECK_FALSE(woby::comparisonSettings(state, id).enabled);
    CHECK(state.files[0].groupSettings[0].visible);
}

TEST_CASE("new and duplicated comparisons are placed beyond existing result bounds")
{
    auto state = stateWithFiles(2);
    const auto first = woby::createComparison(state);
    woby::setComparisonObjects(state, {state.files[0].objectId}, woby::ComparisonSide::a, true, first);
    woby::setComparisonObjects(state, {state.files[1].objectId}, woby::ComparisonSide::b, true, first);
    const auto moved = woby::duplicateComparison(state, first);
    woby::setComparisonTranslation(state, moved, {30, 0, 0});
    const auto copy = woby::duplicateComparison(state, first);
    REQUIRE(woby::comparisonDisplayBounds(state, copy));
    CHECK(woby::comparisonDisplayBounds(state, copy)->min[0] > 31);
    const auto created = woby::createComparison(state);
    woby::setComparisonObjects(state, {state.files[0].objectId}, woby::ComparisonSide::a, true, created);
    woby::setComparisonObjects(state, {state.files[1].objectId}, woby::ComparisonSide::b, true, created);
    CHECK(woby::comparisonDisplayBounds(state, created)->min[0] > woby::comparisonDisplayBounds(state, copy)->max[0]);
}

TEST_CASE("comparison load rejects malformed references and retains changed source layouts as missing")
{
    auto state = stateWithFiles(2);
    const auto id = woby::createComparison(state);
    woby::setComparisonObjects(state, {state.files[0].objectId}, woby::ComparisonSide::a, true, id);
    woby::setComparisonObjects(state, {state.files[1].objectId}, woby::ComparisonSide::b, true, id);
    auto document = woby::createSceneDocument(state);
    const auto path = std::filesystem::temp_directory_path() / "woby-comparison-malformed.woby";
    document.comparisons[0].a[0].groupIndex = 99;
    woby::writeSceneDocument(path, document);
    CHECK_THROWS_WITH((void)woby::readSceneDocument(path), "Comparison references an invalid source part.");
    document = woby::createSceneDocument(state);
    document.comparisons[0].translation = {NAN, 2, 3};
    document.comparisons[0].a.push_back(document.comparisons[0].a[0]);
    woby::writeSceneDocument(path, document);
    const auto normalized = woby::readSceneDocument(path);
    CHECK(normalized.comparisons[0].a.size() == 1);
    CHECK(normalized.comparisons[0].translation == std::array<float, 3>{});
    state.files[0].mesh.nodes[0].name = "replacement group";
    const auto restored = woby::prepareSceneReplacement(state, state.files, normalized);
    CHECK(woby::missingComparisonPartCount(restored) == 1);
    CHECK_FALSE(woby::canCompareGroups(restored));
    std::filesystem::remove(path);
}

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

TEST_CASE("comparison validates empty and invalid input")
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
    CHECK_FALSE(woby::comparisonContains(state, state.files[0].groupSettings[0].objectId, woby::ComparisonSide::a));
    CHECK(woby::comparisonContains(state, state.files[1].groupSettings[0].objectId, woby::ComparisonSide::a));
    state.files.push_back(woby::createUiFileState("new.obj", square(), 0));
    state.sceneNodes[0].children.push_back(woby::createFileSceneNode(state.files.back(), 2));
    woby::assignSceneObjectIds(state);
    CHECK_FALSE(woby::comparisonContains(state, state.files.back().groupSettings[0].objectId, woby::ComparisonSide::a));
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
    CHECK(woby::comparisonContains(state, state.files[0].groupSettings[0].objectId, woby::ComparisonSide::a));
    CHECK(woby::comparisonContains(state, state.files[0].groupSettings[0].objectId, woby::ComparisonSide::b));
    CHECK(woby::comparisonPartCount(state, woby::ComparisonSide::b) == 2);
    woby::swapComparisonGroups(state);
    CHECK(woby::comparisonPartCount(state, woby::ComparisonSide::a) == 2);
    CHECK(woby::comparisonPartCount(state, woby::ComparisonSide::b) == 1);
    auto settings = woby::comparisonSettings(state);
    settings.enabled = true;
    woby::setComparisonSettings(state, settings);
    REQUIRE(woby::comparisonSettings(state).enabled);
    woby::clearComparisonGroup(state, woby::ComparisonSide::b);
    CHECK(woby::comparisonSettings(state).enabled);
    CHECK_FALSE(woby::canCompareGroups(state));
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
    auto settings = woby::comparisonSettings(state);
    settings.enabled = true;
    woby::setComparisonSettings(state, settings);
    CHECK_FALSE(woby::comparisonSettings(state).enabled);
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

TEST_CASE("comparison accepts more than 50000 selected triangles across the whole side")
{
    auto state = stateWithFiles(2);
    for (auto& file : state.files) {
        file.mesh.indices.resize(30000 * 3, 0);
        file.mesh.nodes[0].indexCount = 30000 * 3;
    }
    woby::setComparisonObjects(state, {state.files[0].objectId, state.files[1].objectId}, woby::ComparisonSide::a, true);
    const auto combined = woby::comparisonWorldMesh(state, woby::ComparisonSide::a);
    CHECK(combined.indices.size() == 60000 * 3);
    CHECK(combined.vertices.size() == 60000 * 3);
    auto& file = state.files[0];
    file.mesh.indices.resize(60000 * 3, 0);
    file.mesh.nodes[0].indexCount = 3;
    woby::setComparisonObjects(state, {state.files[1].objectId}, woby::ComparisonSide::a, false);
    CHECK(woby::comparisonWorldMesh(state, woby::ComparisonSide::a).indices.size() == 3);
    file.mesh.vertices[0].position[0] = std::numeric_limits<float>::infinity();
    CHECK_THROWS((void)woby::comparisonWorldMesh(state, woby::ComparisonSide::a));
}

TEST_CASE("comparison measures both surfaces above the former triangle cap")
{
    const auto a = largeGrid(160);
    const auto b = largeGrid(160, .25f);
    REQUIRE(a.indices.size() / 3 > 50000);
    const auto result = woby::compareMeshes(a, b);
    for (const auto* surface : {&result.original, &result.repaired})
    {
        CHECK(surface->maximum == doctest::Approx(.25));
        CHECK(surface->mean == doctest::Approx(.25));
        CHECK(surface->percentile95 == doctest::Approx(.25));
        CHECK(surface->distances.size() == 4 * 51200);
        CHECK(surface->sampled.vertices.size() == 12 * 51200);
        CHECK(surface->sampled.indices.back() == surface->sampled.vertices.size() - 1);
        CHECK(surface->diagnostics.boundaryEdges.size() == 640);
        CHECK(surface->diagnostics.nonManifoldEdges.empty());
        CHECK(woby::surfacePercentAboveTolerance(*surface, .2) == doctest::Approx(100));
        const auto bounds = woby::calculateBounds(surface->sampled.vertices);
        CHECK(surface->sampled.bounds.min == bounds.min);
        CHECK(surface->sampled.bounds.max == bounds.max);
        CHECK(surface->sampled.bounds.center == bounds.center);
        CHECK(surface->sampled.bounds.radius == bounds.radius);
    }
    CHECK(a.vertices.front().position[2] == 0);
    CHECK(b.vertices.front().position[2] == .25f);
}

TEST_CASE("comparison rejects unrepresentable buffers without allocating geometry")
{
    const size_t maxBytes = std::numeric_limits<uint32_t>::max();
    CHECK(woby::comparisonBufferBytes(0, sizeof(woby::Vertex)) == 0);
    CHECK(woby::comparisonBufferBytes(maxBytes, 1) == maxBytes);
    for (const size_t stride : {sizeof(woby::Vertex), sizeof(uint32_t), 2 * sizeof(std::array<float, 3>)})
    {
        CHECK(woby::comparisonBufferBytes(maxBytes / stride, stride) == (maxBytes / stride) * stride);
        CHECK_THROWS((void)woby::comparisonBufferBytes(maxBytes / stride + 1, stride));
        CHECK_THROWS((void)woby::comparisonBufferBytes(std::numeric_limits<size_t>::max(), stride));
    }
    const size_t maxTriangles = maxBytes / (12 * sizeof(woby::Vertex));
    CHECK_NOTHROW(woby::validateComparisonMeshSize(maxTriangles * 3, maxTriangles));
    CHECK_THROWS(woby::validateComparisonMeshSize(3, maxTriangles + 1));
    CHECK_THROWS(woby::validateComparisonMeshSize(maxBytes / sizeof(woby::Vertex) + 1, 1));
}

TEST_CASE("mesh diagnostics honor cancellation")
{
    std::stop_source stop;
    stop.request_stop();
    CHECK_THROWS_WITH((void)woby::inspectMesh(square(), stop.get_token()), "Comparison canceled.");
}

TEST_CASE("large comparison and diagnostics stop during background processing")
{
    const auto input = largeGrid(500);
    for (const bool diagnosticsOnly : {true, false})
    {
        INFO("diagnostics only: ", diagnosticsOnly);
        std::stop_source stop;
        auto worker = std::async(std::launch::async, [&] {
            try
            {
                if (diagnosticsOnly) { (void)woby::inspectMesh(input, stop.get_token()); }
                else { (void)woby::compareMeshes(input, input, stop.get_token()); }
                return std::string{};
            }
            catch (const std::exception& error) { return std::string(error.what()); }
        });
        // Give the worker time to enter a long stage, then cancel. The generous
        // deadline tests responsiveness without depending on exact stage timings.
        (void)worker.wait_for(std::chrono::milliseconds(20));
        stop.request_stop();
        CHECK(worker.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
        CHECK(worker.get() == "Comparison canceled.");
    }
    CHECK(input.indices.size() == 500 * 500 * 6);
    CHECK(input.vertices.front().position == std::array<float, 3>{0, 0, 0});
}

TEST_CASE("comparison settings clamp and memberships survive unrelated file removal")
{
    auto state = stateWithFiles(3);
    const auto first = state.files[1].objectId, second = state.files[2].objectId;
    woby::selectSceneObject(state, first);
    woby::selectSceneObject(state, second, true);
    woby::setComparisonObjects(state, {first}, woby::ComparisonSide::a, true);
    woby::setComparisonObjects(state, {second}, woby::ComparisonSide::b, true);
    auto settings = woby::comparisonSettings(state);
    settings.enabled = true;
    settings.tolerance = NAN;
    settings.colorRange = -4;
    woby::setComparisonSettings(state, settings);
    CHECK(woby::comparisonSettings(state).tolerance == doctest::Approx(.05));
    CHECK(woby::comparisonSettings(state).colorRange >= woby::comparisonSettings(state).tolerance);
    REQUIRE(woby::removeFileFromState(state, 0));
    CHECK(woby::comparisonSettings(state).enabled);
    CHECK(state.files[0].objectId == first);
    CHECK(woby::comparisonContains(state, state.files[0].groupSettings[0].objectId, woby::ComparisonSide::a));
    CHECK(woby::comparisonContains(state, state.files[1].groupSettings[0].objectId, woby::ComparisonSide::b));
    CHECK(state.selectedSceneObjects == std::vector<woby::SceneObjectId>{first, second});
    REQUIRE(woby::removeFileFromState(state, 0));
    CHECK(woby::comparisonSettings(state).enabled);
    CHECK_FALSE(woby::canCompareGroups(state));
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
    auto settings = woby::comparisonSettings(state);
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
    CHECK(read.comparisons == document.comparisons);
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
        woby::applySceneFileRecord(loadedFiles[i], read.files[i]);
    }
    const auto replacement = woby::prepareSceneReplacement(state, std::move(loadedFiles), read);
    CHECK(woby::comparisonSettings(replacement) == settings);
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
    CHECK(woby::comparisonSettings(state).enabled);
    CHECK(woby::comparisonPartCount(state, woby::ComparisonSide::a) == original.nodes.size());
    CHECK(woby::comparisonPartCount(state, woby::ComparisonSide::b) == repaired.nodes.size());
}

TEST_CASE("comparison load normalizes settings and preserves incomplete objects")
{
    const auto path = std::filesystem::temp_directory_path() / "woby-comparison-invalid.woby";
    {
        std::ofstream out(path);
        out << "version = 4\ncomparison_enabled = true\n"
               "comparison_tolerance = nan\ncomparison_color_range = -10\n[[files]]\npath = \"mesh.obj\"\n";
    }
    const auto document = woby::readSceneDocument(path);
    REQUIRE(document.comparisons.size() == 1);
    CHECK(document.comparisons[0].settings.enabled);
    CHECK(document.comparisons[0].a.empty());
    CHECK(document.comparisons[0].settings.tolerance == doctest::Approx(.05));
    CHECK(document.comparisons[0].settings.colorRange >= document.comparisons[0].settings.tolerance);
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
    auto settings = woby::comparisonSettings(state);
    settings.enabled = false;
    woby::setComparisonSettings(state, settings);
    settings.enabled = true;
    woby::setComparisonSettings(state, settings);
    REQUIRE(woby::comparisonSettings(state).enabled);
    CHECK(state.comparisonPaneVisible);
    const auto document = woby::createSceneDocument(state);
    const auto signature = woby::comparisonGeometrySignature(state);
    woby::setComparisonPaneVisible(state, false);
    woby::updateSceneDirty(state, document);
    CHECK_FALSE(state.isDirty);
    CHECK(woby::comparisonSettings(state).enabled);
    CHECK(woby::comparisonGeometrySignature(state) == signature);
    // Editing display settings while hidden should not force the panel open again.
    settings.colorRange = 2;
    woby::setComparisonSettings(state, settings);
    CHECK_FALSE(state.comparisonPaneVisible);
    woby::setComparisonObjects(state, {state.files[0].objectId}, woby::ComparisonSide::a, false);
    CHECK_FALSE(state.comparisonPaneVisible);
    CHECK(woby::comparisonSettings(state).enabled);
    CHECK_FALSE(woby::canCompareGroups(state));
    const auto restored = woby::prepareSceneReplacement(state, state.files, document);
    CHECK_FALSE(restored.comparisonPaneVisible);
    CHECK(woby::comparisonSettings(restored).enabled);
}

TEST_CASE("quick comparison assigns selected objects in click order and opens the panel")
{
    auto state = stateWithFiles(2);
    const auto clean = woby::createSceneDocument(state);
    const auto first = state.files[1].objectId, second = state.files[0].objectId;
    woby::selectSceneObject(state, first);
    woby::selectSceneObject(state, second, true);
    woby::selectSceneObject(state, first, false, true);
    REQUIRE(woby::canCompareSceneSelection(state));
    REQUIRE(woby::compareSceneSelection(state));
    CHECK(woby::comparisonSettings(state).enabled);
    CHECK(state.comparisonPaneVisible);
    CHECK(woby::comparisonSettings(state).mode == woby::ComparisonMode::distance);
    CHECK(woby::comparisonSettings(state).tolerance == doctest::Approx(.05));
    CHECK(woby::comparisonContains(state, state.files[1].groupSettings[0].objectId, woby::ComparisonSide::a));
    CHECK_FALSE(woby::comparisonContains(state, state.files[1].groupSettings[0].objectId, woby::ComparisonSide::b));
    CHECK(woby::comparisonContains(state, state.files[0].groupSettings[0].objectId, woby::ComparisonSide::b));
    CHECK_FALSE(woby::comparisonContains(state, state.files[0].groupSettings[0].objectId, woby::ComparisonSide::a));
    CHECK(state.selectedSceneObjects == std::vector<woby::SceneObjectId>{state.activeComparisonId});
    woby::updateSceneDirty(state, clean);
    CHECK(state.isDirty);
    const auto document = woby::createSceneDocument(state);
    const auto restored = woby::prepareSceneReplacement(state, state.files, document);
    CHECK(woby::comparisonSettings(restored).enabled);
    CHECK(woby::comparisonContains(restored, restored.files[1].groupSettings[0].objectId, woby::ComparisonSide::a));
    CHECK(woby::comparisonContains(restored, restored.files[0].groupSettings[0].objectId, woby::ComparisonSide::b));
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
    woby::setComparisonObjects(state, {state.files[2].objectId}, woby::ComparisonSide::a, true);
    const auto existingId = state.activeComparisonId;
    REQUIRE(woby::compareSceneSelection(state));
    CHECK(state.comparisons.size() == 2);
    CHECK(woby::comparisonPartCount(state, woby::ComparisonSide::a, existingId) == 1);
    state.selectedSceneObjects = {state.files[0].objectId, state.files[1].objectId};
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
    CHECK_FALSE(woby::comparisonContains(state, state.files[2].groupSettings[0].objectId, woby::ComparisonSide::a));
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

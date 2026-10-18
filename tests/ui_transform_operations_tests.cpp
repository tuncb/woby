#include "ui_operations.h"
#include "automation_registry.h"
#include "scene_history.h"
#include "scene_pick.h"
#include "scene_queries.h"
#include "comparison_scene.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <string>

namespace {

struct TransformDirectory {
    std::filesystem::path root = std::filesystem::temp_directory_path()
        / ("woby-transform-" + woby::automationRandomHex(8));
    TransformDirectory() { std::filesystem::create_directory(root); }
    ~TransformDirectory() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
};

woby::Mesh transformMesh(float width, size_t groups = 1)
{
    woby::Mesh mesh;
    auto& geometry = woby::editMeshGeometry(mesh);
    constexpr uint32_t faces[] = {0, 1, 2, 1, 3, 2, 4, 6, 5, 5, 6, 7,
        0, 4, 1, 1, 4, 5, 2, 3, 6, 3, 7, 6, 0, 2, 4, 2, 6, 4, 1, 5, 3, 3, 5, 7};
    for (size_t g = 0; g < groups; ++g) {
        const auto offset = static_cast<uint32_t>(geometry.vertices.size());
        for (size_t corner = 0; corner < 8; ++corner) {
            woby::Vertex vertex;
            vertex.position = {width * (float(g) * 3 + ((corner & 1) ? .5f : -.5f)),
                width * ((corner & 2) ? .25f : -.25f), width * ((corner & 4) ? .125f : -.125f)};
            geometry.vertices.push_back(vertex);
        }
        geometry.nodes.push_back({"part-" + std::to_string(g), static_cast<uint32_t>(geometry.indices.size()), 36});
        for (const auto index : faces) { geometry.indices.push_back(offset + index); }
    }
    mesh.bounds = woby::calculateBounds(geometry.vertices);
    return mesh;
}

struct TransformFixture {
    TransformDirectory directory;
    woby::UiState state;
    TransformFixture()
    {
        for (const float width : {2.f, 4.f, 1.f, 3.f}) {
            const size_t index = state.files.size();
            state.files.push_back(woby::createUiFileState(directory.root / (std::to_string(index) + ".obj"),
                transformMesh(width, index == 0 ? 2 : 1), index));
            state.files.back().fileSettings.translation = {float(index) * 20 + 3, float(index) * -7, float(index) * 4};
        }
        woby::appendDefaultSceneNodesForFiles(state, 0);
        woby::recalculateSceneBounds(state);
        state.selectedSceneObjects = {state.files[2].objectId, state.files[0].objectId, state.files[1].objectId};
    }
};

void checkCoordinate(const woby::Coordinate& actual, const woby::Coordinate& expected, double epsilon = 2e-5)
{
    for (size_t k = 0; k < 3; ++k) { CHECK(actual[k] == doctest::Approx(expected[k]).epsilon(epsilon).scale(1)); }
}

woby::Coordinate rootFilePivot(const woby::UiState& state, size_t index)
{
    const auto& settings = state.files[index].fileSettings;
    woby::CoordinateMatrix matrix;
    woby::fileTransformMatrix(settings, matrix.data());
    return woby::transformCoordinate(matrix.data(), {settings.center[0], settings.center[1], settings.center[2]});
}

woby::Coordinate renderedPoint(const woby::ScenePickPart& part, const woby::Coordinate& point)
{
    woby::CoordinateMatrix matrix;
    std::copy(part.model.begin(), part.model.end(), matrix.begin());
    return woby::transformCoordinate(matrix.data(), point);
}

woby::Coordinate partPivot(const woby::UiState& state, woby::SceneObjectId id)
{
    for (const auto& part : woby::scenePickParts(state, true)) {
        if (part.objectId != id) { continue; }
        const auto& center = state.files[part.fileIndex].groupSettings[part.groupIndex].center;
        return renderedPoint(part, {center[0], center[1], center[2]});
    }
    FAIL("Missing part");
    return {};
}

using WorldPoints = std::vector<std::vector<woby::Coordinate>>;
WorldPoints renderedPoints(const woby::UiState& state)
{
    WorldPoints result(state.files.size());
    for (const auto& part : woby::scenePickParts(state, true)) {
        for (const auto index : woby::scenePartIndices(part)) {
            const auto& p = woby::meshGeometry(*part.mesh).vertices[index].position;
            result[part.fileIndex].push_back(renderedPoint(part, {p[0], p[1], p[2]}));
        }
    }
    return result;
}

std::array<woby::Coordinate, 2> pointBounds(const std::vector<woby::Coordinate>& points)
{
    REQUIRE_FALSE(points.empty());
    std::array<woby::Coordinate, 2> bounds{points.front(), points.front()};
    for (const auto& point : points) {
        for (size_t k = 0; k < 3; ++k) {
            bounds[0][k] = std::min(bounds[0][k], point[k]);
            bounds[1][k] = std::max(bounds[1][k], point[k]);
        }
    }
    return bounds;
}

void checkRigidMove(const WorldPoints& before, const WorldPoints& after, size_t file, const woby::Coordinate& delta)
{
    REQUIRE(before[file].size() == after[file].size());
    for (size_t i = 0; i < before[file].size(); ++i) {
        woby::Coordinate expected;
        for (size_t k = 0; k < 3; ++k) { expected[k] = before[file][i][k] + delta[k]; }
        checkCoordinate(after[file][i], expected);
    }
}

} // namespace

TEST_CASE("part file and folder transforms rotate and scale about their stored pivot")
{
    const auto checkPivot = [&](auto settings, const auto& matrixFor) {
        settings.center = {10, 20, 30};
        settings.translation = {3, 4, 5};
        for (const float scale : {.25f, 1.f, 2.f}) {
            settings.scale = scale;
            for (const auto rotation : {std::array<float,3>{}, std::array<float,3>{0,0,90},
                                       std::array<float,3>{17,-31,48}}) {
                settings.rotationDegrees = rotation;
                woby::CoordinateMatrix matrix;
                matrixFor(settings, matrix.data());
                checkCoordinate(woby::transformCoordinate(matrix.data(), {10,20,30}), {13,24,35});
                // Changing the pivot preserves the linear rotation and scale.
                auto zero = settings; zero.center = {}; zero.translation = {};
                woby::CoordinateMatrix linear;
                matrixFor(zero, linear.data());
                auto expected = woby::transformCoordinate(linear.data(), {1,2,3});
                for (size_t k = 0; k < 3; ++k) { expected[k] += settings.center[k] + settings.translation[k]; }
                checkCoordinate(woby::transformCoordinate(matrix.data(), {11,22,33}), expected);
            }
        }
    };
    checkPivot(woby::UiGroupState{}, [](const auto& s, double* m) { woby::groupTransformMatrix(s,m); });
    checkPivot(woby::UiFileSettings{}, [](const auto& s, double* m) { woby::fileTransformMatrix(s,m); });
    checkPivot(woby::UiSceneNodeSettings{}, [](const auto& s, double* m) { woby::sceneNodeTransformMatrix(s,m); });
}

TEST_CASE("nested transforms apply part then file then folder in rendering queries and analyses")
{
    TransformFixture f;
    auto& state = f.state;
    auto& file = state.files[0];
    file.fileSettings.center = {3,2,1};
    file.fileSettings.rotationDegrees = {23,41,-67};
    file.fileSettings.scale = 2;
    auto& group = file.groupSettings[0];
    group.center = {1,2,3}; group.translation = {2,-3,5};
    group.rotationDegrees = {-37,9,13}; group.scale = .25f;
    woby::UiSceneNode folder;
    folder.kind = woby::UiSceneNodeKind::folder;
    folder.settings.center = {-4,3,1}; folder.settings.translation = {8,1,-4};
    folder.settings.rotationDegrees = {45,3,17}; folder.settings.scale = 3;
    folder.children = {state.sceneNodes[0]}; state.sceneNodes[0] = folder;
    woby::assignSceneObjectIds(state);
    woby::recalculateSceneBounds(state);
    woby::CoordinateMatrix parent, model, local;
    woby::sceneNodeTransformMatrix(folder.settings,parent.data());
    woby::fileTransformMatrix(file.fileSettings,model.data());
    woby::groupTransformMatrix(group,local.data());
    const auto& node = woby::meshGeometry(file.mesh).nodes[0];
    const auto p = woby::meshPosition(file.mesh,woby::meshGeometry(file.mesh).indices[node.indexOffset]);
    const auto expected = woby::transformCoordinate(parent.data(),
        woby::transformCoordinate(model.data(),woby::transformCoordinate(local.data(),p)));
    const auto direct = woby::scenePickParts(state,true);
    checkCoordinate(renderedPoint(direct[0],p),expected);
    woby::SceneQueryRuntime queries;
    std::vector<woby::ScenePickPart> cached;
    woby::resolveSceneParts(queries,state,cached,true);
    checkCoordinate(renderedPoint(cached[0],p),expected);
    REQUIRE(cached[0].coordinateModel);
    checkCoordinate(woby::transformCoordinate(cached[0].coordinateModel->data(),p),expected);
    const auto id = woby::createComparison(state);
    woby::setComparisonObjects(state,{group.objectId},woby::ComparisonSide::a,true,id);
    const auto world = woby::comparisonWorldMesh(state,woby::ComparisonSide::a,id);
    checkCoordinate(woby::meshPosition(world,0),expected);
}

TEST_CASE("selection transforms align current file pivots and leave the reference and other objects fixed")
{
    TransformFixture f;
    auto& state = f.state;
    state.files[0].fileSettings.rotationDegrees = {17, -31, 40};
    state.files[0].fileSettings.scale = 1.5f;
    state.files[0].groupSettings[0].rotationDegrees = {30, 10, 7};
    state.files[0].groupSettings[0].scale = .75f;
    state.files[0].groupSettings[1].rotationDegrees = {-13, 60, 28};
    state.files[0].groupSettings[1].scale = 2;
    const auto selection = state.selectedSceneObjects;
    const auto camera = state.camera;
    const auto document = woby::createSceneDocument(state);
    const auto points = renderedPoints(state);
    const auto reference = rootFilePivot(state, 1);
    const auto a = rootFilePivot(state, 0), c = rootFilePivot(state, 2);
    const auto storage = state.files[0].mesh.storage;
    const auto beforeRevision = state.sceneEditRevision;
    const auto beforeGeometry = state.revisions.geometry;
    REQUIRE(woby::selectedObjectTransformStatus(state, state.files[1].objectId).alignOriginsIssue == nullptr);
    REQUIRE(woby::transformSelectedObjects(state, state.files[1].objectId, woby::UiSelectionTransform::alignOrigins));
    checkCoordinate(rootFilePivot(state, 0), reference);
    checkCoordinate(rootFilePivot(state, 1), reference);
    checkCoordinate(rootFilePivot(state, 2), reference);
    const auto after = woby::createSceneDocument(state);
    CHECK(after.files[1] == document.files[1]);
    CHECK(after.files[3] == document.files[3]);
    CHECK(after.files[0].settings.rotationDegrees == document.files[0].settings.rotationDegrees);
    CHECK(after.files[0].settings.scale == document.files[0].settings.scale);
    for (size_t g = 0; g < after.files[0].groups.size(); ++g) {
        CHECK(after.files[0].groups[g].settings.rotationDegrees == document.files[0].groups[g].settings.rotationDegrees);
        CHECK(after.files[0].groups[g].settings.scale == document.files[0].groups[g].settings.scale);
    }
    const auto moved = renderedPoints(state);
    checkRigidMove(points, moved, 0, {reference[0] - a[0], reference[1] - a[1], reference[2] - a[2]});
    checkRigidMove(points, moved, 2, {reference[0] - c[0], reference[1] - c[1], reference[2] - c[2]});
    checkRigidMove(points, moved, 1, {});
    checkRigidMove(points, moved, 3, {});
    CHECK(state.selectedSceneObjects == selection);
    CHECK(state.camera == camera);
    CHECK(state.files[0].mesh.storage == storage);
    CHECK(state.isDirty);
    const auto revision = state.sceneEditRevision;
    CHECK(revision == beforeRevision + 1);
    CHECK(state.revisions.geometry == beforeGeometry + 1);
    CHECK(woby::selectedObjectTransformStatus(state, state.files[1].objectId).alignOriginsIssue != nullptr);
    CHECK_FALSE(woby::transformSelectedObjects(state, state.files[1].objectId, woby::UiSelectionTransform::alignOrigins));
    CHECK(state.sceneEditRevision == revision);
}

TEST_CASE("selection transforms align parts in rotated scaled parents and stable large coordinate frames")
{
    TransformFixture f;
    auto& state = f.state;
    state.files[0].mesh.origin = {1e12, -1e12, 1e12};
    state.files[1].mesh.origin = {1e12 + .25, -1e12 + .5, 1e12 - .125};
    state.coordinateOrigin.reset();
    state.files[0].fileSettings.rotationDegrees = {15, 60, 90};
    state.files[0].fileSettings.scale = 2;
    state.files[1].fileSettings.rotationDegrees = {-30, 20, 7};
    state.files[1].fileSettings.scale = .75f;
    woby::UiSceneNode folder;
    folder.kind = woby::UiSceneNodeKind::folder;
    folder.name = "parent";
    folder.settings.translation = {9, -7, 11};
    folder.settings.rotationDegrees = {30, 0, -40};
    folder.settings.scale = 1.25f;
    folder.children = {state.sceneNodes[0], state.sceneNodes[1]};
    state.sceneNodes = {folder, state.sceneNodes[2], state.sceneNodes[3]};
    woby::assignSceneObjectIds(state);
    woby::recalculateSceneBounds(state);
    const auto a = state.files[0].groupSettings[0].objectId, b = state.files[1].groupSettings[0].objectId;
    state.selectedSceneObjects = {a, b};
    const auto reference = partPivot(state, b);
    const auto fileSettings = woby::createSceneDocument(state).files[0].settings;
    REQUIRE(woby::transformSelectedObjects(state, b, woby::UiSelectionTransform::alignOrigins));
    checkCoordinate(partPivot(state, a), reference);
    checkCoordinate(partPivot(state, b), reference);
    CHECK(woby::createSceneDocument(state).files[0].settings == fileSettings);
    CHECK((state.coordinateOrigin == woby::Coordinate{1e12, -1e12, 1e12}));
}

TEST_CASE("selection transforms move folder geometry rigidly through nested child transforms")
{
    TransformFixture f;
    auto& state = f.state;
    woby::UiSceneNode child, parent;
    child.kind = parent.kind = woby::UiSceneNodeKind::folder;
    child.name = "child"; parent.name = "parent";
    child.settings.rotationDegrees = {30, -70, 20}; child.settings.scale = 2;
    child.children = {state.sceneNodes[0]};
    parent.settings.translation = {8, 20, 2}; parent.settings.center = {1, 2, 3};
    parent.settings.rotationDegrees = {-40, 10, 15}; parent.settings.scale = .5f;
    parent.children = {child};
    state.sceneNodes = {parent, state.sceneNodes[1], state.sceneNodes[2], state.sceneNodes[3]};
    woby::assignSceneObjectIds(state);
    state.files[0].fileSettings.rotationDegrees = {10, 20, 30};
    state.files[0].groupSettings[0].rotationDegrees = {80, 15, -20};
    state.files[0].groupSettings[1].scale = .3f;
    const auto points = renderedPoints(state);
    const auto before = woby::createSceneDocument(state);
    const auto& settings = state.sceneNodes[0].settings;
    woby::CoordinateMatrix matrix;
    woby::sceneNodeTransformMatrix(settings, matrix.data());
    const auto pivot = woby::transformCoordinate(matrix.data(), {settings.center[0], settings.center[1], settings.center[2]});
    const auto reference = rootFilePivot(state, 1);
    state.selectedSceneObjects = {state.sceneNodes[0].objectId, state.files[1].objectId};
    REQUIRE(woby::transformSelectedObjects(state, state.files[1].objectId, woby::UiSelectionTransform::alignOrigins));
    checkRigidMove(points, renderedPoints(state), 0, {reference[0] - pivot[0], reference[1] - pivot[1], reference[2] - pivot[2]});
    checkRigidMove(points, renderedPoints(state), 1, {});
    CHECK(woby::createSceneDocument(state).files == before.files);
}

TEST_CASE("selection transforms arrange hidden rotated geometry with gaps in scene tree order")
{
    TransformFixture f;
    auto& state = f.state;
    state.files[0].fileSettings.rotationDegrees = {10, 25, 40};
    state.files[0].groupSettings[1].rotationDegrees = {5, 70, -20};
    state.files[2].fileSettings.scale = 1.5f;
    state.files[2].fileSettings.rotationDegrees = {-40, -30, 15};
    state.files[0].fileSettings.visible = false;
    state.files[2].fileSettings.opacity = 0;
    state.sceneNodes = {state.sceneNodes[2], state.sceneNodes[1], state.sceneNodes[0], state.sceneNodes[3]};
    const auto before = renderedPoints(state);
    const auto document = woby::createSceneDocument(state);
    const auto reference = pointBounds(before[1]);
    double size = 0;
    for (size_t i = 0; i < 3; ++i) {
        const auto bounds = pointBounds(before[i]);
        for (size_t k = 0; k < 3; ++k) { size = std::max(size, bounds[1][k] - bounds[0][k]); }
    }
    REQUIRE(woby::selectedObjectTransformStatus(state, state.files[1].objectId).arrangeNearbyIssue == nullptr);
    REQUIRE(woby::transformSelectedObjects(state, state.files[1].objectId, woby::UiSelectionTransform::arrangeNearby));
    const auto after = renderedPoints(state);
    const auto c = pointBounds(after[2]), a = pointBounds(after[0]);
    CHECK(c[0][0] - reference[1][0] == doctest::Approx(size * .05).epsilon(1e-4));
    CHECK(a[0][0] - c[1][0] == doctest::Approx(size * .05).epsilon(1e-4));
    for (size_t k = 1; k < 3; ++k) {
        CHECK(a[0][k] + a[1][k] == doctest::Approx(reference[0][k] + reference[1][k]).epsilon(2e-5).scale(1));
        CHECK(c[0][k] + c[1][k] == doctest::Approx(reference[0][k] + reference[1][k]).epsilon(2e-5).scale(1));
    }
    checkRigidMove(before, after, 1, {});
    checkRigidMove(before, after, 3, {});
    const auto saved = woby::createSceneDocument(state);
    CHECK(saved.files[1] == document.files[1]);
    CHECK_FALSE(state.files[0].fileSettings.visible);
    CHECK(state.files[2].fileSettings.opacity == 0);
    const auto revision = state.sceneEditRevision;
    CHECK(woby::selectedObjectTransformStatus(state, state.files[1].objectId).arrangeNearbyIssue != nullptr);
    CHECK_FALSE(woby::transformSelectedObjects(state, state.files[1].objectId, woby::UiSelectionTransform::arrangeNearby));
    CHECK(state.sceneEditRevision == revision);
    CHECK(woby::createSceneDocument(state) == saved);
}

TEST_CASE("selection transforms reject unsupported overlapping pending and repeated selections without edits")
{
    TransformFixture f;
    auto& state = f.state;
    const auto reference = state.files[1].objectId;
    SUBCASE("one object") { state.selectedSceneObjects = {reference}; }
    SUBCASE("reference is not selected") { state.selectedSceneObjects = {state.files[0].objectId, state.files[2].objectId}; }
    SUBCASE("stale object") { state.selectedSceneObjects.push_back(state.nextObjectId + 100); }
    SUBCASE("analysis") { const auto id = woby::createComparison(state); state.selectedSceneObjects = {reference, id}; }
    SUBCASE("file and part") { state.selectedSceneObjects.push_back(state.files[0].groupSettings[0].objectId); }
    SUBCASE("folder and descendant") {
        woby::UiSceneNode folder;
        folder.kind = woby::UiSceneNodeKind::folder; folder.children = {state.sceneNodes[0]};
        state.sceneNodes[0] = folder; woby::assignSceneObjectIds(state);
        state.selectedSceneObjects.push_back(state.sceneNodes[0].objectId);
    }
    SUBCASE("repeated selected file") { state.sceneNodes.push_back(state.sceneNodes[0]); }
    SUBCASE("repeated descendant part") { state.sceneNodes.push_back(state.sceneNodes[0].children[0]); }
    SUBCASE("model preparation") { state.files[0].postLoadPhase = woby::PostLoadPhase::preparing; }
    const auto before = woby::createSceneDocument(state);
    const auto revision = state.sceneEditRevision;
    const auto status = woby::selectedObjectTransformStatus(state, reference);
    CHECK(status.alignOriginsIssue != nullptr);
    CHECK(status.arrangeNearbyIssue != nullptr);
    for (const auto action : {woby::UiSelectionTransform::alignOrigins, woby::UiSelectionTransform::arrangeNearby}) {
        CHECK_FALSE(woby::transformSelectedObjects(state, reference, action));
        CHECK(state.sceneEditRevision == revision);
        CHECK(woby::createSceneDocument(state) == before);
    }
}

TEST_CASE("selection transforms reject an unrepresentable later move before applying earlier edits")
{
    TransformFixture f;
    auto& state = f.state;
    state.files[1].mesh.origin = {1e20, 0, 0};
    woby::recalculateSceneBounds(state);
    const auto before = woby::createSceneDocument(state);
    const auto revision = state.sceneEditRevision;
    const auto reference = state.files[2].objectId;
    for (const auto action : {woby::UiSelectionTransform::alignOrigins, woby::UiSelectionTransform::arrangeNearby}) {
        CHECK_FALSE(woby::transformSelectedObjects(state, reference, action));
        CHECK(woby::createSceneDocument(state) == before);
        CHECK(state.sceneEditRevision == revision);
    }
}

TEST_CASE("selection transforms support flat scenes duplicate selection IDs and tiny model units")
{
    TransformFixture f;
    auto& state = f.state;
    for (auto& file : state.files) {
        const auto id = file.objectId;
        const auto groupId = file.groupSettings[0].objectId;
        file = woby::createUiFileState(file.path, transformMesh(1e-9f), 0);
        file.objectId = id; file.groupSettings[0].objectId = groupId;
    }
    state.files[0].fileSettings.translation[0] = -1e-8f;
    state.files[1].fileSettings.translation[0] = 1e-8f;
    state.sceneNodes.clear();
    state.selectedSceneObjects = {state.files[0].objectId, state.files[1].objectId, state.files[0].objectId};
    const auto reference = state.files[1].objectId;
    REQUIRE(woby::transformSelectedObjects(state, reference, woby::UiSelectionTransform::arrangeNearby));
    const auto points = renderedPoints(state);
    const auto a = pointBounds(points[0]), b = pointBounds(points[1]);
    CHECK(a[0][0] - b[1][0] == doctest::Approx(5e-11).epsilon(1e-3));
    CHECK_FALSE(woby::transformSelectedObjects(state, reference, woby::UiSelectionTransform::arrangeNearby));
    REQUIRE(woby::transformSelectedObjects(state, reference, woby::UiSelectionTransform::alignOrigins));
    CHECK(rootFilePivot(state, 0)[0] == doctest::Approx(rootFilePivot(state, 1)[0]).epsilon(1e-6));
}

TEST_CASE("selection transforms do not skip a move when another selected model has a much larger size")
{
    TransformFixture f;
    auto& state = f.state;
    const auto fileId = state.files[0].objectId;
    const auto groupId = state.files[0].groupSettings[0].objectId;
    state.files[0] = woby::createUiFileState(state.files[0].path, transformMesh(1e9f), 0);
    state.files[0].objectId = fileId;
    state.files[0].groupSettings[0].objectId = groupId;
    state.sceneNodes[0] = woby::createFileSceneNode(state.files[0], 0);
    const auto pivot = rootFilePivot(state, 1);
    REQUIRE(woby::transformSelectedObjects(state, state.files[1].objectId, woby::UiSelectionTransform::alignOrigins));
    checkCoordinate(rootFilePivot(state, 0), pivot);
    checkCoordinate(rootFilePivot(state, 2), pivot);
    CHECK_FALSE(woby::transformSelectedObjects(state, state.files[1].objectId, woby::UiSelectionTransform::alignOrigins));
}

TEST_CASE("selection transforms reject nonfinite transforms and empty arrangement geometry")
{
    TransformFixture f;
    auto& state = f.state;
    SUBCASE("nonfinite transform") {
        state.files[0].groupSettings[1].translation[0] = std::numeric_limits<float>::infinity();
        const auto status = woby::selectedObjectTransformStatus(state, state.files[1].objectId);
        CHECK(status.alignOriginsIssue != nullptr);
        CHECK(status.arrangeNearbyIssue != nullptr);
        const auto revision = state.sceneEditRevision;
        CHECK_FALSE(woby::transformSelectedObjects(state, state.files[1].objectId, woby::UiSelectionTransform::alignOrigins));
        CHECK_FALSE(woby::transformSelectedObjects(state, state.files[1].objectId, woby::UiSelectionTransform::arrangeNearby));
        CHECK(state.sceneEditRevision == revision);
        CHECK(state.files[0].fileSettings.translation[0] == 3);
    }
    SUBCASE("empty geometry") {
        state.files[0].groupSettings.clear();
        state.sceneNodes[0].children.clear();
        const auto status = woby::selectedObjectTransformStatus(state, state.files[1].objectId);
        CHECK(status.alignOriginsIssue == nullptr);
        CHECK(status.arrangeNearbyIssue != nullptr);
        const auto revision = state.sceneEditRevision;
        CHECK_FALSE(woby::transformSelectedObjects(state, state.files[1].objectId, woby::UiSelectionTransform::arrangeNearby));
        CHECK(state.sceneEditRevision == revision);
    }
}

TEST_CASE("selection transforms are one undoable edit and persist compensated translations in scene files")
{
    TransformFixture f;
    f.state.files[0].groupSettings[0].rotationDegrees = {17, 33, 65};
    f.state.files[0].groupSettings[1].scale = 2;
    for (const auto action : {woby::UiSelectionTransform::alignOrigins, woby::UiSelectionTransform::arrangeNearby}) {
        auto state = f.state;
        const auto clean = woby::createSceneDocument(state);
        woby::SceneHistory history;
        woby::resetSceneHistory(history, state);
        REQUIRE(woby::transformSelectedObjects(state, state.files[1].objectId, action));
        const auto changed = woby::createSceneDocument(state);
        REQUIRE(woby::recordSceneHistory(history, state));
        CHECK(history.snapshots.size() == 2);
        CHECK(history.cursor == 1);
        auto undo = woby::prepareSceneHistoryStep(history, state, clean, false);
        REQUIRE(undo);
        woby::commitSceneHistoryStep(history, state, std::move(*undo), false);
        CHECK(woby::createSceneDocument(state) == clean);
        CHECK_FALSE(state.isDirty);
        auto redo = woby::prepareSceneHistoryStep(history, state, clean, true);
        REQUIRE(redo);
        woby::commitSceneHistoryStep(history, state, std::move(*redo), true);
        CHECK(woby::createSceneDocument(state) == changed);
        CHECK(state.isDirty);

        const auto scenePath = f.directory.root / "transforms.woby";
        woby::writeSceneDocument(scenePath, changed);
        const auto document = woby::readSceneDocument(scenePath);
        auto restored = f.state;
        for (size_t i = 0; i < restored.files.size(); ++i) { woby::applySceneFileRecord(restored.files[i], document.files[i]); }
        woby::applySceneNodeRecords(restored, document.nodes);
        woby::recalculateSceneBounds(restored);
        CHECK(woby::createSceneDocument(restored) == changed);
        const auto points = renderedPoints(state), reloadedPoints = renderedPoints(restored);
        for (size_t i = 0; i < state.files.size(); ++i) { checkRigidMove(points, reloadedPoints, i, {}); }
    }
}

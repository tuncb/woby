#include "scene_lifecycle.h"
#include "ui_operations.h"
#include "automation_registry.h"
#include "background_load.h"

#include <doctest/doctest.h>
#include <fstream>
#include <future>
#include <limits>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
struct LifecycleFixture {
    std::filesystem::path root = std::filesystem::temp_directory_path()
        / ("woby-lifecycle-" + woby::automationRandomHex(8));
    woby::UiState state;
    std::optional<std::filesystem::path> path;
    woby::SceneDocument clean = woby::createSceneDocument(state);

    LifecycleFixture() { std::filesystem::create_directory(root); }
    ~LifecycleFixture() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
    std::optional<woby::SceneLifecycleError> begin(const woby::SceneLifecycleCommand& command, bool busy = false) {
        return woby::beginSceneLifecycle(command, state, path, clean, busy);
    }
    void dirty() { woby::setShowGrid(state, false); }
};
}

TEST_CASE("lifecycle dirty policies gate every destructive operation")
{
    for (const auto action : {woby::SceneAction::open, woby::SceneAction::newScene, woby::SceneAction::quit}) {
        LifecycleFixture fixture;
        woby::SceneLifecycleCommand command;
        command.action = action;
        CHECK_FALSE(fixture.begin(command));
        fixture.dirty();
        auto error = fixture.begin(command);
        REQUIRE(error);
        CHECK(error->reason == "dirty_scene");
        CHECK(fixture.state.running);
        CHECK(fixture.state.isDirty);
        command.onDirty = woby::DirtyPolicy::save;
        error = fixture.begin(command);
        REQUIRE(error);
        CHECK(error->reason == "save_path_required");
        CHECK(fixture.state.isDirty);
        command.onDirty = woby::DirtyPolicy::discard;
        CHECK_FALSE(fixture.begin(command));
        CHECK(fixture.state.isDirty); // Gate does not discard until commit.
        command.onDirty = woby::DirtyPolicy::save;
        command.savePath = fixture.root / "before";
        CHECK_FALSE(fixture.begin(command));
        CHECK(fixture.path == fixture.root / "before.woby");
        CHECK_FALSE(fixture.state.isDirty);
        CHECK_FALSE(woby::readSceneDocument(*fixture.path).showGrid);
    }
}

TEST_CASE("lifecycle rejects busy work before saving or discarding")
{
    LifecycleFixture fixture;
    fixture.dirty();
    for (const auto action : {woby::SceneAction::save, woby::SceneAction::saveAs,
        woby::SceneAction::open, woby::SceneAction::newScene, woby::SceneAction::quit}) {
        woby::SceneLifecycleCommand command;
        command.action = action;
        command.path = fixture.root / "output.woby";
        command.savePath = command.path;
        command.onDirty = woby::DirtyPolicy::save;
        const auto error = fixture.begin(command, true);
        REQUIRE(error);
        CHECK(error->reason == "scene_busy");
        CHECK_FALSE(std::filesystem::exists(command.path));
        CHECK_FALSE(fixture.path);
        CHECK(fixture.state.isDirty);
    }
}

TEST_CASE("save-as protects existing files and save updates the current destination")
{
    LifecycleFixture fixture;
    fixture.dirty();
    const auto destination = fixture.root / "existing.woby";
    woby::writeSceneDocument(destination, fixture.clean);
    woby::SceneLifecycleCommand command;
    command.action = woby::SceneAction::saveAs;
    command.path = destination;
    auto error = fixture.begin(command);
    REQUIRE(error);
    CHECK(error->reason == "destination_exists");
    CHECK(woby::readSceneDocument(destination) == fixture.clean);
    CHECK_FALSE(fixture.path);
    CHECK(fixture.state.isDirty);
    command.overwrite = true;
    CHECK_FALSE(fixture.begin(command));
    CHECK(fixture.path == destination);
    CHECK_FALSE(fixture.state.isDirty);
    command.overwrite = false;
    error = fixture.begin(command); // save-as to the current file is still explicit.
    REQUIRE(error);
    CHECK(error->reason == "destination_exists");
    woby::setShowOrigin(fixture.state, false);
    command = {};
    CHECK_FALSE(fixture.begin(command));
    CHECK_FALSE(woby::readSceneDocument(destination).showOrigin);
    CHECK_FALSE(fixture.state.isDirty);
}

TEST_CASE("failed save preserves path baseline and dirty scene")
{
    LifecycleFixture fixture;
    const auto originalPath = fixture.root / "original.woby";
    REQUIRE_FALSE(woby::saveSceneState(fixture.state, fixture.path, fixture.clean, originalPath, true));
    const auto baseline = fixture.clean;
    fixture.dirty();
    woby::SceneLifecycleCommand command;
    command.action = woby::SceneAction::open;
    command.onDirty = woby::DirtyPolicy::save;
    command.savePath = fixture.root / "missing-directory" / "scene.woby";
    auto error = fixture.begin(command);
    REQUIRE(error);
    CHECK(error->reason == "save_failed");
    CHECK(fixture.path == originalPath);
    CHECK(fixture.clean == baseline);
    CHECK(fixture.state.isDirty);
    CHECK(woby::readSceneDocument(originalPath) == baseline);

    // Installation failure after the temporary document has been fully written.
    command.savePath = fixture.root / "directory.woby";
    command.overwrite = true;
    std::filesystem::create_directory(command.savePath);
    error = fixture.begin(command);
    REQUIRE(error);
    CHECK(error->reason == "save_failed");
    CHECK(std::filesystem::is_directory(command.savePath));
    CHECK(fixture.path == originalPath);
    CHECK(fixture.clean == baseline);
    CHECK(fixture.state.isDirty);
    for (const auto& entry : std::filesystem::directory_iterator(fixture.root)) {
        CHECK(entry.path().filename().string().find(".woby-save-") == std::string::npos);
    }
}

TEST_CASE("save-as switches subsequent saves to the new file without changing the original")
{
    LifecycleFixture fixture;
    const auto original = fixture.root / "original.woby";
    REQUIRE_FALSE(woby::saveSceneState(fixture.state, fixture.path, fixture.clean, original, false));
    const auto originalDocument = fixture.clean;
    fixture.dirty();

    woby::SceneLifecycleCommand command;
    command.action = woby::SceneAction::saveAs;
    command.path = fixture.root / "copy.woby";
    REQUIRE_FALSE(fixture.begin(command));
    CHECK(fixture.path == command.path);
    CHECK_FALSE(fixture.state.isDirty);
    CHECK(woby::readSceneDocument(original) == originalDocument);
    CHECK_FALSE(woby::readSceneDocument(command.path).showGrid);

    woby::setShowOrigin(fixture.state, false);
    command.action = woby::SceneAction::save;
    command.path.clear();
    REQUIRE_FALSE(fixture.begin(command));
    CHECK_FALSE(woby::readSceneDocument(*fixture.path).showOrigin);
    CHECK(woby::readSceneDocument(original) == originalDocument);
}

TEST_CASE("new scene replacement clears content and editing state but retains pane preferences")
{
    LifecycleFixture fixture;
    woby::UiFileState file;
    file.path = fixture.root / "model.obj";
    fixture.state.files.push_back(file);
    woby::appendDefaultSceneNodesForFiles(fixture.state, 0);
    const auto comparisonId = woby::createComparison(fixture.state);
    woby::selectSceneObject(fixture.state, comparisonId);
    woby::setShowGrid(fixture.state, false);
    woby::setShowOrigin(fixture.state, false);
    woby::setSceneUpAxis(fixture.state, woby::SceneUpAxis::y);
    woby::setMasterVertexPointSize(fixture.state, 12.0f);
    woby::setCameraOrbiting(fixture.state, true);
    woby::setCameraPanning(fixture.state, true);
    woby::setCameraRolling(fixture.state, true);
    woby::setViewerPaneVisible(fixture.state, false);
    const auto allocator = fixture.state.nextObjectId;

    const auto empty = woby::prepareSceneReplacement(fixture.state, {}, {});
    CHECK(empty.files.empty());
    CHECK(empty.sceneNodes.empty());
    CHECK(empty.comparisons.empty());
    CHECK(empty.selectedSceneObjects.empty());
    CHECK(empty.activeComparisonId == woby::invalidSceneObjectId);
    CHECK_FALSE(empty.cameraInput.orbiting);
    CHECK_FALSE(empty.cameraInput.panning);
    CHECK_FALSE(empty.cameraInput.rolling);
    CHECK_FALSE(empty.isDirty);
    CHECK_FALSE(empty.viewerPaneVisible);
    CHECK(empty.propertiesPaneVisible == fixture.state.propertiesPaneVisible);
    CHECK(empty.nextObjectId == allocator);
    CHECK(woby::createSceneDocument(empty) == woby::createSceneDocument(woby::UiState{}));
}

#ifdef _WIN32
TEST_CASE("locked Windows save destination retains its original bytes")
{
    LifecycleFixture fixture;
    const auto destination = fixture.root / "locked.woby";
    REQUIRE_FALSE(woby::saveSceneState(fixture.state, fixture.path, fixture.clean, destination, true));
    const auto baseline = fixture.clean;
    fixture.dirty();
    const HANDLE handle = CreateFileW(destination.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(handle != INVALID_HANDLE_VALUE);
    const auto error = fixture.begin({});
    CloseHandle(handle);
    REQUIRE(error);
    CHECK(error->reason == "save_failed");
    CHECK(woby::readSceneDocument(destination) == baseline);
    CHECK(fixture.clean == baseline);
    CHECK(fixture.state.isDirty);
}
#endif

TEST_CASE("atomic no-overwrite installation permits exactly one concurrent writer")
{
    LifecycleFixture fixture;
    const auto destination = fixture.root / "race.woby";
    auto writer = [&](bool grid) {
        woby::SceneDocument document;
        document.showGrid = grid;
        try {
            woby::writeSceneDocument(destination, document, false);
            return 1;
        } catch (const std::filesystem::filesystem_error& error) {
            return error.code() == std::errc::file_exists ? 0 : -10;
        }
    };
    auto first = std::async(std::launch::async, writer, true);
    auto second = std::async(std::launch::async, writer, false);
    CHECK(first.get() + second.get() == 1);
    CHECK_NOTHROW((void)woby::readSceneDocument(destination));
}

TEST_CASE("save-as model paths resolve relative to the final destination")
{
    LifecycleFixture fixture;
    std::filesystem::create_directory(fixture.root / "scenes");
    woby::SceneDocument document;
    woby::SceneFileRecord file;
    file.path = fixture.root / "model.obj";
    document.files.push_back(file);
    const auto destination = fixture.root / "scenes" / "scene.woby";
    woby::writeSceneDocument(destination, document, false);
    const auto read = woby::readSceneDocument(destination);
    REQUIRE(read.files.size() == 1);
    CHECK(woby::sceneAbsolutePath(destination, read.files[0].path) == file.path);
}

TEST_CASE("replacement assigns fresh IDs while preserving session preferences")
{
    LifecycleFixture fixture;
    woby::UiFileState file;
    file.path = fixture.root / "model.obj";
    fixture.state.files.push_back(file);
    woby::appendDefaultSceneNodesForFiles(fixture.state, 0);
    const auto oldId = fixture.state.files[0].objectId;
    fixture.state.viewerPaneVisible = false;
    fixture.state.viewerPaneWidth = 333.0f;
    const auto document = woby::createSceneDocument(fixture.state);
    auto replacement = woby::prepareSceneReplacement(fixture.state, fixture.state.files, document);
    CHECK_FALSE(woby::findSceneObject(replacement, oldId));
    CHECK(replacement.files[0].objectId > oldId);
    CHECK_FALSE(replacement.viewerPaneVisible);
    CHECK(replacement.viewerPaneWidth == 333.0f);
    CHECK_FALSE(replacement.isDirty);
    const auto freshId = replacement.files[0].objectId;
    const auto allocator = replacement.nextObjectId;
    auto empty = woby::prepareSceneReplacement(replacement, {}, {});
    CHECK(empty.files.empty());
    CHECK(empty.sceneNodes.empty());
    CHECK(empty.nextObjectId == allocator);
    CHECK(empty.showGrid);
    CHECK(empty.showOrigin);
    CHECK_FALSE(empty.isDirty);
    auto reopened = woby::prepareSceneReplacement(empty, fixture.state.files, document);
    CHECK(reopened.files[0].objectId > freshId);
    CHECK_FALSE(woby::findSceneObject(reopened, freshId));
    REQUIRE_FALSE(woby::saveSceneState(reopened, fixture.path, fixture.clean, fixture.root / "ids.woby", false));
    CHECK(reopened.files[0].objectId > freshId);
    const auto savedId = reopened.files[0].objectId;
    REQUIRE_FALSE(woby::saveSceneState(reopened, fixture.path, fixture.clean, fixture.root / "ids-as.woby", false));
    CHECK(reopened.files[0].objectId == savedId);
}

TEST_CASE("failed replacement leaves live scene camera and allocator untouched")
{
    LifecycleFixture fixture;
    fixture.dirty();
    fixture.state.camera.distance = 42.0f;
    fixture.state.nextObjectId = 100;
    const auto original = woby::createSceneDocument(fixture.state);
    woby::SceneDocument invalid;
    woby::SceneNodeRecord node;
    node.kind = woby::SceneNodeKind::file;
    node.fileIndex = 42;
    invalid.nodes.push_back(node);
    CHECK_THROWS((void)woby::prepareSceneReplacement(fixture.state, {}, invalid));
    CHECK(woby::createSceneDocument(fixture.state) == original);
    CHECK(fixture.state.nextObjectId == 100);
    CHECK(fixture.state.camera.distance == 42.0f);
    CHECK(fixture.state.isDirty);
}

TEST_CASE("save before a failed or canceled open stays saved")
{
    LifecycleFixture fixture;
    fixture.dirty();
    woby::SceneLifecycleCommand command;
    command.action = woby::SceneAction::open;
    command.onDirty = woby::DirtyPolicy::save;
    command.savePath = fixture.root / "before.woby";
    REQUIRE_FALSE(fixture.begin(command));
    const auto baseline = fixture.clean;
    const auto path = fixture.path;
    const auto missing = fixture.root / "missing.woby";
    CHECK_THROWS((void)woby::loadSceneCpu(missing, {}, {}));
    std::ofstream(fixture.root / "malformed.woby") << "not a scene";
    CHECK_THROWS((void)woby::loadSceneCpu(fixture.root / "malformed.woby", {}, {}));
    woby::SceneDocument source;
    woby::SceneFileRecord record;
    record.path = fixture.root / "missing.obj";
    source.files.push_back(record);
    woby::writeSceneDocument(missing, source);
    CHECK_THROWS((void)woby::loadSceneCpu(missing, {}, {}));
    CHECK(woby::loadSceneCpu(missing, {}, [] { return true; }).canceled);
    CHECK(fixture.path == path);
    CHECK(fixture.clean == baseline);
    CHECK_FALSE(fixture.state.isDirty);
    CHECK(woby::readSceneDocument(*path) == baseline);
}

TEST_CASE("clean destructive policies never write an explicit save destination")
{
    LifecycleFixture fixture;
    woby::SceneLifecycleCommand command;
    command.action = woby::SceneAction::newScene;
    command.onDirty = woby::DirtyPolicy::save;
    command.savePath = fixture.root / "unused.woby";
    REQUIRE_FALSE(fixture.begin(command));
    CHECK_FALSE(std::filesystem::exists(command.savePath));
    CHECK_FALSE(fixture.path);
}

TEST_CASE("canceling an empty scene load prevents replacement")
{
    LifecycleFixture fixture;
    const auto path = fixture.root / "empty.woby";
    woby::writeSceneDocument(path, {});
    CHECK(woby::loadSceneCpu(path, {}, [] { return true; }).canceled);
}

#include "automation_registry.h"
#include "background_load.h"
#include "file_discovery.h"
#include "obj_mesh.h"
#include "recent_files_storage.h"
#include "scene_file.h"
#include "utf8_path.h"
#include "ui_operations.h"

#include <doctest/doctest.h>
#include <fstream>
#include <string>
#include <system_error>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace {

bool longPathsEnabled()
{
    DWORD enabled = 0;
    DWORD bytes = sizeof(enabled);
    const auto status = RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\FileSystem",
        L"LongPathsEnabled", RRF_RT_REG_DWORD, nullptr, &enabled, &bytes);
    return status == ERROR_SUCCESS && enabled == 1;
}

struct PathDirectory {
    std::filesystem::path root = std::filesystem::absolute(std::filesystem::temp_directory_path())
        / ("woby-long-paths-" + woby::automationRandomHex(8));
    PathDirectory() { std::filesystem::create_directory(root); }
    ~PathDirectory() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
};

std::filesystem::path deepDirectory(const std::filesystem::path& root, size_t minimum)
{
    auto path = root;
    const auto component = woby::pathFromUtf8("caf\xc3\xa9 \xe6\xa8\xa1\xe5\x9e\x8b-" + std::string(64, 'x'));
    while (path.native().size() < minimum) { path /= component; }
    std::filesystem::create_directories(path);
    return path;
}

void writeObj(const std::filesystem::path& path, size_t padding = 0)
{
    std::ofstream output;
    output.exceptions(std::ios::badbit | std::ios::failbit);
    output.open(path, std::ios::binary);
    const auto comment = "#" + std::string(62, 'x') + "\n";
    for (size_t bytes = 0; bytes < padding; bytes += comment.size()) { output << comment; }
    output << "o mesh\nv 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
}

} // namespace

TEST_CASE("Windows long paths load small and large OBJ files and discover folders")
{
    if (!longPathsEnabled()) { MESSAGE("Windows long paths are disabled; skip filesystem checks."); return; }
    const PathDirectory fixture;
    size_t minimum = 300;
    SUBCASE("above MAX_PATH") {}
    SUBCASE("above 1200 characters") { minimum = 1200; }
    const auto folder = deepDirectory(fixture.root, minimum);
    const auto smallFile = folder / "small.obj";
    const auto largeFile = folder / "large.obj";
    writeObj(smallFile);
    writeObj(largeFile, 2 * 1024 * 1024);
    REQUIRE(smallFile.native().size() > minimum);
    for (const auto& path : {smallFile, largeFile}) {
        const auto mesh = woby::loadObjMesh(path);
        CHECK(woby::meshGeometry(mesh).vertices.size() == 3);
        CHECK(woby::meshGeometry(mesh).indices.size() == 3);
    }
    const std::vector<std::filesystem::path> expected{largeFile, smallFile};
    CHECK(woby::collectModelPathsRecursive(folder) == expected);
    CHECK(woby::collectModelPathsRecursive(fixture.root) == expected);
}

TEST_CASE("Windows Unicode model and folder names remain UTF-8 through loading and scene nodes")
{
    const PathDirectory fixture;
    const std::string rootName = "caf\xc3\xa9 \xe6\xa8\xa1\xe5\x9e\x8b";
    const std::string childName = "parts \xe6\xa8\xa1\xe5\x9e\x8b";
    const std::string filename = "part \xe6\xa8\xa1\xe5\x9e\x8b.obj";
    const auto root = fixture.root / woby::pathFromUtf8(rootName);
    const auto child = root / woby::pathFromUtf8(childName);
    std::filesystem::create_directories(child);
    const auto model = child / woby::pathFromUtf8(filename);
    writeObj(model);
    auto loaded = woby::loadModelBatchCpu({model}, 0, {}, {}, true);
    CHECK(loaded.failedCount == 0);
    REQUIRE(loaded.files.size() == 1);
    woby::UiState state;
    state.files = std::move(loaded.files);
    woby::appendFolderTreeSceneNode(state, root, 0, 1);
    REQUIRE(state.sceneNodes.size() == 1);
    const auto& node = state.sceneNodes[0];
    CHECK(node.name == rootName);
    REQUIRE(node.children.size() == 1);
    CHECK(node.children[0].name == childName);
    REQUIRE(node.children[0].children.size() == 1);
    CHECK(node.children[0].children[0].name == filename);
    const auto scene = root / woby::pathFromUtf8("review \xe6\xa8\xa1\xe5\x9e\x8b.woby");
    woby::writeSceneDocument(scene, woby::createSceneDocument(state));
    const auto restored = woby::loadSceneCpu(scene, {}, {}, true);
    REQUIRE(restored.files.size() == 1);
    CHECK(restored.files[0].path == model);
}

TEST_CASE("Windows long paths save scenes and resolve long relative model paths")
{
    if (!longPathsEnabled()) { MESSAGE("Windows long paths are disabled; skip filesystem checks."); return; }
    const PathDirectory fixture;
    const auto folder = deepDirectory(fixture.root, 1200);
    const auto model = folder / "small.obj";
    writeObj(model);
    const auto scene = fixture.root / "review.woby";
    woby::SceneDocument document;
    woby::SceneFileRecord file;
    file.path = model;
    document.files.push_back(file);
    woby::writeSceneDocument(scene, document);
    const auto restored = woby::readSceneDocument(scene);
    REQUIRE(restored.files.size() == 1);
    CHECK(restored.files[0].path.is_relative());
    CHECK(restored.files[0].path.native().size() > 1000);
    CHECK(woby::sceneAbsolutePath(scene, restored.files[0].path) == model);

    const auto deepScene = folder / "review.woby";
    woby::writeSceneDocument(deepScene, document, false);
    const auto deepRestored = woby::readSceneDocument(deepScene);
    REQUIRE(deepRestored.files.size() == 1);
    CHECK(woby::sceneAbsolutePath(deepScene, deepRestored.files[0].path) == model);
    document.showGrid = false;
    woby::writeSceneDocument(deepScene, document);
    CHECK_FALSE(woby::readSceneDocument(deepScene).showGrid);
    for (const auto& entry : std::filesystem::directory_iterator(folder)) {
        CHECK(entry.path().filename().native().find(L".woby-save-") != 0);
    }
}

TEST_CASE("Windows long paths persist recent files in long storage paths")
{
    if (!longPathsEnabled()) { MESSAGE("Windows long paths are disabled; skip filesystem checks."); return; }
    const PathDirectory fixture;
    const auto folder = deepDirectory(fixture.root, 1200);
    const auto model = folder / "small.obj";
    writeObj(model);
    const auto storage = folder / "state" / "recents.json";
    const woby::RecentFileChange change{woby::RecentFileAction::remember, {woby::RecentFileKind::model, model, 1234}};
    const auto saved = woby::updateRecentFiles(storage, {change});
    CHECK(saved.warning.empty());
    const auto restored = woby::readRecentFiles(storage);
    CHECK(restored.warning.empty());
    REQUIRE(restored.files.entries.size() == 1);
    CHECK(restored.files.entries[0].path == model);
}

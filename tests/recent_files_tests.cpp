#include "recent_files.h"
#include "recent_files_runtime.h"
#include "automation_registry.h"
#include "utf8_path.h"
#include "main_menu.h"
#include "annotation_ui.h"
#include "ui_operations.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>
#include <imgui_internal.h>
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
using namespace woby;
struct RecentFixture {
    std::filesystem::path root = std::filesystem::temp_directory_path() / ("woby-recents-" + automationRandomHex(8));
    std::filesystem::path storage = root / "state" / "recents.json";
    RecentFixture() { std::filesystem::create_directory(root); }
    ~RecentFixture() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
    RecentFileChange remember(const char* name, RecentFileKind kind = RecentFileKind::model, uint64_t time = 1) const {
        return {RecentFileAction::remember, {kind, root / name, time}};
    }
    void writeJson(const nlohmann::json& data) const {
        std::filesystem::create_directory(storage.parent_path());
        std::ofstream(storage) << data.dump();
    }
    void checkNoTemporaryFiles() const {
        for (const auto& item : std::filesystem::directory_iterator(storage.parent_path())) {
            CHECK((item.path().filename() == "recents.json" || item.path().filename() == "recents.json.lock"));
        }
    }
};

struct RecentMenuFixture {
    RecentFixture paths;
    ImGuiContext* previous = ImGui::GetCurrentContext();
    ImGuiContext* context = ImGui::CreateContext();
    UiState state;
    AnnotationInteraction annotation;
    RecentFiles files;
    MainMenuAvailability availability;
    MainMenuResult result;

    RecentMenuFixture() {
        ImGui::SetCurrentContext(context);
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1200, 800);
        io.DeltaTime = 1.0f / 60.0f;
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    }
    ~RecentMenuFixture() {
        ImGui::EndFrame();
        ImGui::DestroyContext(context);
        ImGui::SetCurrentContext(previous);
    }
    void frame() {
        ImGui::NewFrame();
        result = drawMainMenu(state, annotation, availability, files);
        ImGui::EndFrame();
    }
    ImGuiWindow* popup() const {
        ImGuiWindow* found = nullptr;
        for (auto* window : context->Windows) {
            if (window->Active && (window->Flags & ImGuiWindowFlags_ChildMenu)) { found = window; }
        }
        return found;
    }
    void openFileMenu() {
        frame(); frame();
        auto* bar = ImGui::FindWindowByName("##MainMenuBar");
        REQUIRE(bar != nullptr);
        ImGui::ActivateItemByID(ImHashStr("File", 0, bar->GetID("##MenuBar")));
        frame(); frame();
        REQUIRE(popup() != nullptr);
    }
};
} // namespace

TEST_CASE("recent files normalize paths and retain the latest twenty of each kind")
{
    RecentFixture f;
    RecentFiles files;
    for (uint64_t i = 1; i <= 25; ++i) {
        const auto stem = std::to_string(i);
        applyRecentFileChange(files, {RecentFileAction::remember, {RecentFileKind::scene, f.root / (stem + ".woby"), i}});
        applyRecentFileChange(files, {RecentFileAction::remember, {RecentFileKind::model, f.root / (stem + ".obj"), i}});
    }
    CHECK(recentFileCount(files, RecentFileKind::scene) == recentFileLimit);
    CHECK(recentFileCount(files, RecentFileKind::model) == recentFileLimit);
    CHECK(files.entries.size() == 40);
    CHECK(files.entries.front().path == f.root / "25.obj");
    applyRecentFileChange(files, {RecentFileAction::remember,
        {RecentFileKind::model, f.root / "unused" / ".." / "12.obj", 100}});
    CHECK(files.entries.front().path == f.root / "12.obj");
    CHECK(recentFileCount(files, RecentFileKind::model) == recentFileLimit);
    const auto before = files;
    CHECK_THROWS_AS(applyRecentFileChange(files,
        (RecentFileChange{RecentFileAction::remember, {RecentFileKind::model, "relative.obj", 1}})), std::invalid_argument);
    CHECK(files == before);
}

TEST_CASE("recent files remove entries and clear history without checking file availability")
{
    RecentFixture f;
    RecentFiles files;
    const auto scene = f.remember("offline.woby", RecentFileKind::scene);
    const auto model = f.remember("offline.obj");
    applyRecentFileChange(files, scene);
    applyRecentFileChange(files, model);
    CHECK_FALSE(std::filesystem::exists(scene.entry.path));
    CHECK(files.entries.size() == 2);
    applyRecentFileChange(files, {RecentFileAction::remove, scene.entry});
    REQUIRE(files.entries.size() == 1);
    CHECK(files.entries[0] == model.entry);
    applyRecentFileChange(files, {RecentFileAction::clear, {}});
    CHECK(files.entries.empty());
}

TEST_CASE("recent files load boundaries reject invalid paths and keep the newest duplicate")
{
    RecentFixture f;
    RecentFiles files{{
        {RecentFileKind::model, f.root / "part.obj", 1},
        {RecentFileKind::scene, "relative.woby", 100},
        {RecentFileKind::model, f.root / "folder" / ".." / "part.obj", 5},
        {RecentFileKind::model, {}, 100},
        {static_cast<RecentFileKind>(99), f.root / "bad.obj", 100},
        {RecentFileKind::model, pathFromUtf8(pathToUtf8(f.root) + std::string("/bad\0.obj", 9)), 100}}};
    normalizeRecentFiles(files);
    REQUIRE(files.entries.size() == 1);
    CHECK(files.entries[0].path == f.root / "part.obj");
    CHECK(files.entries[0].lastUsedUnixMilliseconds == 5);
}

TEST_CASE("recent files storage round trips Unicode paths and supports repeated replacement")
{
    RecentFixture f;
    CHECK(readRecentFiles(f.storage).files.entries.empty());
    CHECK_FALSE(std::filesystem::exists(f.storage.parent_path()));
    const auto unicodePath = f.root / pathFromUtf8("m\xc3\xb6" "del \xe6\xa8\xa1\xe5\x9e\x8b.obj");
    const RecentFileChange unicode{RecentFileAction::remember, {RecentFileKind::model, unicodePath, 1234}};
    const auto first = updateRecentFiles(f.storage, {unicode});
    CHECK(first.warning.empty());
    CHECK(first.files == readRecentFiles(f.storage).files);
    const auto second = updateRecentFiles(f.storage, {f.remember("review.woby", RecentFileKind::scene, 1235)});
    CHECK(second.files.entries.size() == 2);
    CHECK(second.files == readRecentFiles(f.storage).files);
    CHECK(second.files.entries[1].path == unicodePath);
#ifndef _WIN32
    CHECK(std::filesystem::status(f.storage).permissions()
        == (std::filesystem::perms::owner_read | std::filesystem::perms::owner_write));
#endif
    f.checkNoTemporaryFiles();
    CHECK(updateRecentFiles(f.storage, {{RecentFileAction::clear, {}}}).files.entries.empty());
    CHECK(readRecentFiles(f.storage).files.entries.empty());
    f.checkNoTemporaryFiles();
}

TEST_CASE("recent files recover corrupt JSON and skip invalid entries")
{
    RecentFixture f;
    std::filesystem::create_directory(f.storage.parent_path());
    std::ofstream(f.storage) << "{broken json";
    const auto broken = readRecentFiles(f.storage);
    CHECK(broken.files.entries.empty());
    CHECK_FALSE(broken.warning.empty());
    const auto recovered = updateRecentFiles(f.storage, {f.remember("valid.obj")});
    CHECK_FALSE(recovered.warning.empty());
    CHECK(readRecentFiles(f.storage).warning.empty());
    const auto valid = nlohmann::json{{"kind", "model"}, {"path", pathToUtf8(f.root / "valid.obj")}, {"last_used_unix_ms", 10}};
    f.writeJson({{"version", 1}, {"entries", {valid,
        {{"kind", "model"}, {"path", "relative.obj"}, {"last_used_unix_ms", 10}},
        {{"kind", "unknown"}, {"path", pathToUtf8(f.root / "bad.obj")}, {"last_used_unix_ms", 10}},
        {{"kind", "model"}, {"path", pathToUtf8(f.root / "bad.obj")}, {"last_used_unix_ms", -1}},
        {{"kind", "model"}, {"path", 42}, {"last_used_unix_ms", 10}}, nlohmann::json::object()}}});
    const auto loaded = readRecentFiles(f.storage);
    REQUIRE(loaded.files.entries.size() == 1);
    CHECK(loaded.files.entries[0].path == f.root / "valid.obj");
    CHECK_FALSE(loaded.warning.empty());
}

TEST_CASE("recent files protect unsupported versions and reject relative storage paths")
{
    RecentFixture f;
    for (const auto& document : {nlohmann::json{{"version", 2}, {"entries", nlohmann::json::array()}},
            nlohmann::json{{"version", 2}, {"new_schema", true}}}) {
        f.writeJson(document);
        CHECK_THROWS((void)updateRecentFiles(f.storage, {f.remember("part.obj")}));
        std::ifstream input(f.storage);
        CHECK(nlohmann::json::parse(input) == document);
    }
    CHECK_THROWS_AS((void)readRecentFiles("relative.json"), std::invalid_argument);
    CHECK_THROWS_AS((void)updateRecentFiles("relative.json", {}), std::invalid_argument);
    f.checkNoTemporaryFiles();
}

TEST_CASE("recent files state directory honors absolute XDG paths and ignores relative values")
{
    RecentFixture f;
    const auto home = pathToUtf8(f.root / "home");
    const auto state = pathToUtf8(f.root / "state");
    CHECK(linuxRecentFilesStoragePath(state, home) == f.root / "state" / "woby" / "recents.json");
    CHECK(linuxRecentFilesStoragePath({}, home) == f.root / "home" / ".local" / "state" / "woby" / "recents.json");
    CHECK(linuxRecentFilesStoragePath("relative-state", home) == linuxRecentFilesStoragePath({}, home));
    CHECK_THROWS((void)linuxRecentFilesStoragePath("relative-state", "relative-home"));
}

TEST_CASE("recent files writers merge concurrent updates under the history lock")
{
    RecentFixture f;
    std::promise<void> gate;
    const auto start = gate.get_future().share();
    std::vector<std::future<RecentFilesStorageResult>> writers;
    for (uint64_t i = 0; i < 8; ++i) {
        const RecentFileChange change{RecentFileAction::remember, {RecentFileKind::model, f.root / (std::to_string(i) + ".obj"), i}};
        writers.push_back(std::async(std::launch::async, [path = f.storage, change, start] {
            start.wait();
            return updateRecentFiles(path, {change});
        }));
    }
    gate.set_value();
    for (auto& writer : writers) { CHECK_NOTHROW(writer.get()); }
    const auto loaded = readRecentFiles(f.storage);
    REQUIRE(loaded.files.entries.size() == 8);
    for (size_t i = 0; i < 8; ++i) { CHECK(loaded.files.entries[i].lastUsedUnixMilliseconds == 7 - i); }
    f.checkNoTemporaryFiles();
}

TEST_CASE("recent files storage merges filesystem aliases according to filesystem identity")
{
    RecentFixture f;
    const auto source = f.root / "source.obj";
    const auto alias = f.root / "alias.obj";
    std::ofstream(source) << "model";
    std::filesystem::create_hard_link(source, alias);
    (void)updateRecentFiles(f.storage, {{RecentFileAction::remember, {RecentFileKind::model, source, 1}}});
    const auto result = updateRecentFiles(f.storage, {{RecentFileAction::remember, {RecentFileKind::model, alias, 2}}});
    REQUIRE(result.files.entries.size() == 1);
    CHECK(result.files.entries[0].path == alias);
    const auto delayed = updateRecentFiles(f.storage, {{RecentFileAction::remember, {RecentFileKind::model, source, 1}}});
    CHECK(delayed.files == result.files);
    CHECK(updateRecentFiles(f.storage, {{RecentFileAction::remove, {RecentFileKind::model, source, 0}}}).files.entries.empty());
}

TEST_CASE("recent files delayed writers cannot move a newer file use back in history")
{
    RecentFixture f;
    RecentFiles files;
    applyRecentFileChange(files, f.remember("part.obj", RecentFileKind::model, 30));
    applyRecentFileChange(files, f.remember("other.obj", RecentFileKind::model, 20));
    const auto before = files;
    applyRecentFileChange(files, f.remember("part.obj", RecentFileKind::model, 10));
    CHECK(files == before);
    (void)updateRecentFiles(f.storage, {f.remember("part.obj", RecentFileKind::model, 30)});
    (void)updateRecentFiles(f.storage, {f.remember("other.obj", RecentFileKind::model, 20)});
    const auto delayed = updateRecentFiles(f.storage, {f.remember("part.obj", RecentFileKind::model, 10)});
    CHECK(delayed.files == before);
}

TEST_CASE("recent files runtime does not publish stale startup or write results")
{
    RecentFixture f;
    (void)updateRecentFiles(f.storage, {f.remember("old.obj")});
    RecentFilesRuntime runtime;
    startRecentFiles(runtime, f.storage);
    runtime.work.wait();
    rememberRecentFile(runtime, RecentFileKind::model, f.root / "new.obj");
    REQUIRE(runtime.files.entries.size() == 1);
    CHECK(pollRecentFiles(runtime).empty());
    CHECK(runtime.files.entries[0].path == f.root / "new.obj");
    finishRecentFiles(runtime);
    REQUIRE(runtime.files.entries.size() == 2);
    CHECK(runtime.files == readRecentFiles(f.storage).files);
    rememberRecentFile(runtime, RecentFileKind::scene, f.root / "review.woby");
    CHECK(pollRecentFiles(runtime).empty());
    runtime.work.wait();
    clearRecentFiles(runtime);
    CHECK(pollRecentFiles(runtime).empty());
    CHECK(runtime.files.entries.empty());
    finishRecentFiles(runtime);
    CHECK(runtime.files.entries.empty());
    CHECK(readRecentFiles(f.storage).files.entries.empty());
}

TEST_CASE("recent files runtime keeps queued changes in order and drains them on shutdown")
{
    RecentFixture f;
    RecentFilesRuntime runtime;
    startRecentFiles(runtime, f.storage);
    rememberRecentFile(runtime, RecentFileKind::scene, f.root / "review.woby");
    rememberRecentFile(runtime, RecentFileKind::model, f.root / "part.obj");
    removeRecentFile(runtime, RecentFileKind::model, f.root / "part.obj");
    finishRecentFiles(runtime);
    REQUIRE(runtime.files.entries.size() == 1);
    CHECK(runtime.files.entries[0].path == f.root / "review.woby");
    CHECK(runtime.files == readRecentFiles(f.storage).files);
    CHECK_FALSE(runtime.sessionOnly);
}

TEST_CASE("recent files runtime retains session history when storage fails")
{
    RecentFixture f;
    std::ofstream(f.root / "blocked") << "not a directory";
    RecentFilesRuntime runtime;
    startRecentFiles(runtime, f.root / "blocked" / "recents.json");
    rememberRecentFile(runtime, RecentFileKind::model, f.root / "part.obj");
    finishRecentFiles(runtime);
    CHECK(runtime.sessionOnly);
    REQUIRE(runtime.files.entries.size() == 1);
    CHECK(runtime.files.entries[0].path == f.root / "part.obj");
    CHECK_FALSE(pollRecentFiles(runtime).empty());
    CHECK(pollRecentFiles(runtime).empty());
    rememberRecentFile(runtime, RecentFileKind::scene, f.root / "review.woby");
    clearRecentFiles(runtime);
    CHECK(runtime.files.entries.empty());
    CHECK_FALSE(std::filesystem::exists(f.storage));
}

TEST_CASE("recent files runtime is inactive until a desktop session enables it")
{
    RecentFixture f;
    RecentFilesRuntime runtime;
    rememberRecentFile(runtime, RecentFileKind::scene, f.root / "headless.woby");
    clearRecentFiles(runtime);
    finishRecentFiles(runtime);
    CHECK(pollRecentFiles(runtime).empty());
    CHECK(runtime.files.entries.empty());
    CHECK_FALSE(runtime.work.valid());
    CHECK_FALSE(std::filesystem::exists(f.storage));
}

TEST_CASE("recent files menu selects the correct path for duplicate filenames without changing the scene")
{
    for (const auto kind : {RecentFileKind::scene, RecentFileKind::model}) {
        RecentMenuFixture f;
        const auto filename = kind == RecentFileKind::scene ? "review.woby" : "part.obj";
        const auto first = f.paths.root / "first" / filename;
        const auto second = f.paths.root / "second" / filename;
        applyRecentFileChange(f.files, {RecentFileAction::remember, {kind, first, 1}});
        applyRecentFileChange(f.files, {RecentFileAction::remember, {kind, second, 2}});
        const auto before = createSceneDocument(f.state);
        const auto history = f.files;
        f.openFileMenu();
        ImGui::ActivateItemByID(f.popup()->GetID(kind == RecentFileKind::scene ? "Recent scenes" : "Recent models"));
        f.frame(); f.frame();
        auto* popup = f.popup();
        REQUIRE(popup != nullptr);
        const auto firstId = ImHashStr(filename, 0, popup->GetID(pathToUtf8(first).c_str()));
        const auto secondId = ImHashStr(filename, 0, popup->GetID(pathToUtf8(second).c_str()));
        CHECK(firstId != secondId);
        ImGui::ActivateItemByID(secondId);
        f.frame();
        CHECK(f.result.command == (kind == RecentFileKind::scene ? MainMenuCommand::openRecentScene : MainMenuCommand::addRecentModel));
        CHECK(f.result.recentPath == second);
        CHECK(f.files == history);
        CHECK(createSceneDocument(f.state) == before);
        CHECK_FALSE(f.state.isDirty);
    }
}

TEST_CASE("recent files clear menu action respects empty history and busy file operations")
{
    for (const bool empty : {false, true}) {
        for (const bool busy : {false, true}) {
            RecentMenuFixture f;
            if (!empty) { applyRecentFileChange(f.files, f.paths.remember("part.obj")); }
            f.availability.fileActionsDisabled = busy;
            f.openFileMenu();
            ImGui::ActivateItemByID(f.popup()->GetID("Clear recent history"));
            f.frame();
            CHECK(f.result.command == (empty || busy ? MainMenuCommand::none : MainMenuCommand::clearRecentFiles));
        }
    }
}

TEST_CASE("recent files submenus are unavailable while file operations are busy")
{
    for (const auto kind : {RecentFileKind::scene, RecentFileKind::model}) {
        RecentMenuFixture f;
        applyRecentFileChange(f.files, f.paths.remember("file", kind));
        f.availability.fileActionsDisabled = true;
        f.openFileMenu();
        auto* parent = f.popup();
        ImGui::ActivateItemByID(parent->GetID(kind == RecentFileKind::scene ? "Recent scenes" : "Recent models"));
        f.frame(); f.frame();
        CHECK(f.popup() == parent);
        CHECK(f.result.command == MainMenuCommand::none);
    }
}

#ifdef _WIN32
TEST_CASE("recent files replacement failure keeps prior history and removes sibling temporary files")
{
    RecentFixture f;
    const auto before = updateRecentFiles(f.storage, {f.remember("old.obj")}).files;
    struct OpenHistory {
        HANDLE handle = INVALID_HANDLE_VALUE;
        ~OpenHistory() { if (handle != INVALID_HANDLE_VALUE) { CloseHandle(handle); } }
    } open;
    open.handle = CreateFileW(f.storage.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(open.handle != INVALID_HANDLE_VALUE);
    CHECK_THROWS((void)updateRecentFiles(f.storage, {f.remember("new.obj", RecentFileKind::model, 2)}));
    CHECK(readRecentFiles(f.storage).files == before);
    f.checkNoTemporaryFiles();
}
#endif

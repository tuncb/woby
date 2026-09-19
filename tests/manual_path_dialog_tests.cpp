#include "manual_path_dialog.h"
#include "native_dialogs.h"
#include "automation_registry.h"
#include "scene_file.h"
#include "utf8_path.h"

#include <doctest/doctest.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <SDL3/SDL.h>

#include <fstream>

namespace {

struct PathFixtures {
    std::filesystem::path root = std::filesystem::absolute(std::filesystem::temp_directory_path())
        / ("woby-manual-paths-" + woby::automationRandomHex(8));
    PathFixtures() { std::filesystem::create_directories(root); }
    ~PathFixtures() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
};

void createFile(const std::filesystem::path& path) { std::ofstream(path) << "fixture"; }

struct DialogUi {
    ImGuiContext* previous = ImGui::GetCurrentContext();
    ImGuiContext* context = ImGui::CreateContext();
    DialogUi()
    {
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1000, 800);
        io.DeltaTime = 1.0f / 60.0f;
        unsigned char* pixels = nullptr;
        int width = 0;
        int height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    }
    ~DialogUi() { ImGui::DestroyContext(context); ImGui::SetCurrentContext(previous); }
};

} // namespace

TEST_CASE("manual paths accept quoted UTF-8 filenames and multiple models without splitting spaces")
{
    PathFixtures fixture;
    const auto first = fixture.root / woby::pathFromUtf8("caf\xc3\xa9 model.obj");
    const auto second = fixture.root / "second model.stl";
    createFile(first);
    createFile(second);
    woby::ManualPathDialog dialog;
    woby::requestManualPathDialog(dialog, "No native chooser");
    dialog.input = " \t\"" + woby::pathToUtf8(first) + "\"\r\n\n'" + woby::pathToUtf8(second) + "'\n";
    const auto paths = woby::submitManualPathDialog(dialog);
    REQUIRE(paths);
    CHECK(*paths == std::vector<std::filesystem::path>{first, second});
    CHECK_FALSE(dialog.active);
}

TEST_CASE("manual paths reject invalid selections atomically and keep the dialog open")
{
    PathFixtures fixture;
    const auto file = fixture.root / "model.obj";
    createFile(file);
    woby::ManualPathDialog dialog;
    woby::requestManualPathDialog(dialog, "No native chooser");
    SUBCASE("blank") { dialog.input = " \r\n\t"; }
    SUBCASE("relative") { dialog.input = "model.obj"; }
    SUBCASE("missing file after valid file") { dialog.input = woby::pathToUtf8(file) + "\n" + woby::pathToUtf8(fixture.root / "missing.obj"); }
    SUBCASE("folder instead of file") { dialog.input = woby::pathToUtf8(fixture.root); }
    SUBCASE("file instead of folder") { dialog.kind = woby::ManualPathKind::folder; dialog.input = woby::pathToUtf8(file); }
    SUBCASE("multiple folders") { dialog.kind = woby::ManualPathKind::folder; dialog.input = woby::pathToUtf8(fixture.root) + "\n" + woby::pathToUtf8(fixture.root); }
    SUBCASE("multiple scenes") { dialog.kind = woby::ManualPathKind::openScene; dialog.input = woby::pathToUtf8(file) + "\n" + woby::pathToUtf8(file); }
    SUBCASE("null byte") { dialog.input = woby::pathToUtf8(file) + std::string(1, '\0'); }
    CHECK_FALSE(woby::submitManualPathDialog(dialog));
    CHECK(dialog.active);
    CHECK_FALSE(dialog.error.empty());
    CHECK_FALSE(dialog.overwritePath);
}

TEST_CASE("manual save paths normalize extensions and confirm the actual destination before overwriting")
{
    PathFixtures fixture;
    woby::ManualPathDialog dialog;
    dialog.kind = woby::ManualPathKind::saveScene;
    auto input = fixture.root / "scene.other";
    auto output = woby::sceneSavePathWithExtension(input);
    SUBCASE("scene") {}
    SUBCASE("screenshot") {
        dialog.kind = woby::ManualPathKind::saveScreenshot;
        input = fixture.root / "image.other";
        output = fixture.root / "image.png";
    }
    SUBCASE("existing uppercase extension") {
        input = fixture.root / "scene.WOBY";
        output = input;
    }
    createFile(output);
    woby::requestManualPathDialog(dialog, "No native chooser");
    dialog.input = woby::pathToUtf8(input);
    CHECK_FALSE(woby::submitManualPathDialog(dialog));
    CHECK(dialog.active);
    REQUIRE(dialog.overwritePath);
    CHECK(*dialog.overwritePath == output);
    const auto paths = woby::submitManualPathDialog(dialog);
    REQUIRE(paths);
    CHECK(*paths == std::vector<std::filesystem::path>{output});
    CHECK_FALSE(dialog.active);
}

TEST_CASE("manual save paths validate parents and bind overwrite confirmation to the destination")
{
    PathFixtures fixture;
    woby::ManualPathDialog dialog;
    dialog.kind = woby::ManualPathKind::saveScene;
    woby::requestManualPathDialog(dialog, "No native chooser");
    SUBCASE("new file in an existing parent") {
        dialog.input = woby::pathToUtf8(fixture.root / "new");
        const auto paths = woby::submitManualPathDialog(dialog);
        REQUIRE(paths);
        CHECK(paths->front() == fixture.root / "new.woby");
        CHECK_FALSE(std::filesystem::exists(paths->front()));
    }
    SUBCASE("nonexistent parent") {
        dialog.input = woby::pathToUtf8(fixture.root / "missing" / "scene");
        CHECK_FALSE(woby::submitManualPathDialog(dialog));
        CHECK_FALSE(dialog.error.empty());
    }
    SUBCASE("directory is not a filename") {
        dialog.input = woby::pathToUtf8(fixture.root);
        CHECK_FALSE(woby::submitManualPathDialog(dialog));
        CHECK_FALSE(dialog.error.empty());
    }
    SUBCASE("changed destination needs fresh confirmation") {
        createFile(fixture.root / "first.woby");
        createFile(fixture.root / "second.woby");
        dialog.input = woby::pathToUtf8(fixture.root / "first");
        CHECK_FALSE(woby::submitManualPathDialog(dialog));
        dialog.input = woby::pathToUtf8(fixture.root / "second");
        CHECK_FALSE(woby::submitManualPathDialog(dialog));
        CHECK(dialog.active);
        CHECK(dialog.overwritePath == fixture.root / "second.woby");
    }
}

TEST_CASE("manual dialog routes accepted selections to the existing file workflows")
{
    PathFixtures fixture;
    const auto file = fixture.root / "scene.woby";
    createFile(file);
    for (const auto kind : {woby::ManualPathKind::models, woby::ManualPathKind::folder,
            woby::ManualPathKind::openScene, woby::ManualPathKind::saveScene, woby::ManualPathKind::saveScreenshot}) {
        DialogUi ui;
        woby::ModelFileDialogState models;
        woby::SceneFileDialogState scene;
        woby::SceneScreenshotDialogState screenshot;
        const bool model = kind == woby::ManualPathKind::models || kind == woby::ManualPathKind::folder;
        const bool png = kind == woby::ManualPathKind::saveScreenshot;
        const bool saving = kind == woby::ManualPathKind::saveScene || png;
        auto& dialog = model ? models.fallback : png ? screenshot.fallback : scene.fallback;
        dialog.kind = kind;
        dialog.anchorX = 100;
        dialog.anchorY = 120;
        woby::requestManualPathDialog(dialog, "No native chooser");
        const auto input = kind == woby::ManualPathKind::folder ? fixture.root : saving ? fixture.root / "new" : file;
        dialog.input = woby::pathToUtf8(input);
        CHECK((model ? woby::modelFileDialogIsOpen(models) : png ? woby::sceneScreenshotDialogIsOpen(screenshot) : woby::sceneFileDialogIsOpen(scene)));
        const auto frame = [&]() {
            ImGui::NewFrame();
            woby::drawNativeDialogFallbacks(models, scene, screenshot);
            ImGui::EndFrame();
        };
        frame();
        frame();
        const char* title = kind == woby::ManualPathKind::models ? "Open model files##manual"
            : kind == woby::ManualPathKind::folder ? "Open model folder##manual"
            : kind == woby::ManualPathKind::openScene ? "Open scene##manual"
            : png ? "Save screenshot##manual" : "Save scene##manual";
        auto* window = ImGui::FindWindowByName(title);
        REQUIRE(window);
        CHECK(window->Pos.x == doctest::Approx(100));
        CHECK(window->Pos.y == doctest::Approx(120));
        ImGui::ActivateItemByID(window->GetID(saving ? "Save" : "Open"));
        frame();
        CHECK_FALSE(dialog.active);
        if (kind == woby::ManualPathKind::models) { CHECK(woby::takePendingModelPaths(models) == std::vector<std::filesystem::path>{file}); }
        if (kind == woby::ManualPathKind::folder) { CHECK(woby::takePendingModelFolderTreeRoots(models) == std::vector<std::filesystem::path>{fixture.root}); }
        if (kind == woby::ManualPathKind::openScene) { CHECK(woby::takePendingOpenScenePath(scene) == file); }
        if (kind == woby::ManualPathKind::saveScene) { CHECK(woby::takePendingSaveScenePath(scene) == fixture.root / "new.woby"); }
        if (png) { CHECK(woby::takePendingSaveSceneScreenshotPath(screenshot) == fixture.root / "new.png"); }
    }
}

TEST_CASE("manual dialog cancellation closes without queuing paths and can reopen")
{
    DialogUi ui;
    woby::ModelFileDialogState models;
    woby::SceneFileDialogState scene;
    woby::SceneScreenshotDialogState screenshot;
    woby::requestManualPathDialog(models.fallback, "No native chooser");
    const auto frame = [&]() {
        ImGui::NewFrame();
        woby::drawNativeDialogFallbacks(models, scene, screenshot);
        ImGui::EndFrame();
    };
    frame();
    frame();
    SUBCASE("cancel button") {
        auto* window = ImGui::FindWindowByName("Open model files##manual");
        REQUIRE(window);
        ImGui::ActivateItemByID(window->GetID("Cancel"));
    }
    SUBCASE("escape") { ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true); }
    frame();
    CHECK_FALSE(woby::modelFileDialogIsOpen(models));
    CHECK(woby::takePendingModelPaths(models).empty());
    CHECK(models.status == "Open canceled");
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false);
    woby::requestManualPathDialog(models.fallback, "Still unavailable");
    frame();
    CHECK(woby::modelFileDialogIsOpen(models));
}

#if defined(__linux__)
TEST_CASE("Linux native dialog backend failures activate the manual fallback")
{
    DialogUi ui;
    const char* hint = SDL_GetHint(SDL_HINT_FILE_DIALOG_DRIVER);
    struct RestoreHint {
        std::optional<std::string> value;
        ~RestoreHint() { SDL_SetHint(SDL_HINT_FILE_DIALOG_DRIVER, value ? value->c_str() : nullptr); }
    } restore{hint ? std::optional<std::string>(hint) : std::nullopt};
    REQUIRE(SDL_SetHint(SDL_HINT_FILE_DIALOG_DRIVER, "woby-test-unavailable"));
    woby::ModelFileDialogState models;
    woby::SceneFileDialogState scene;
    woby::SceneScreenshotDialogState screenshot;
    SUBCASE("models") { woby::showModelFileDialog(nullptr, models); CHECK(models.fallback.active); CHECK(woby::modelFileDialogIsOpen(models)); }
    SUBCASE("folders") { woby::showModelFolderTreeDialog(nullptr, models); CHECK(models.fallback.active); CHECK(models.fallback.kind == woby::ManualPathKind::folder); }
    SUBCASE("open scene") { woby::showOpenSceneDialog(nullptr, scene); CHECK(scene.fallback.active); CHECK(woby::sceneFileDialogIsOpen(scene)); }
    SUBCASE("save scene") { woby::showSaveSceneDialog(nullptr, scene); CHECK(scene.fallback.active); CHECK(scene.fallback.kind == woby::ManualPathKind::saveScene); }
    SUBCASE("screenshot") { woby::showSaveSceneScreenshotDialog(nullptr, screenshot); CHECK(screenshot.fallback.active); CHECK(woby::sceneScreenshotDialogIsOpen(screenshot)); }
}
#endif

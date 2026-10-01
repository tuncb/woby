#include "utf8_path.h"
#include "background_load.h"
#include "file_discovery.h"
#include "importer_host.h"
#include "model_load.h"
#include "control_importers.h"
#include "control_scene.h"
#include "scene_history.h"
#include "ui_operations.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <cmath>
#include <chrono>
#include <random>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

// Cleanup only; the host itself uses structs and free functions.
struct ImporterTestScope {
    std::filesystem::path root = std::filesystem::absolute(std::filesystem::temp_directory_path()) / ("woby_importer_tests_"
        + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
        + "_" + std::to_string(std::random_device{}()));
    ImporterTestScope()
    {
        woby::unloadImporters();
        std::filesystem::create_directories(root);
    }
    ~ImporterTestScope()
    {
        woby::unloadImporters();
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
};

void writeFile(const std::filesystem::path& path, const std::string& contents = "fixture")
{
    std::ofstream stream(path);
    stream << contents;
}

std::filesystem::path installPortableImporter(const std::filesystem::path& folder,
    const std::filesystem::path& library = WOBY_TEST_IMPORTER)
{
    std::filesystem::create_directories(folder);
    const auto destination = folder / library.filename();
    std::filesystem::copy_file(library, destination);
    writeFile(folder / "importer.json", nlohmann::json{{"schema", 1},
        {"library", woby::pathToUtf8(library.filename())}}.dump());
    return destination;
}

} // namespace

TEST_CASE("DLL importer registration validates ABI exports conflicts and deduplicates paths")
{
    ImporterTestScope scope;
    CHECK_THROWS_AS((void)woby::loadImporter(scope.root / "absent.dll"), std::filesystem::filesystem_error);
    CHECK_THROWS_WITH_AS((void)woby::loadImporter(WOBY_TEST_BAD_IMPORTER), doctest::Contains("Incompatible importer API"), std::runtime_error);
    CHECK_THROWS_WITH_AS((void)woby::loadImporter(WOBY_TEST_MISSING_EXPORT), doctest::Contains("Missing woby_get_importer_api"), std::runtime_error);
    CHECK_THROWS_WITH_AS((void)woby::loadImporter(WOBY_TEST_RESERVED_IMPORTER), doctest::Contains("reserved importer extension"), std::runtime_error);
    CHECK(woby::loadedImporters().empty());
    woby::loadImporter(WOBY_TEST_IMPORTER);
    woby::loadImporter(WOBY_TEST_IMPORTER);
    CHECK_THROWS_WITH_AS((void)woby::loadImporter(WOBY_TEST_CONFLICT_IMPORTER), doctest::Contains("extension already registered"), std::runtime_error);
    REQUIRE(woby::loadedImporters().size() == 1u);
    CHECK(woby::loadedImporters()[0].extensions.size() == 2u);
    const auto copy = scope.root / std::filesystem::path(WOBY_TEST_IMPORTER).filename();
    std::filesystem::copy_file(WOBY_TEST_IMPORTER, copy, std::filesystem::copy_options::overwrite_existing);
    CHECK_THROWS_WITH_AS((void)woby::loadImporter(copy), doctest::Contains("Duplicate importer ID"), std::runtime_error);
    CHECK(woby::loadedImporters().size() == 1u);
}

TEST_CASE("ctl importer registration separates live loading from remembered configuration")
{
    ImporterTestScope scope;
    const auto settings = scope.root / "importers.json";
    std::vector<std::filesystem::path> remembered;
    woby::ControlOperation command;
    command.action = woby::ControlAction::importersAdd;
    command.path = WOBY_TEST_IMPORTER;
    auto result = woby::applyControlImporterOperation(command, settings, remembered);
    CHECK(result["loadedCount"] == 1);
    CHECK(remembered.empty());
    CHECK_FALSE(std::filesystem::exists(settings));
    command.remember = true;
    result = woby::applyControlImporterOperation(command, settings, remembered);
    CHECK(result["outcomes"][0]["remembered"] == true);
    CHECK(woby::readImporterSettings(settings) == remembered);
    REQUIRE(remembered.size() == 1);
    command.action = woby::ControlAction::importersForget;
    result = woby::applyControlImporterOperation(command, settings, remembered);
    CHECK(result["forgotten"] == true);
    CHECK(remembered.empty());
    CHECK(woby::readImporterSettings(settings).empty());
    CHECK(woby::loadedImporters().size() == 1);
    CHECK(woby::applyControlImporterOperation(command, settings, remembered)["forgotten"] == false);
    command.action = woby::ControlAction::importersAdd;
    command.path = WOBY_TEST_BAD_IMPORTER;
    result = woby::applyControlImporterOperation(command, settings, remembered);
    CHECK(result["failedCount"] == 1);
    CHECK(result["loadedCount"] == 0);
    CHECK(result["outcomes"][0].contains("error"));
    CHECK(remembered.empty());
    command.path = WOBY_TEST_IMPORTER;
    // Force a file-as-parent settings failure while the importer remains loaded.
    const auto blocked = scope.root / "blocked";
    writeFile(blocked);
    remembered.clear();
    result = woby::applyControlImporterOperation(command, blocked / "settings.json", remembered);
    CHECK(result["loadedCount"] == 1);
    CHECK_FALSE(result["registrationError"].is_null());
    CHECK(remembered.empty());
}

TEST_CASE("DLL importer supports case insensitive discovery and owns copied mesh data")
{
    ImporterTestScope scope;
    woby::loadImporter(WOBY_TEST_IMPORTER);
    const auto path = scope.root / "triangle.WTEST";
    writeFile(path);
    writeFile(scope.root / "ignored.txt");
    CHECK(woby::isModelPath(path));
    CHECK(woby::isModelPath("triangle.WT2"));
    REQUIRE(woby::collectModelPathsRecursive(scope.root).size() == 1u);
    auto model = woby::loadModel(path);
    CHECK_THROWS_WITH_AS((void)woby::loadModel("model.obj", "org.woby.test"), doctest::Contains("no longer supports"), std::runtime_error);
    CHECK(model.importerId == "org.woby.test");
    REQUIRE(model.mesh.vertices.size() == 4u);
    CHECK(model.mesh.vertices[3].position == std::array<float, 3>{9, 9, 9});
    CHECK(model.mesh.indices == std::vector<uint32_t>{0, 1, 2});
    REQUIRE(model.mesh.nodes.size() == 1u);
    CHECK(model.mesh.nodes[0].name == "triangle");
    CHECK(model.mesh.bounds.max == std::array<float, 3>{9, 9, 9});
    for (const auto& vertex : model.mesh.vertices) { CHECK(woby::validNormal(vertex.normal)); }
    woby::unloadImporters();
    CHECK_FALSE(woby::isModelPath(path));
    CHECK(model.mesh.vertices.size() == 4u);
    CHECK_THROWS_WITH_AS((void)woby::loadModel(path, "org.woby.test"), doctest::Contains("Required importer is not loaded"), std::runtime_error);
}

TEST_CASE("DLL importer rejects malformed outputs and releases every result")
{
    ImporterTestScope scope;
    woby::loadImporter(WOBY_TEST_IMPORTER);
    for (const auto* name : {"invalid_index", "nan_position", "invalid_group", "invalid_flags", "invalid_color",
            "null_buffer", "huge_buffer", "short_result", "failure"}) {
        CHECK_THROWS_AS((void)woby::loadModel(scope.root / (std::string(name) + ".wtest")), std::runtime_error);
        CHECK_NOTHROW((void)woby::loadModel(scope.root / "valid.wtest"));
    }
}

TEST_CASE("DLL importer cancellation and callback exceptions release results without crossing ABI")
{
    ImporterTestScope scope;
    woby::loadImporter(WOBY_TEST_IMPORTER);
    bool canceled = false;
    int updates = 0;
    const auto model = woby::loadModel("triangle.wtest", {}, {
        [&] { return canceled; }, [&](float fraction) { ++updates; CHECK(fraction == 0.5f); canceled = true; }});
    CHECK(model.canceled);
    CHECK(model.mesh.vertices.empty());
    CHECK(updates == 1);
    CHECK_THROWS_WITH_AS((void)woby::loadModel("triangle.wtest", {}, {{}, [](float) {
        throw std::runtime_error("callback failed");
    }}), "callback failed", std::runtime_error);
    CHECK_NOTHROW((void)woby::loadModel("valid.wtest"));
    int polls = 0;
    const auto canceledAfterCopy = woby::loadModel("valid.wtest", {}, {[&] { return ++polls == 4; }, {}});
    CHECK(canceledAfterCopy.canceled);
    CHECK(canceledAfterCopy.mesh.vertices.empty());
    CHECK_NOTHROW((void)woby::loadModel("valid.wtest"));
}

TEST_CASE("DLL importer calls serialize across workers")
{
    ImporterTestScope scope;
    woby::loadImporter(WOBY_TEST_IMPORTER);
    std::exception_ptr errorA, errorB;
    const auto run = [](std::exception_ptr& error) {
        try { for (int i = 0; i < 20; ++i) { (void)woby::loadModel("valid.wtest"); } }
        catch (...) { error = std::current_exception(); }
    };
    std::thread a(run, std::ref(errorA)), b(run, std::ref(errorB));
    a.join(); b.join();
    CHECK_FALSE(errorA);
    CHECK_FALSE(errorB);
}

TEST_CASE("Plugin folders scan sorted immediate libraries only")
{
    ImporterTestScope scope;
    const auto extension = std::filesystem::path(WOBY_TEST_IMPORTER).extension();
    const auto a = scope.root / std::filesystem::path("a").concat(extension.native());
    const auto b = scope.root / std::filesystem::path("b").concat(extension.native());
    writeFile(b); writeFile(a); writeFile(scope.root / "ignored.txt");
    std::filesystem::create_directories(scope.root / "nested");
    writeFile(scope.root / "nested" / a.filename());
    const auto found = woby::discoverImporterFiles(scope.root);
    REQUIRE(found.size() == 2u);
    CHECK(found[0] == a);
    CHECK(found[1] == b);
}

TEST_CASE("Portable importers load only manifest entries and remain relocatable")
{
    ImporterTestScope scope;
    const auto deployment = scope.root / "deployment";
    const auto folder = deployment / "importers";
    CHECK(woby::loadPortableImporters(folder).empty());
    CHECK_FALSE(std::filesystem::exists(folder));
    const auto package = folder / woby::pathFromUtf8("caf\xc3\xa9 importer");
    const auto library = installPortableImporter(package);
    writeFile(package / "dependency.dll", "not an importer");
    writeFile(folder / "loose.dll", "not an importer");
    // Neither a package without a manifest nor a deeper nested package is scanned.
    std::filesystem::create_directories(folder / "unregistered");
    installPortableImporter(folder / "unregistered" / "nested", WOBY_TEST_BAD_IMPORTER);
    CHECK(woby::loadPortableImporters(folder).empty());
    REQUIRE(woby::loadedImporters().size() == 1u);
    CHECK(woby::loadedImporters()[0].path == std::filesystem::canonical(library));
    CHECK(woby::loadPortableImporters(folder).empty());
    CHECK(woby::loadedImporters().size() == 1u);
    woby::unloadImporters();
    const auto moved = scope.root / "moved deployment";
    std::filesystem::rename(deployment, moved);
    CHECK(woby::loadPortableImporters(moved / "importers").empty());
    REQUIRE(woby::loadedImporters().size() == 1u);
    CHECK(woby::loadedImporters()[0].path == std::filesystem::canonical(
        moved / "importers" / package.filename() / library.filename()));
}

TEST_CASE("Portable importer failures are isolated and conflicts use sorted package order")
{
    ImporterTestScope scope;
    const auto folder = scope.root / "importers";
    installPortableImporter(folder / "00-bad-abi", WOBY_TEST_BAD_IMPORTER);
    installPortableImporter(folder / "10-first");
    installPortableImporter(folder / "20-conflict", WOBY_TEST_CONFLICT_IMPORTER);
    installPortableImporter(folder / "30-independent", WOBY_TEST_OFF_IMPORTER);
    const auto errors = woby::loadPortableImporters(folder);
    REQUIRE(errors.size() == 2u);
    CHECK(errors[0].find("00-bad-abi") != std::string::npos);
    CHECK(errors[1].find("extension already registered") != std::string::npos);
    REQUIRE(woby::loadedImporters().size() == 2u);
    CHECK(woby::loadedImporters()[0].id == "org.woby.test");
    CHECK(woby::loadedImporters()[1].id == "org.woby.example.off");
}

TEST_CASE("Explicit importer registrations keep precedence over portable packages")
{
    ImporterTestScope scope;
    const auto folder = scope.root / "importers";
    const auto explicitLibrary = installPortableImporter(scope.root / "explicit", WOBY_TEST_CONFLICT_IMPORTER);
    installPortableImporter(folder / "automatic");
    woby::loadImporter(explicitLibrary);
    CHECK(woby::loadPortableImporters(folder).size() == 1u);
    REQUIRE(woby::loadedImporters().size() == 1u);
    CHECK(woby::loadedImporters()[0].id == "org.woby.conflict");
}

TEST_CASE("Portable manifests reject malformed metadata and paths outside their package")
{
    ImporterTestScope scope;
    const auto folder = scope.root / "importers";
    const auto package = folder / "bad";
    const auto library = installPortableImporter(package);
    nlohmann::json manifest{{"schema", 1}, {"library", woby::pathToUtf8(library.filename())}};
    SUBCASE("unsupported schema") { manifest["schema"] = 2; }
    SUBCASE("missing schema") { manifest.erase("schema"); }
    SUBCASE("fractional schema") { manifest["schema"] = 1.0; }
    SUBCASE("missing library") { manifest.erase("library"); }
    SUBCASE("wrong type") { manifest["library"] = 42; }
    SUBCASE("empty path") { manifest["library"] = ""; }
    SUBCASE("absolute path") { manifest["library"] = woby::pathToUtf8(library); }
    SUBCASE("drive relative path") { manifest["library"] = "C:plugin.dll"; }
    SUBCASE("backslash escape") { manifest["library"] = "..\\plugin.dll"; }
    SUBCASE("parent escape") { manifest["library"] = "../bad/" + woby::pathToUtf8(library.filename()); }
    SUBCASE("embedded null") { manifest["library"] = std::string("plugin\0.dll", 11); }
    SUBCASE("missing library file") { manifest["library"] = "missing.dll"; }
    SUBCASE("directory") { manifest["library"] = "."; }
    auto contents = manifest.dump();
    SUBCASE("malformed JSON") { contents = "{"; }
    SUBCASE("oversized manifest") { contents = std::string(65537, ' '); }
    writeFile(package / "importer.json", contents);
    installPortableImporter(folder / "good", WOBY_TEST_OFF_IMPORTER);
    const auto errors = woby::loadPortableImporters(folder);
    REQUIRE(errors.size() == 1u);
    CHECK(errors[0].find("importer.json") != std::string::npos);
    REQUIRE(woby::loadedImporters().size() == 1u);
    CHECK(woby::loadedImporters()[0].id == "org.woby.example.off");
}

TEST_CASE("Portable manifests support libraries in package subdirectories")
{
    ImporterTestScope scope;
    const auto folder = scope.root / "importers";
    const auto package = folder / "nested";
    const auto library = installPortableImporter(package / "bin");
    writeFile(package / "importer.json", nlohmann::json{{"schema", 1},
        {"library", "bin/" + woby::pathToUtf8(library.filename())}}.dump());
    CHECK(woby::loadPortableImporters(folder).empty());
    REQUIRE(woby::loadedImporters().size() == 1u);
    CHECK(woby::loadedImporters()[0].path == std::filesystem::canonical(library));
}

TEST_CASE("Portable manifests reject library links escaping their package")
{
    ImporterTestScope scope;
    const auto folder = scope.root / "importers";
    const auto package = folder / "linked";
    std::filesystem::create_directories(package);
    const auto outside = installPortableImporter(scope.root / "outside");
    const auto link = package / outside.filename();
    std::error_code error;
    std::filesystem::create_symlink(outside, link, error);
    if (error) {
        MESSAGE("Library link test unavailable: ", error.message());
        return;
    }
    writeFile(package / "importer.json", nlohmann::json{{"schema", 1},
        {"library", woby::pathToUtf8(link.filename())}}.dump());
    const auto errors = woby::loadPortableImporters(folder);
    REQUIRE(errors.size() == 1u);
    CHECK(errors[0].find("inside its package") != std::string::npos);
    CHECK(woby::loadedImporters().empty());
}

TEST_CASE("Portable importer folder errors are reported without throwing")
{
    ImporterTestScope scope;
    const auto folder = scope.root / "importers";
    writeFile(folder);
    const auto errors = woby::loadPortableImporters(folder);
    REQUIRE(errors.size() == 1u);
    CHECK(errors[0].find("importers") != std::string::npos);
}

TEST_CASE("Importer settings round trip Unicode spaces and removals")
{
    ImporterTestScope scope;
    const auto settings = scope.root / "importers.txt";
    const auto path = scope.root / woby::pathFromUtf8("caf\xc3\xa9 folder") / "plugin.dll";
    CHECK(woby::readImporterSettings(settings).empty());
    woby::writeImporterSettings(settings, {path});
    REQUIRE(woby::readImporterSettings(settings).size() == 1u);
    CHECK(woby::readImporterSettings(settings)[0] == path);
    woby::writeImporterSettings(settings, {});
    CHECK(woby::readImporterSettings(settings).empty());
    writeFile(settings, "\"unterminated\n");
    CHECK_THROWS_AS((void)woby::readImporterSettings(settings), std::runtime_error);
}

TEST_CASE("Plugin scene round trips importer identity and rejects missing or changed groups")
{
    ImporterTestScope scope;
    woby::loadImporter(WOBY_TEST_IMPORTER);
    const auto path = scope.root / "triangle.wtest";
    writeFile(path);
    std::vector<float> fractions;
    auto loaded = woby::loadModelBatchCpu({path}, 0u, [&](const woby::BackgroundLoadProgress& p) {
        fractions.push_back(p.currentFileFraction);
    }, {});
    REQUIRE(loaded.files.size() == 1u);
    CHECK(fractions.back() == 1.0f);
    woby::UiState state;
    state.files = std::move(loaded.files);
    auto document = woby::createSceneDocument(state);
    REQUIRE(document.files[0].importerId == "org.woby.test");
    const auto scene = scope.root / "scene.woby";
    woby::writeSceneDocument(scene, document);
    const auto restored = woby::loadSceneCpu(scene, {}, {});
    REQUIRE(restored.files.size() == 1u);
    CHECK(restored.files[0].importerId == "org.woby.test");
    document.files[0].groups[0].name = "old_group";
    woby::writeSceneDocument(scene, document);
    CHECK_THROWS_WITH_AS((void)woby::loadSceneCpu(scene, {}, {}), doctest::Contains("Importer group order changed"), std::runtime_error);
    woby::unloadImporters();
    CHECK_THROWS_WITH_AS((void)woby::loadSceneCpu(scene, {}, {}), doctest::Contains("Required importer is not loaded"), std::runtime_error);
}

TEST_CASE("Background plugin cancellation does not append a partial file")
{
    ImporterTestScope scope;
    woby::loadImporter(WOBY_TEST_IMPORTER);
    bool canceled = false;
    const auto result = woby::loadModelBatchCpu({"triangle.wtest"}, 0u,
        [&](const woby::BackgroundLoadProgress& p) { canceled = p.currentFileFraction > 0.0f; },
        [&] { return canceled; });
    CHECK(result.canceled);
    CHECK(result.files.empty());
    CHECK(result.failedCount == 0u);
}

TEST_CASE("Imported appearance initializes UI state and hidden geometry remains available")
{
    ImporterTestScope scope;
    woby::loadImporter(WOBY_TEST_IMPORTER);
    const auto path = scope.root / "colored_hidden.wtest";
    writeFile(path);
    auto imported = woby::loadModel(path);
    REQUIRE(imported.mesh.nodes.size() == 1u);
    // The fixture clears its metadata during release_result.
    REQUIRE(imported.mesh.nodes[0].defaultColor.has_value());
    CHECK(*imported.mesh.nodes[0].defaultColor == std::array<float, 4>{0.2f, 0.4f, 0.6f, 0.25f});
    CHECK_FALSE(imported.mesh.nodes[0].defaultVisible);
    auto file = woby::createUiFileState(path, std::move(imported.mesh), 5u, imported.importerId);
    REQUIRE(file.groupSettings.size() == 1u);
    CHECK(file.groupSettings[0].color == std::array<float, 4>{0.2f, 0.4f, 0.6f, 1.0f});
    CHECK(file.groupSettings[0].opacity == 0.25f);
    CHECK_FALSE(file.groupSettings[0].visible);
    CHECK_FALSE(file.fileSettings.visible);
    CHECK(file.mesh.vertices.size() == 4u);
    CHECK(file.mesh.indices == std::vector<uint32_t>{0, 1, 2});
    woby::setGroupVisible(file, file.groupSettings[0], true);
    CHECK(file.groupSettings[0].visible);
    CHECK(file.fileSettings.visible);
}

TEST_CASE("Saved scenes override importer appearance defaults in both directions")
{
    ImporterTestScope scope;
    woby::loadImporter(WOBY_TEST_IMPORTER);
    const auto hiddenPath = scope.root / "colored_hidden.wtest";
    const auto plainPath = scope.root / "plain.wtest";
    writeFile(hiddenPath);
    writeFile(plainPath);
    auto loaded = woby::loadModelBatchCpu({hiddenPath, plainPath}, 5u, {}, {});
    REQUIRE(loaded.files.size() == 2u);
    REQUIRE(loaded.files[0].groupSettings.size() == 1u);
    REQUIRE(loaded.files[1].groupSettings.size() == 1u);
    CHECK_FALSE(loaded.files[0].groupSettings[0].visible);
    CHECK(loaded.files[0].groupSettings[0].opacity == 0.25f);
    CHECK(loaded.files[1].groupSettings[0].visible);
    CHECK(loaded.files[1].groupSettings[0].color == woby::defaultGroupColor(6u));
    CHECK(loaded.files[1].groupSettings[0].opacity == 1.0f);

    woby::UiState state;
    state.files = std::move(loaded.files);
    const auto scene = scope.root / "appearance.woby";
    woby::writeSceneDocument(scene, woby::createSceneDocument(state));
    auto restored = woby::loadSceneCpu(scene, {}, {});
    REQUIRE(restored.files.size() == 2u);
    CHECK_FALSE(restored.files[0].groupSettings[0].visible);
    CHECK_FALSE(restored.files[0].fileSettings.visible);
    CHECK(restored.files[0].groupSettings[0].color == state.files[0].groupSettings[0].color);
    CHECK(restored.files[0].groupSettings[0].opacity == 0.25f);

    auto& first = state.files[0];
    auto& second = state.files[1];
    woby::setGroupVisible(first, first.groupSettings[0], true);
    woby::setGroupColor(first.groupSettings[0], {0.8f, 0.1f, 0.3f, 1.0f});
    woby::setGroupOpacity(first.groupSettings[0], 0.75f);
    woby::setGroupVisible(second, second.groupSettings[0], false);
    woby::setGroupColor(second.groupSettings[0], {0.1f, 0.8f, 0.2f, 1.0f});
    woby::setGroupOpacity(second.groupSettings[0], 0.5f);
    auto document = woby::createSceneDocument(state);
    woby::writeSceneDocument(scene, document);
    restored = woby::loadSceneCpu(scene, {}, {});
    REQUIRE(restored.files.size() == 2u);
    for (size_t i = 0; i < state.files.size(); ++i) {
        CHECK(restored.files[i].fileSettings.visible == state.files[i].fileSettings.visible);
        CHECK(restored.files[i].groupSettings[0].visible == state.files[i].groupSettings[0].visible);
        CHECK(restored.files[i].groupSettings[0].color == state.files[i].groupSettings[0].color);
        CHECK(restored.files[i].groupSettings[0].opacity == state.files[i].groupSettings[0].opacity);
    }
    // Simulate changed source defaults using the same importer/group layout.
    document.files[1].path = hiddenPath;
    woby::writeSceneDocument(scene, document);
    restored = woby::loadSceneCpu(scene, {}, {});
    REQUIRE(restored.files.size() == 2u);
    CHECK(restored.files[1].groupSettings[0].color == second.groupSettings[0].color);
    CHECK(restored.files[1].groupSettings[0].opacity == 0.5f);
    CHECK_FALSE(restored.files[1].groupSettings[0].visible);
}

TEST_CASE("OFF example DLL reads real files including Unicode paths")
{
    ImporterTestScope scope;
    woby::loadImporter(WOBY_TEST_OFF_IMPORTER);
    const auto path = scope.root / woby::pathFromUtf8("triangle_\xc3\xa9.off");
    writeFile(path, "OFF\n3 1 0\n0 0 0\n1 0 0\n0 1 0\n3 0 1 2\n");
    const auto loaded = woby::loadModel(path);
    CHECK(loaded.importerId == "org.woby.example.off");
    CHECK(loaded.mesh.vertices.size() == 3u);
    CHECK(loaded.mesh.indices.size() == 3u);
    woby::UiState state;
    state.files.push_back(woby::createUiFileState(path, loaded.mesh, 0u, loaded.importerId));
    const auto scene = scope.root / "unicode.woby";
    woby::writeSceneDocument(scene, woby::createSceneDocument(state));
    const auto reopened = woby::loadSceneCpu(scene, {}, {});
    REQUIRE(reopened.files.size() == 1u);
    CHECK(reopened.files[0].path == path);
    CHECK(reopened.files[0].importerId == loaded.importerId);
    writeFile(path, "OFF\n3 1 0\n0 0 0\n1 0 0\n0 1 0\n3 0 1 9\n");
    CHECK_THROWS_WITH_AS((void)woby::loadModel(path), doctest::Contains("triangular OFF faces"), std::runtime_error);
}

TEST_CASE("Importer mesh validation handles optional attributes and group partitions")
{
    WobyImportVertex vertices[] = {
        {{0, 0, 0}, {0, 0, 2}, {0, 0}},
        {{1, 0, 0}, {0, 0, 2}, {1, 0}},
        {{0, 1, 0}, {0, 0, 2}, {0, 1}},
    };
    uint32_t indices[] = {0, 1, 2, 2, 1, 0};
    WobyImportGroup groups[] = {{"front", 0, 3, 0u, {}}, {"back", 3, 3, 0u, {}}};
    WobyImportResult result{};
    result.struct_size = sizeof(result);
    result.vertices = vertices;
    result.vertex_count = 3;
    result.indices = indices;
    result.index_count = 6;
    result.groups = groups;
    result.group_count = 2;
    result.flags = WOBY_IMPORT_HAS_NORMALS | WOBY_IMPORT_HAS_TEXCOORDS;
    const auto mesh = woby::copyImportedMesh(result);
    REQUIRE(mesh.nodes.size() == 2u);
    CHECK(mesh.nodes[1].indexOffset == 3u);
    CHECK(mesh.vertices[0].normal[2] == 1.0f);
    CHECK(mesh.nodes[0].hasTexcoords);
    CHECK(mesh.nodes[1].hasTexcoords);
    CHECK(mesh.vertices[1].texcoord == std::array<float, 2>{1, 0});
    groups[1].name = "front";
    CHECK_THROWS_AS((void)woby::copyImportedMesh(result), std::runtime_error);
    groups[1].name = "back";
    groups[1].index_count = UINT32_MAX;
    CHECK_THROWS_AS((void)woby::copyImportedMesh(result), std::runtime_error);
    groups[1].index_count = 3;
    result.group_count = 1;
    CHECK_THROWS_AS((void)woby::copyImportedMesh(result), std::runtime_error);
    result.group_count = 0;
    result.flags = 8;
    CHECK_THROWS_AS((void)woby::copyImportedMesh(result), std::runtime_error);
    result.flags = WOBY_IMPORT_HAS_NORMALS;
    vertices[0].normal[2] = 0;
    CHECK_THROWS_AS((void)woby::copyImportedMesh(result), std::runtime_error);
    result.flags = WOBY_IMPORT_HAS_TEXCOORDS;
    vertices[0].texcoord[0] = std::numeric_limits<float>::infinity();
    CHECK_THROWS_AS((void)woby::copyImportedMesh(result), std::runtime_error);
    result.flags = 0;
    CHECK_NOTHROW((void)woby::copyImportedMesh(result));
    CHECK_FALSE(woby::copyImportedMesh(result).nodes[0].hasTexcoords);
}

TEST_CASE("Importer group appearance is optional validated and copied per group")
{
    ImporterTestScope scope;
    WobyImportVertex vertices[] = {
        {{0, 0, 0}, {}, {}}, {{1, 0, 0}, {}, {}}, {{0, 1, 0}, {}, {}},
    };
    uint32_t indices[] = {0, 1, 2, 2, 1, 0};
    WobyImportGroup groups[] = {
        {"front", 0, 3, WOBY_IMPORT_GROUP_HAS_COLOR, {0.0f, 0.5f, 1.0f, 0.0f}},
        {"back", 3, 3, WOBY_IMPORT_GROUP_INITIALLY_HIDDEN, {}},
    };
    // Unflagged color storage is ignored, even if it does not contain a valid color.
    groups[1].color[0] = std::numeric_limits<float>::quiet_NaN();
    WobyImportResult result{};
    result.struct_size = sizeof(result);
    result.vertices = vertices;
    result.vertex_count = 3;
    result.indices = indices;
    result.index_count = 6;
    result.groups = groups;
    result.group_count = 2;
    auto mesh = woby::copyImportedMesh(result);
    REQUIRE(mesh.nodes.size() == 2u);
    CHECK(mesh.indices == std::vector<uint32_t>{0, 1, 2, 2, 1, 0});
    CHECK(mesh.nodes[0].indexOffset == 0u);
    CHECK(mesh.nodes[1].indexOffset == 3u);
    CHECK_FALSE(mesh.nodes[1].defaultColor.has_value());
    const auto file = woby::createUiFileState(scope.root / "groups.wtest", std::move(mesh), 7u);
    CHECK(file.fileSettings.visible);
    REQUIRE(file.groupSettings.size() == 2u);
    CHECK(file.groupSettings[0].visible);
    CHECK(file.groupSettings[0].color == std::array<float, 4>{0.0f, 0.5f, 1.0f, 1.0f});
    CHECK(file.groupSettings[0].opacity == 0.0f);
    CHECK_FALSE(file.groupSettings[1].visible);
    CHECK(file.groupSettings[1].color == woby::defaultGroupColor(8u));
    CHECK(file.groupSettings[1].opacity == 1.0f);

    for (size_t component = 0; component < 4u; ++component) {
        const float original = groups[0].color[component];
        for (const float invalid : {-0.01f, 1.01f, std::numeric_limits<float>::quiet_NaN(),
                std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()}) {
            groups[0].color[component] = invalid;
            CHECK_THROWS_WITH_AS((void)woby::copyImportedMesh(result), doctest::Contains("group color"), std::runtime_error);
        }
        groups[0].color[component] = original;
    }
    groups[0].flags = 4u;
    CHECK_THROWS_WITH_AS((void)woby::copyImportedMesh(result), doctest::Contains("group flags"), std::runtime_error);
    groups[0].flags = WOBY_IMPORT_GROUP_HAS_COLOR;
    groups[0].color[3] = 1.0f;
    CHECK_NOTHROW((void)woby::copyImportedMesh(result));

    result.group_count = 0;
    result.groups = nullptr;
    const auto fallback = woby::createUiFileState(scope.root / "ungrouped.wtest", woby::copyImportedMesh(result), 7u);
    REQUIRE(fallback.groupSettings.size() == 1u);
    CHECK(fallback.mesh.nodes[0].name == "Mesh");
    CHECK(fallback.groupSettings[0].visible);
    CHECK(fallback.groupSettings[0].color == woby::defaultGroupColor(7u));
    CHECK(fallback.groupSettings[0].opacity == 1.0f);
}

TEST_CASE("Importer hierarchy validates structure coverage labels and limits")
{
    const WobyImportVertex vertices[] = {{{0, 0, 0}, {}, {}}, {{1, 0, 0}, {}, {}}, {{0, 1, 0}, {}, {}}};
    const uint32_t indices[] = {0, 1, 2, 0, 1, 2};
    const WobyImportGroup groups[] = {{"a/patch", 0, 3, 0, {}}, {"b/patch", 3, 3, 0, {}}};
    WobyImportResult result{};
    result.struct_size = sizeof(result);
    result.vertices = vertices; result.vertex_count = 3;
    result.indices = indices; result.index_count = 6;
    result.groups = groups; result.group_count = 2;
    WobyImportHierarchyNode nodes[] = {
        {"A", WOBY_IMPORT_NO_PARENT, WOBY_IMPORT_NO_GROUP}, {"Patch", 0, 0},
        {"B", WOBY_IMPORT_NO_PARENT, WOBY_IMPORT_NO_GROUP}, {"Patch", 2, 1},
    };
    WobyImportHierarchy hierarchy{sizeof(WobyImportHierarchy), nodes, 4};
    SUBCASE("repeated display labels keep unique source identities and ordering") {
        const auto mesh = woby::copyImportedMesh(result, &hierarchy);
        REQUIRE(mesh.hierarchy.size() == 4);
        CHECK(mesh.hierarchy[3].parentIndex == 2);
        CHECK(mesh.nodes[0].name == "a/patch");
        CHECK(mesh.nodes[1].name == "b/patch");
        CHECK(woby::meshNodeDisplayName(mesh.nodes[0]) == "Patch");
        CHECK(woby::meshNodeDisplayName(mesh.nodes[1]) == "Patch");
        return;
    }
    SUBCASE("short metadata") { hierarchy.struct_size = 0; }
    SUBCASE("null buffer") { hierarchy.nodes = nullptr; }
    SUBCASE("empty forest") { hierarchy.node_count = 0; }
    SUBCASE("too many nodes") { hierarchy.node_count = 100001; }
    SUBCASE("self parent") { nodes[0].parent_index = 0; }
    SUBCASE("forward parent or cycle") { nodes[0].parent_index = 2; nodes[2].parent_index = 0; }
    SUBCASE("invalid parent") { nodes[3].parent_index = 100; }
    SUBCASE("leaf cannot parent") { nodes[2].parent_index = 1; }
    SUBCASE("invalid group") { nodes[3].group_index = 2; }
    SUBCASE("repeated group") { nodes[3].group_index = 0; }
    SUBCASE("missing group") { hierarchy.node_count = 3; }
    SUBCASE("null label") { nodes[0].name = nullptr; }
    SUBCASE("empty label") { nodes[0].name = ""; }
    CHECK_THROWS_AS((void)woby::copyImportedMesh(result, &hierarchy), std::runtime_error);
}

TEST_CASE("Importer hierarchy bounds depth and label storage and supports ungrouped meshes")
{
    const WobyImportVertex vertices[] = {{{0, 0, 0}, {}, {}}, {{1, 0, 0}, {}, {}}, {{0, 1, 0}, {}, {}}};
    const uint32_t indices[] = {0, 1, 2};
    WobyImportResult result{};
    result.struct_size = sizeof(result);
    result.vertices = vertices; result.vertex_count = 3;
    result.indices = indices; result.index_count = 3;
    std::vector<WobyImportHierarchyNode> nodes;
    for (uint32_t i = 0; i < WOBY_IMPORT_MAX_HIERARCHY_DEPTH - 1u; ++i) {
        nodes.push_back({"Container", i == 0 ? WOBY_IMPORT_NO_PARENT : i - 1u, WOBY_IMPORT_NO_GROUP});
    }
    nodes.push_back({"Surface", WOBY_IMPORT_MAX_HIERARCHY_DEPTH - 2u, 0});
    WobyImportHierarchy hierarchy{sizeof(WobyImportHierarchy), nodes.data(), static_cast<uint32_t>(nodes.size())};
    const auto mesh = woby::copyImportedMesh(result, &hierarchy);
    CHECK(mesh.nodes[0].name == "Mesh");
    CHECK(woby::meshNodeDisplayName(mesh.nodes[0]) == "Surface");
    nodes.back().group_index = WOBY_IMPORT_NO_GROUP;
    nodes.push_back({"Too deep", WOBY_IMPORT_MAX_HIERARCHY_DEPTH - 1u, 0});
    hierarchy.nodes = nodes.data(); hierarchy.node_count = static_cast<uint32_t>(nodes.size());
    CHECK_THROWS_WITH_AS((void)woby::copyImportedMesh(result, &hierarchy), doctest::Contains("depth"), std::runtime_error);

    const std::string longName(4096, 'x');
    nodes = {{longName.c_str(), WOBY_IMPORT_NO_PARENT, 0}};
    hierarchy.nodes = nodes.data(); hierarchy.node_count = 1;
    CHECK_THROWS_WITH_AS((void)woby::copyImportedMesh(result, &hierarchy), doctest::Contains("string exceeds"), std::runtime_error);
    const std::string label(4095, 'x');
    nodes.assign(258, {label.c_str(), WOBY_IMPORT_NO_PARENT, WOBY_IMPORT_NO_GROUP});
    nodes.back().group_index = 0;
    hierarchy.nodes = nodes.data(); hierarchy.node_count = static_cast<uint32_t>(nodes.size());
    CHECK_THROWS_WITH_AS((void)woby::copyImportedMesh(result, &hierarchy), doctest::Contains("labels"), std::runtime_error);
    // Empty containers and root-level leaves are legal; no fabricated geometry.
    nodes = {{"Empty", WOBY_IMPORT_NO_PARENT, WOBY_IMPORT_NO_GROUP}, {"Surface", WOBY_IMPORT_NO_PARENT, 0}};
    hierarchy.nodes = nodes.data(); hierarchy.node_count = 2;
    CHECK(woby::copyImportedMesh(result, &hierarchy).hierarchy.size() == 2);
}

TEST_CASE("Extended importer copies hierarchy before release and retains flat ABI compatibility")
{
    ImporterTestScope scope;
    woby::loadImporter(WOBY_TEST_IMPORTER);
    woby::loadImporter(WOBY_TEST_HIERARCHY_IMPORTER);
    const auto legacyPath = scope.root / "legacy.wtest";
    const auto path = scope.root / "assembly.whier";
    writeFile(legacyPath); writeFile(path);
    const auto legacy = woby::loadModel(legacyPath);
    CHECK(legacy.mesh.hierarchy.empty());
    CHECK(legacy.mesh.nodes[0].displayName.empty());
    const auto oldTree = woby::createFileSceneNode(woby::createUiFileState(legacyPath, legacy.mesh, 0), 0);
    REQUIRE(oldTree.children.size() == 1);
    CHECK(oldTree.children[0].kind == woby::UiSceneNodeKind::group);
    for (const auto* mode : {"invalid", "failure", "cancel", "flat"}) {
        const auto testPath = scope.root / (std::string(mode) + ".whier");
        writeFile(testPath);
        if (std::string(mode) == "cancel") {
            bool canceled = false;
            woby::ImportCallbacks callbacks;
            callbacks.canceled = [&] { return canceled; };
            callbacks.progress = [&](float) { canceled = true; };
            CHECK(woby::importModel(testPath, {}, callbacks).canceled);
        } else if (std::string(mode) == "flat") {
            CHECK(woby::loadModel(testPath).mesh.hierarchy.empty());
        } else {
            CHECK_THROWS_AS((void)woby::loadModel(testPath), std::runtime_error);
        }
        // The DLL refuses the next import if the previous result leaked.
        const auto imported = woby::loadModel(path);
        REQUIRE(imported.mesh.hierarchy.size() == 7);
        CHECK(imported.mesh.hierarchy[0].name == "Assembly");
        CHECK(imported.mesh.nodes[0].name == "solid-a/patch-1");
        CHECK(imported.mesh.nodes[0].displayName == "Patch 1");
        CHECK(imported.mesh.nodes[2].displayName == "Patch 1");
    }
}

TEST_CASE("Imported nested parents support child overrides scene round trips views and history")
{
    ImporterTestScope scope;
    woby::loadImporter(WOBY_TEST_HIERARCHY_IMPORTER);
    const auto path = scope.root / "assembly.whier";
    writeFile(path);
    auto loaded = woby::loadModelBatchCpu({path}, 0, {}, {});
    REQUIRE(loaded.files.size() == 1);
    woby::UiState state;
    state.files = std::move(loaded.files);
    woby::appendDefaultSceneNodesForFiles(state, 0);
    const auto clean = woby::createSceneDocument(state);
    const auto format = [](woby::SceneObjectId id) { return std::to_string(id); };
    auto& fileNode = state.sceneNodes[0];
    REQUIRE(fileNode.children.size() == 1);
    auto& assembly = fileNode.children[0];
    REQUIRE(assembly.children.size() == 2);
    auto& solidA = assembly.children[0];
    auto& solidB = assembly.children[1];
    REQUIRE(solidA.children.size() == 2);
    const auto solidId = solidA.objectId;
    auto& file = state.files[0];
    auto& groups = file.groupSettings;
    CHECK(solidA.kind == woby::UiSceneNodeKind::folder);
    CHECK(solidA.settings.center == groups[0].localBounds.center);
    CHECK(solidA.children[0].name == solidB.children[0].name);
    CHECK(solidA.children[0].objectId != solidB.children[0].objectId);
    CHECK(woby::comparisonObjectParts(state, {solidId}) == std::vector<woby::SceneObjectId>{groups[0].objectId, groups[1].objectId});
    CHECK(woby::countSceneNodeGroups(state, assembly) == 4);
    CHECK(woby::countVisibleSceneNodeGroups(state, assembly) == 3);

    woby::SceneHistory history;
    woby::resetSceneHistory(history, state);
    woby::selectSceneObject(state, solidId);
    woby::setSelectedObjectsVisible(state, false);
    CHECK_FALSE(groups[0].visible); CHECK_FALSE(groups[1].visible);
    CHECK(groups[2].visible); CHECK(groups[3].visible);
    CHECK_FALSE(solidA.settings.visible); CHECK(assembly.settings.visible);
    woby::setSelectedObjectsVisible(state, true);
    woby::setSelectedObjectProperty(state, woby::UiObjectProperty::red, 0.6f);
    CHECK(groups[0].color[0] == 0.6f); CHECK(groups[1].color[0] == 0.6f);
    CHECK(groups[2].color[0] == 0.2f);
    woby::selectSceneObject(state, groups[1].objectId);
    woby::setSelectedObjectsVisible(state, false);
    woby::setSelectedObjectProperty(state, woby::UiObjectProperty::red, 0.1f);
    woby::selectSceneObject(state, solidId);
    CHECK(woby::selectedObjectProperty(state, woby::UiObjectProperty::red).mixed);
    woby::setSelectedObjectProperty(state, woby::UiObjectProperty::translationX, 5.0f);
    const auto tree = woby::controlSceneTree(state, format);
    CHECK(tree[0]["parentId"].is_null());
    CHECK(tree[0]["children"][0]["parentId"] == format(file.objectId));
    const auto details = woby::controlObjectDetails(state, groups[0].objectId, format);
    REQUIRE(details.contains("occurrences"));
    REQUIRE(details.at("occurrences").size() == 1);
    CHECK(woby::findSceneObject(state, groups[0].objectId)->name == "Patch 1");
    CHECK(details["occurrences"][0]["parentId"] == format(solidId));
    CHECK(details["occurrences"][0]["effective"]["worldMatrix"][12].get<float>() == doctest::Approx(5));
    const auto view = woby::createView(state);
    const auto saved = woby::createSceneDocument(state);
    REQUIRE(woby::recordSceneHistory(history, state));
    auto undo = woby::prepareSceneHistoryStep(history, state, clean, false);
    REQUIRE(undo);
    woby::commitSceneHistoryStep(history, state, std::move(*undo), false);
    CHECK(woby::createSceneDocument(state) == clean);
    auto redo = woby::prepareSceneHistoryStep(history, state, clean, true);
    REQUIRE(redo);
    woby::commitSceneHistoryStep(history, state, std::move(*redo), true);
    CHECK(woby::createSceneDocument(state) == saved);
    woby::selectSceneObject(state, solidId);
    woby::setSelectedObjectsVisible(state, false);
    woby::setSelectedObjectProperty(state, woby::UiObjectProperty::red, 0.8f);
    woby::applyView(state, view);
    CHECK(woby::createSceneDocument(state) == saved);

    const auto scenePath = scope.root / "assembly.woby";
    woby::writeSceneDocument(scenePath, saved);
    auto reopened = woby::loadSceneCpu(scenePath, {}, {});
    auto restored = woby::prepareSceneReplacement(state, std::move(reopened.files), reopened.document);
    CHECK(woby::createSceneDocument(restored) == saved);
    CHECK(restored.files[0].groupSettings[0].color[0] == 0.6f);
    CHECK(restored.files[0].groupSettings[1].color[0] == 0.1f);
    CHECK_FALSE(restored.files[0].groupSettings[1].visible);
    CHECK(restored.sceneNodes[0].children[0].children[0].settings.translation[0] == 5.0f);
    // Previously saved flat trees stay authoritative even if a plugin now supplies hierarchy.
    auto flat = saved;
    flat.nodes.resize(1);
    for (size_t i = 0; i < flat.files[0].groups.size(); ++i) {
        woby::SceneNodeRecord leaf;
        leaf.kind = woby::SceneNodeKind::group; leaf.name = flat.files[0].groups[i].name;
        leaf.parentIndex = 0; leaf.fileIndex = 0; leaf.groupIndex = static_cast<int>(i);
        flat.nodes.push_back(leaf);
    }
    woby::writeSceneDocument(scenePath, flat);
    reopened = woby::loadSceneCpu(scenePath, {}, {});
    restored = woby::prepareSceneReplacement(state, std::move(reopened.files), reopened.document);
    REQUIRE(restored.sceneNodes[0].children.size() == 4);
    CHECK(restored.sceneNodes[0].children[0].kind == woby::UiSceneNodeKind::group);
    CHECK(restored.files[0].groupSettings[1].color[0] == 0.1f);
}

TEST_CASE("Imported hidden hierarchy initializes ancestors and folder imports survive file removal")
{
    ImporterTestScope scope;
    woby::loadImporter(WOBY_TEST_HIERARCHY_IMPORTER);
    const auto first = scope.root / "hidden.whier", second = scope.root / "visible.whier";
    writeFile(first); writeFile(second);
    auto loaded = woby::loadModelBatchCpu({first, second}, 0, {}, {});
    REQUIRE(loaded.files.size() == 2);
    woby::UiState state;
    state.files = std::move(loaded.files);
    woby::appendFolderTreeSceneNode(state, scope.root, 0, 2);
    REQUIRE(state.sceneNodes.size() == 1);
    auto& root = state.sceneNodes[0];
    REQUIRE(root.children.size() == 2);
    CHECK_FALSE(root.children[0].children[0].settings.visible);
    CHECK_FALSE(root.children[0].children[0].children[0].settings.visible);
    const auto survivor = state.files[1].groupSettings[0].objectId;
    REQUIRE(woby::removeFileFromState(state, 0));
    REQUIRE(state.files.size() == 1);
    REQUIRE(state.sceneNodes[0].children.size() == 1);
    const auto& leaf = state.sceneNodes[0].children[0].children[0].children[0].children[0];
    CHECK(leaf.fileIndex == 0);
    CHECK(leaf.objectId == survivor);
    REQUIRE(woby::removeFileFromState(state, 0));
    CHECK(state.sceneNodes.empty());
}

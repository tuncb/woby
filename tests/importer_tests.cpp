#include "utf8_path.h"
#include "background_load.h"
#include "file_discovery.h"
#include "importer_host.h"
#include "model_load.h"
#include "control_importers.h"

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
    std::filesystem::path root = std::filesystem::temp_directory_path() / ("woby_importer_tests_"
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
    CHECK(model.mesh.vertices.size() == 3u);
    CHECK(model.mesh.indices.size() == 3u);
    REQUIRE(model.mesh.nodes.size() == 1u);
    CHECK(model.mesh.nodes[0].name == "triangle");
    CHECK(model.mesh.bounds.max[0] == 1.0f);
    for (const auto& vertex : model.mesh.vertices) { CHECK(woby::validNormal(vertex.normal)); }
    woby::unloadImporters();
    CHECK_FALSE(woby::isModelPath(path));
    CHECK(model.mesh.vertices.size() == 3u);
    CHECK_THROWS_WITH_AS((void)woby::loadModel(path, "org.woby.test"), doctest::Contains("Required importer is not loaded"), std::runtime_error);
}

TEST_CASE("DLL importer rejects malformed outputs and releases every result")
{
    ImporterTestScope scope;
    woby::loadImporter(WOBY_TEST_IMPORTER);
    for (const auto* name : {"invalid_index", "nan_position", "invalid_group", "null_buffer", "huge_buffer", "short_result", "failure"}) {
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
    WobyImportGroup groups[] = {{"front", 0, 3}, {"back", 3, 3}};
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
}

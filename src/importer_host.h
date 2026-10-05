#pragma once

#include "model_mesh.h"
#include "woby/importer.h"

#include <exception>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace woby {

struct ImporterInfo {
    std::filesystem::path path;
    std::string id;
    std::string name;
    std::string version;
    std::vector<std::string> extensions;
};

struct ImportCallbacks {
    std::function<bool()> canceled;
    std::function<void(float)> progress;
    ModelLoadProgressCallback stageProgress = {};
};

struct ImportedModel {
    Mesh mesh;
    std::string importerId;
    bool canceled = false;
};

struct ImporterLoadFailure {
    std::filesystem::path path;
    // Brief UI reason and original diagnostic for stderr/file logging.
    std::string reason;
    std::string details;
};

[[nodiscard]] ImporterLoadFailure importerLoadFailure(const std::filesystem::path& path,
    const std::exception& error);
[[nodiscard]] std::string importerLoadFailureSummary(const std::vector<ImporterLoadFailure>& failures);

// Runtime-only registry. Calls retain the library and serialize work per importer.
void loadImporter(const std::filesystem::path& path);
void unloadImporters();
[[nodiscard]] std::vector<ImporterInfo> loadedImporters();
[[nodiscard]] std::vector<std::filesystem::path> discoverImporterFiles(const std::filesystem::path& folder);
// Load importer.json from sorted immediate package folders. Missing folders are optional;
// failures are returned per package so other importers can still load.
[[nodiscard]] std::vector<ImporterLoadFailure> loadPortableImporters(const std::filesystem::path& folder);
[[nodiscard]] bool hasImporterForPath(const std::filesystem::path& path);
[[nodiscard]] ImportedModel importModel(
    const std::filesystem::path& path,
    const std::string& requiredImporterId,
    const ImportCallbacks& callbacks);
[[nodiscard]] Mesh copyImportedMesh(const WobyImportResult& result, const WobyImportHierarchy* hierarchy = nullptr,
    const WobyImportLines* lines = nullptr, const WobyImportPointIds* pointIds = nullptr,
    const WobyImportPoints* points = nullptr, const WobyImportFreeform* freeform = nullptr,
    const ModelLoadProgressCallback& progress = {});
[[nodiscard]] std::vector<std::filesystem::path> readImporterSettings(const std::filesystem::path& path);
void writeImporterSettings(const std::filesystem::path& path, const std::vector<std::filesystem::path>& plugins);

} // namespace woby

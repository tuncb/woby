#pragma once

#include "model_mesh.h"
#include "woby/importer.h"

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
};

struct ImportedModel {
    Mesh mesh;
    std::string importerId;
    bool canceled = false;
};

// Runtime-only registry. Calls retain the library and serialize work per importer.
void loadImporter(const std::filesystem::path& path);
void unloadImporters();
[[nodiscard]] std::vector<ImporterInfo> loadedImporters();
[[nodiscard]] std::vector<std::filesystem::path> discoverImporterFiles(const std::filesystem::path& folder);
[[nodiscard]] bool hasImporterForPath(const std::filesystem::path& path);
[[nodiscard]] ImportedModel importModel(
    const std::filesystem::path& path,
    const std::string& requiredImporterId,
    const ImportCallbacks& callbacks);
[[nodiscard]] Mesh copyImportedMesh(const WobyImportResult& result);
[[nodiscard]] std::vector<std::filesystem::path> readImporterSettings(const std::filesystem::path& path);
void writeImporterSettings(const std::filesystem::path& path, const std::vector<std::filesystem::path>& plugins);

} // namespace woby

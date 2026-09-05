#pragma once

#include "importer_host.h"

#include <filesystem>

namespace woby {

[[nodiscard]] ImportedModel loadModel(
    const std::filesystem::path& path,
    const std::string& requiredImporterId = {},
    const ImportCallbacks& callbacks = {});

[[nodiscard]] Mesh loadModelMesh(const std::filesystem::path& path);

} // namespace woby

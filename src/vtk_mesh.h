#pragma once
#include "model_mesh.h"
#include <filesystem>
#include <istream>

namespace woby {
// Limited, dependency-free legacy ASCII UNSTRUCTURED_GRID reader.
[[nodiscard]] Mesh loadVtkMesh(std::istream& input, const ModelLoadProgressCallback& progress = {}, bool deferPreparation = false);
[[nodiscard]] Mesh loadVtkMesh(const std::filesystem::path& path, const ModelLoadProgressCallback& progress = {}, bool deferPreparation = false);
}

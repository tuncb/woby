#pragma once

#include "model_mesh.h"

#include <filesystem>
#include <string_view>

namespace woby {

[[nodiscard]] Mesh loadObjMesh(const std::filesystem::path& path, const ModelLoadProgressCallback& progress = {});
// Polygonal OBJ held in memory; external material libraries and freeform
// statements are not loaded. Uses the same mesh construction as file import.
[[nodiscard]] Mesh loadObjMeshText(std::string_view text, const ModelLoadProgressCallback& progress = {});

} // namespace woby

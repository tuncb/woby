#pragma once

#include "model_mesh.h"

#include <filesystem>

namespace woby {

[[nodiscard]] Mesh loadObjMesh(const std::filesystem::path& path, const ModelLoadProgressCallback& progress = {});

} // namespace woby

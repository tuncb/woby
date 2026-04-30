#pragma once

#include "model_mesh.h"

#include <filesystem>

namespace woby {

[[nodiscard]] Mesh loadStlMesh(const std::filesystem::path& path);

} // namespace woby

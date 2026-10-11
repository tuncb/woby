#pragma once
#include "model_mesh.h"
#include <filesystem>
#include <stop_token>

namespace woby
{
struct DifferenceSettings {
    bool created = false, visible = false, showA = true, showB = true, showWalls = true;
    float opacity = .85f;
    friend bool operator==(const DifferenceSettings &, const DifferenceSettings &) = default;
};
struct DifferenceMesh {
    // Solids: A only, B only. Open surfaces: gap surface, empty, closure walls.
    std::array<Mesh, 3> parts;
    bool constructed = false;
    std::array<double, 2> volumes{};
    size_t closureTriangles = 0;
    std::string message;
};
[[nodiscard]] DifferenceMesh buildDifferenceMesh(const Mesh &a, const Mesh &b, std::stop_token stop = {});
// Export all parts, independent of display visibility/opacity. Existing files are rejected.
void exportDifferenceMesh(const DifferenceMesh &result, const std::filesystem::path &path, std::stop_token stop = {});
} // namespace woby

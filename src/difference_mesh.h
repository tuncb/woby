#pragma once
#include "model_mesh.h"
#include <filesystem>
#include <stop_token>

namespace woby
{
enum class DifferenceMethod { direct, approximate };
struct DifferenceSettings {
    bool created = false, visible = false, showA = true, showB = true, showWalls = true;
    float opacity = .85f;
    DifferenceMethod method = DifferenceMethod::direct;
    double cellSize = .5;
    friend bool operator==(const DifferenceSettings &, const DifferenceSettings &) = default;
};
[[nodiscard]] bool sameDifferenceConstruction(const DifferenceSettings &a, const DifferenceSettings &b);
[[nodiscard]] const char *differenceMethodName(DifferenceMethod method);
[[nodiscard]] DifferenceMethod parseDifferenceMethod(const std::string &name);
struct DifferenceMesh {
    // Solids: A only, B only. Open surfaces: gap surface, empty, closure walls.
    std::array<Mesh, 3> parts;
    bool constructed = false;
    std::array<double, 2> volumes{};
    // Exact zero-area input faces omitted from the calculation, in A/B order.
    std::array<size_t, 2> removedCollapsedTriangles{};
    size_t closureTriangles = 0;
    std::string message;
    DifferenceSettings construction;
};
[[nodiscard]] DifferenceMesh buildDifferenceMesh(const Mesh &a, const Mesh &b, std::stop_token stop = {});
[[nodiscard]] DifferenceMesh buildDifferenceMesh(const Mesh &a, const Mesh &b, const DifferenceSettings &settings,
                                                std::stop_token stop = {});
// Export all parts, independent of display visibility/opacity. Existing files are rejected.
void exportDifferenceMesh(const DifferenceMesh &result, const std::filesystem::path &path, std::stop_token stop = {});
} // namespace woby

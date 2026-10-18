#pragma once
#include "model_mesh.h"
#include <filesystem>
#include <stop_token>

namespace woby
{
struct DifferenceSettings {
    bool created = false, showA = true, showB = true;
    float opacity = .85f;
    friend bool operator==(const DifferenceSettings &, const DifferenceSettings &) = default;
};
struct DifferenceMesh {
    // Approximate closed solids: A only, B only.
    // Non-empty parts retain input A's origin for comparison display and picking.
    std::array<Mesh, 2> parts;
    std::array<double, 2> volumes{};
    // Exact zero-area input faces omitted from the calculation, in A/B order.
    std::array<size_t, 2> removedCollapsedTriangles{};
    std::string message;
};
[[nodiscard]] DifferenceMesh buildDifferenceMesh(const Mesh &a, const Mesh &b, std::stop_token stop = {});
// Export all parts, independent of display visibility/opacity. Existing files are rejected.
void exportDifferenceMesh(const DifferenceMesh &result, const std::filesystem::path &path, std::stop_token stop = {});
} // namespace woby

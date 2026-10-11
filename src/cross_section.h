#pragma once
#include "scene_pick.h"
#include <stop_token>

namespace woby {
struct CrossSectionContour {
    SceneObjectId source = 0;
    bool missing = false;
    std::vector<DiagnosticEdge> lines;
    std::array<float, 2> minimum{}, maximum{};
};
struct CrossSectionResult {
    std::vector<CrossSectionContour> contours;
    std::optional<DiagnosticEdge> largestGap;
    double gap = 0;
};
[[nodiscard]] CrossSectionResult cutCrossSection(const CrossSectionSettings& settings,
                                                 std::span<const SceneObjectId> sources,
                                                 std::span<const ScenePickPart> parts, std::stop_token stop = {});
[[nodiscard]] std::array<float, 3> sectionDirection(const CrossSectionSettings& settings);
// Constrain an endpoint to the nearest 45-degree ray in the horizontal workplane.
[[nodiscard]] std::array<float, 3> snapSectionEndpoint(const std::array<float, 3>& anchor,
    const std::array<float, 3>& point, SceneUpAxis upAxis);
[[nodiscard]] std::array<float, 3> sectionNormal(const CrossSectionSettings& settings);
[[nodiscard]] std::array<float, 2> sectionCoordinates(const CrossSectionSettings& settings,
                                                      const std::array<float, 3>& point);
[[nodiscard]] std::array<float, 4> sectionSourceColor(size_t index);
[[nodiscard]] std::vector<DiagnosticEdge> sectionGuides(const CrossSectionSettings& settings);
[[nodiscard]] std::optional<std::array<float, 3>> sectionWorkplanePoint(const ScenePickView& view, PickPoint point,
                                                                        SceneUpAxis up, float height);
} // namespace woby

#pragma once
#include "scene_pick.h"
#include <stop_token>

namespace woby {
struct CrossSectionEdge { Coordinate a{}, b{}; };
struct CrossSectionContour {
    SceneObjectId source = 0;
    bool missing = false;
    std::vector<CrossSectionEdge> lines;
    std::array<double, 2> minimum{}, maximum{};
};
struct CrossSectionResult {
    std::vector<CrossSectionContour> contours;
    std::optional<CrossSectionEdge> largestGap;
    double gap = 0;
};
// Snapshot the model-tree eye states, including hidden ancestor folders/files.
[[nodiscard]] std::vector<bool> crossSectionSourceVisibility(const UiState& state,
    std::span<const SceneObjectId> sources);
[[nodiscard]] CrossSectionResult cutCrossSection(const CrossSectionSettings& settings,
                                                 std::span<const SceneObjectId> sources,
                                                 std::span<const ScenePickPart> parts, std::stop_token stop = {},
                                                 std::span<const ComparisonSide> sides = {});
// A selected analysis (or its section) is the fallback when a gesture misses all views.
[[nodiscard]] SceneObjectId crossSectionAnalysisScope(const UiState& state);
struct CrossSectionDrawTarget {
    SceneObjectId analysis = 0; // Zero identifies the model view.
    std::array<std::array<double, 3>, 3> screen;
};
// Project coarse view bounds once per gesture; no mesh triangles are scanned.
[[nodiscard]] std::vector<CrossSectionDrawTarget> crossSectionDrawTargets(const UiState& state, const ScenePickView& view);
// The first view crossed by the line wins; depth resolves overlapping views.
[[nodiscard]] std::optional<SceneObjectId> crossSectionDrawScope(std::span<const CrossSectionDrawTarget> targets,
    PickPoint start, PickPoint end);
[[nodiscard]] UiAnnotation comparisonSectionDefinition(const UiState& state, SceneObjectId analysisId);
void validateSectionAnalysis(const SceneAnnotationRecord& record, size_t comparisonCount);
[[nodiscard]] std::array<float, 3> sectionDirection(const CrossSectionSettings& settings);
// Constrain an endpoint to the nearest 45-degree ray in the horizontal workplane.
[[nodiscard]] std::array<float, 3> snapSectionEndpoint(const std::array<float, 3>& anchor,
    const std::array<float, 3>& point, SceneUpAxis upAxis);
[[nodiscard]] std::array<float, 3> sectionNormal(const CrossSectionSettings& settings);
[[nodiscard]] std::array<double, 2> sectionCoordinates(const CrossSectionSettings& settings,
                                                       const Coordinate& point);
[[nodiscard]] std::array<float, 4> sectionSourceColor(size_t index);
[[nodiscard]] std::array<float, 4> sectionSourceColor(const CrossSectionSettings& settings, size_t index);
// Edit captured source indices, including hidden items. Validate before changing any value.
bool editCrossSectionSources(CrossSectionSettings& settings, std::span<const size_t> indices,
    std::optional<bool> visible, std::optional<std::array<float, 3>> color);
[[nodiscard]] std::vector<DiagnosticEdge> sectionGuides(const CrossSectionSettings& settings);
[[nodiscard]] std::optional<std::array<float, 3>> sectionWorkplanePoint(const ScenePickView& view, PickPoint point,
                                                                        SceneUpAxis up, float height);
} // namespace woby

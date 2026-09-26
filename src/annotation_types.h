#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace woby {

enum class AnnotationShape { line, rectangle };

// Preserve the near-plane offset when composing distant camera transforms.
using AnnotationProjector = std::array<double, 16>;

// Surface segments use one triangle. Bridges anchor their end on a second
// source-part triangle, spanning the empty space between two surface rims.
struct AnnotationSegment {
    uint32_t triangle = 0;
    std::array<float, 3> a{}, b{};
    std::optional<uint32_t> endTriangle;
    uint32_t source = 0;
    std::optional<uint32_t> endSource;
    friend bool operator==(const AnnotationSegment&, const AnnotationSegment&) = default;
};

struct AnnotationSettings {
    std::string name = "Annotation", note;
    bool visible = true, locked = false;
    float width = 3.0f;
    std::array<float, 4> color{0.15f, 0.9f, 1.0f, 1.0f};
    friend bool operator==(const AnnotationSettings&, const AnnotationSettings&) = default;
};

struct AnnotationSource {
    AnnotationProjector projector{};
    std::string fingerprint;
    // Frozen source-local to primary-local transform, independent of camera depth
    // precision. Used when validating joins again during editing.
    std::array<float, 16> toPrimary{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    friend bool operator==(const AnnotationSource&, const AnnotationSource&) = default;
};

struct AnnotationGeometry {
    AnnotationShape shape = AnnotationShape::line;
    // Frozen source-local to clip transform and normalized drawing coordinates.
    AnnotationProjector projector{};
    std::array<float, 2> start{}, end{};
    bool homogeneousDepth = false;
    std::string fingerprint;
    std::vector<AnnotationSegment> segments;
    // Empty for legacy single-part annotations; otherwise source zero is the
    // primary target and every segment identifies its own source part.
    std::vector<AnnotationSource> sources;
    friend bool operator==(const AnnotationGeometry&, const AnnotationGeometry&) = default;
};

struct SceneAnnotationTarget {
    int fileIndex = -1, groupIndex = -1;
    friend bool operator==(const SceneAnnotationTarget&, const SceneAnnotationTarget&) = default;
};

struct SceneAnnotationRecord {
    int fileIndex = -1, groupIndex = -1;
    std::string targetName;
    AnnotationSettings settings;
    AnnotationGeometry geometry;
    std::vector<SceneAnnotationTarget> targets;
    friend bool operator==(const SceneAnnotationRecord&, const SceneAnnotationRecord&) = default;
};

// Reject malformed geometry; normalize editable style at operation/load boundaries.
void validateAnnotationGeometry(const AnnotationGeometry& geometry);
[[nodiscard]] AnnotationSettings normalizedAnnotationSettings(AnnotationSettings settings);

} // namespace woby

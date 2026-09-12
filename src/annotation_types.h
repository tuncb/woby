#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace woby {

enum class AnnotationShape { line, rectangle };

// Both endpoints belong to this source-part triangle. No chord crosses faces.
struct AnnotationSegment {
    uint32_t triangle = 0;
    std::array<float, 3> a{}, b{};
    friend bool operator==(const AnnotationSegment&, const AnnotationSegment&) = default;
};

struct AnnotationSettings {
    std::string name = "Annotation", note;
    bool visible = true, locked = false;
    float width = 3.0f;
    std::array<float, 4> color{0.15f, 0.9f, 1.0f, 1.0f};
    friend bool operator==(const AnnotationSettings&, const AnnotationSettings&) = default;
};

struct AnnotationGeometry {
    AnnotationShape shape = AnnotationShape::line;
    // Frozen source-local to clip transform and normalized drawing coordinates.
    std::array<float, 16> projector{};
    std::array<float, 2> start{}, end{};
    bool homogeneousDepth = false;
    std::string fingerprint;
    std::vector<AnnotationSegment> segments;
    friend bool operator==(const AnnotationGeometry&, const AnnotationGeometry&) = default;
};

struct SceneAnnotationRecord {
    int fileIndex = -1, groupIndex = -1;
    std::string targetName;
    AnnotationSettings settings;
    AnnotationGeometry geometry;
    friend bool operator==(const SceneAnnotationRecord&, const SceneAnnotationRecord&) = default;
};

// Reject malformed geometry; normalize editable style at operation/load boundaries.
void validateAnnotationGeometry(const AnnotationGeometry& geometry);
[[nodiscard]] AnnotationSettings normalizedAnnotationSettings(AnnotationSettings settings);

} // namespace woby

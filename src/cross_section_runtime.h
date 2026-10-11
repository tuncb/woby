#pragma once
#include "cross_section.h"
namespace woby {
struct SceneQueryRuntime;
struct CrossSectionRuntime;
// Polls an owned worker snapshot; publishes only matching source/plane results.
bool updateCrossSections(SceneQueryRuntime& runtime, const UiState& state);
[[nodiscard]] std::shared_ptr<const CrossSectionResult> crossSectionResult(const SceneQueryRuntime& runtime,
                                                                           SceneObjectId id);
[[nodiscard]] std::string crossSectionError(const SceneQueryRuntime& runtime);
} // namespace woby

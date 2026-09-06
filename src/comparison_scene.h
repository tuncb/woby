#pragma once

#include "ui_state.h"

namespace woby
{

// Group comparison uses the same hierarchy transforms as the renderer,
// independently of ordinary scene visibility and appearance settings.
[[nodiscard]] Mesh comparisonWorldMesh(const UiState &state, ComparisonSide side);
[[nodiscard]] uint64_t comparisonGeometrySignature(const UiState &state);

} // namespace woby

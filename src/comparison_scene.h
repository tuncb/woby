#pragma once

#include "ui_state.h"

namespace woby
{

// Whole-file comparison uses the same hierarchy transforms as the renderer,
// independently of ordinary scene visibility and appearance settings.
[[nodiscard]] Mesh comparisonWorldMesh(const UiState &state, size_t fileIndex);
[[nodiscard]] uint64_t comparisonGeometrySignature(const UiState &state);

} // namespace woby

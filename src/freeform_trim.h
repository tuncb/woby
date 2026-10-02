#pragma once
#include "freeform.h"

namespace woby {
// Validates trimming and replaces the grid's implicit rectangular topology with
// deterministic UV samples and local triangles. Logical source data is immutable.
void triangulateFreeformTrim(const FreeformPatch& patch, FreeformGrid& grid,
    const ModelLoadProgressCallback& progress = {});
} // namespace woby

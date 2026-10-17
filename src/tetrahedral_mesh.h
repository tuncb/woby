#pragma once
#include "model_mesh.h"

namespace woby {
// Validate buffers and append exterior and ambiguous faces to existing groups.
// Keep bad cells for inspection. Remove only unambiguous internal face pairs.
// Positions must be original doubles. Run during loading, before localization.
void appendTetrahedralBoundary(MeshGeometry& geometry, const ModelLoadProgressCallback& progress = {});
}

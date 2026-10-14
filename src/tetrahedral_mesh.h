#pragma once
#include "model_mesh.h"

namespace woby {
// Validate cells and append their exterior triangles to their existing groups.
// Positions must be original doubles. Run during loading, before localization.
void appendTetrahedralBoundary(MeshGeometry& geometry, const ModelLoadProgressCallback& progress = {});
}

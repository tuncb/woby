#pragma once
#include "freeform.h"
#include "graphics.h"

namespace woby {
// Header, control net, and separable basis tables. Empty means the source needs
// the double-precision CPU fallback (not representable safely in GPU floats).
[[nodiscard]] std::vector<float> packFreeformGpu(const FreeformPatch& patch,
    const FreeformGrid& grid, const Coordinate& origin);
void dispatchFreeformGpu(const Mesh& mesh, graphics::VertexBufferHandle vertices,
    graphics::IndexBufferHandle triangles, graphics::IndexBufferHandle lines,
    graphics::ProgramHandle program, graphics::ViewId view);
} // namespace woby

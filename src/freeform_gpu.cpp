#include "freeform_gpu.h"
#include <algorithm>
#include <bit>
#include <cmath>

namespace woby {
std::vector<float> packFreeformGpu(const FreeformPatch& patch, const FreeformGrid& grid, const Coordinate& origin)
{
    validateFreeformPatch(patch);
    // Layout shared with cs_freeform: 16-word header, 9 words per control,
    // 19 per basis sample (first control index, 9 values, 9 derivatives).
    std::vector<float> data(16);
    const auto header = [&](size_t slot, size_t value) { data[slot] = std::bit_cast<float>(static_cast<uint32_t>(value)); };
    header(0,patch.degreeU); header(1,patch.degreeV); header(2,patch.countU); header(3,patch.surface);
    header(4,grid.u.size()); header(5,grid.v.size()); header(6,grid.vertexOffset); header(7,grid.indexOffset);
    header(8,data.size()); header(11,!patch.texcoords.empty()); header(12,!patch.normals.empty());
    double scale = 0;
    for (const auto& p : patch.controls) { scale = std::max(scale,p[3]); }
    bool safe = true;
    const auto value = [&](double d) {
        const float f = static_cast<float>(d);
        // Leave headroom for products, derivative sums and division in float.
        safe = safe && std::isfinite(f) && std::abs(d) <= 1e12 && (d == 0 || std::abs(f) >= 1e-30f);
        data.push_back(f);
    };
    for (size_t i = 0; i < patch.controls.size(); ++i) {
        const auto& p = patch.controls[i];
        for (size_t k = 0; k < 3; ++k) { value(p[k]-origin[k]); }
        safe = safe && p[3]/scale >= 1e-12;
        value(p[3]/scale);
        for (size_t k = 0; k < 2; ++k) { value(patch.texcoords.empty() ? 0 : patch.texcoords[i][k]); }
        for (size_t k = 0; k < 3; ++k) { value(patch.normals.empty() ? 0 : patch.normals[i][k]); }
    }
    const auto axis = [&](const std::vector<double>& parameters, bool v) {
        header(v ? 10 : 9,data.size());
        for (double t : parameters) {
            FreeformBasis b;
            b.values[0] = 1;
            double extent = 1;
            if (!v || patch.surface) {
                b = freeformBasis(v ? patch.knotsV : patch.knotsU, v ? patch.countV : patch.countU,
                    v ? patch.degreeV : patch.degreeU, t);
                const auto& domain = v ? patch.domainV : patch.domainU;
                extent = domain[1]-domain[0];
            }
            data.push_back(std::bit_cast<float>(static_cast<uint32_t>(b.first)));
            for (double a : b.values) { value(a); }
            // A positive axis rescale preserves normal direction and avoids
            // precision loss from large absolute knot values on the GPU.
            for (double a : b.derivatives) { value(a*extent); }
        }
    };
    axis(grid.u,false); axis(grid.v,true);
    return safe ? data : std::vector<float>{};
}

void dispatchFreeformGpu(const Mesh& mesh, graphics::VertexBufferHandle vertices,
    graphics::IndexBufferHandle triangles, graphics::IndexBufferHandle lines,
    graphics::ProgramHandle program, graphics::ViewId view)
{
    if (!mesh.freeform) { return; }
    for (size_t i = 0; i < mesh.freeform->patches.size(); ++i) {
        const auto& patch = mesh.freeform->patches[i];
        const auto& grid = mesh.freeform->grids[i];
        const auto data = packFreeformGpu(patch,grid,mesh.origin);
        if (data.empty()) { continue; }
        const auto input = graphics::createVertexBuffer(graphics::copy(data.data(),static_cast<uint32_t>(data.size()*sizeof(float))),
            {4},WOBY_GPU_BUFFER_COMPUTE_READ);
        try {
            graphics::setBuffer(0,vertices,graphics::Access::ReadWrite);
            graphics::setBuffer(1,patch.surface ? triangles : lines,graphics::Access::Write);
            graphics::setBuffer(2,input,graphics::Access::Read);
            graphics::dispatch(view,program,static_cast<uint32_t>((grid.u.size()*grid.v.size()+63)/64));
        } catch (...) { graphics::destroy(input); throw; }
        // Recorded dispatches retain their resources through GPU completion.
        graphics::destroy(input);
    }
}
} // namespace woby

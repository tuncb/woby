#include "probe.h"
#include <rapidobj/rapidobj.hpp>

namespace obj_probe {
Json rapid(const std::filesystem::path& path)
{
    const auto begin = Clock::now();
    auto result = rapidobj::ParseFile(path, rapidobj::MaterialLibrary::Default(rapidobj::Load::Optional));
    const auto ms = elapsed(begin);
    if (result.error) { throw std::runtime_error(result.error.code.message() + " line " + std::to_string(result.error.line_num)); }
    Summary s;
    s.normals = result.attributes.normals.size() / 3;
    s.texcoords = result.attributes.texcoords.size() / 2;
    for (size_t i = 0; i < result.attributes.positions.size(); i += 3) { point(s, &result.attributes.positions[i]); }
    for (const auto& shape : result.shapes) {
        for (const auto n : shape.mesh.num_face_vertices) { face(s, n); }
        for (const auto& v : shape.mesh.indices) { index(s, v.position_index, v.normal_index, v.texcoord_index); }
    }
    return {{"load_ms", ms}, {"geometry", summary(s)}, {"shapes", result.shapes.size()}, {"materials", result.materials.size()}};
}
} // namespace obj_probe

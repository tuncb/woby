#include "probe.h"
#include <cstdlib>
#include <memory>
#define FAST_OBJ_IMPLEMENTATION
#include <fast_obj.h>

namespace obj_probe {
Json fast(const std::string& path)
{
    const auto begin = Clock::now();
    const std::unique_ptr<fastObjMesh, decltype(&fast_obj_destroy)> mesh(fast_obj_read(path.c_str()), fast_obj_destroy);
    const auto ms = elapsed(begin);
    if (!mesh) { throw std::runtime_error("fast_obj_read failed"); }
    Summary s;
    s.normals = mesh->normal_count - 1;
    s.texcoords = mesh->texcoord_count - 1;
    for (size_t i = 1; i < mesh->position_count; ++i) { point(s, &mesh->positions[i * 3]); }
    for (size_t i = 0; i < mesh->face_count; ++i) { face(s, mesh->face_vertices[i]); }
    for (size_t i = 0; i < mesh->index_count; ++i) {
        const auto v = mesh->indices[i];
        index(s, int64_t{v.p} - 1, int64_t{v.n} - 1, int64_t{v.t} - 1);
    }
    return {{"load_ms", ms}, {"geometry", summary(s)}, {"shapes", mesh->group_count}, {"materials", mesh->material_count}};
}
} // namespace obj_probe

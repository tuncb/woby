#include "model_load.h"

#include "file_discovery.h"
#include "obj_mesh.h"
#include "stl_mesh.h"

#include <stdexcept>

namespace woby {

Mesh loadModelMesh(const std::filesystem::path& path)
{
    if (isObjPath(path)) {
        return loadObjMesh(path);
    }
    if (isStlPath(path)) {
        return loadStlMesh(path);
    }

    throw std::runtime_error("Unsupported model file extension: " + path.string());
}

} // namespace woby

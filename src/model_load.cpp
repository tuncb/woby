#include "model_load.h"

#include "file_discovery.h"
#include "obj_mesh.h"
#include "stl_mesh.h"

namespace woby {

ImportedModel loadModel(const std::filesystem::path& path, const std::string& requiredImporterId,
    const ImportCallbacks& callbacks)
{
    if (!requiredImporterId.empty() || (!isObjPath(path) && !isStlPath(path))) {
        return importModel(path, requiredImporterId, callbacks);
    }
    ImportedModel result;
    result.mesh = loadModelMesh(path);
    return result;
}

Mesh loadModelMesh(const std::filesystem::path& path)
{
    if (isObjPath(path)) {
        return loadObjMesh(path);
    }
    if (isStlPath(path)) {
        return loadStlMesh(path);
    }

    return importModel(path, {}, {}).mesh;
}

} // namespace woby

#include "model_load.h"

#include "file_discovery.h"
#include "obj_mesh.h"

namespace woby {

ImportedModel loadModel(const std::filesystem::path& path, const std::string& requiredImporterId,
    const ImportCallbacks& callbacks)
{
    if (!requiredImporterId.empty() || !isObjPath(path)) {
        return importModel(path, requiredImporterId, callbacks);
    }
    ImportedModel result;
    struct Canceled {};
    const auto progress = [&](const ModelLoadProgress& update) {
        if (callbacks.canceled && callbacks.canceled()) { throw Canceled{}; }
        if (callbacks.stageProgress) { callbacks.stageProgress(update); }
    };
    try {
        result.mesh = loadObjMesh(path, progress);
    } catch (const Canceled&) { result.canceled = true; }
    return result;
}

Mesh loadModelMesh(const std::filesystem::path& path)
{
    if (isObjPath(path)) {
        return loadObjMesh(path);
    }
    return importModel(path, {}, {}).mesh;
}

} // namespace woby

#include "post_loading.h"
#include "ui_operations.h"
#include "surface_annotation.h"
#include <algorithm>
#include <stdexcept>

namespace woby {
void setPostLoadPhase(UiState& state, SceneObjectId id, PostLoadPhase phase, std::string error)
{
    for (auto& file : state.files) {
        if (file.objectId != id) { continue; }
        file.postLoadPhase = phase;
        file.postLoadError = phase == PostLoadPhase::failed ? "Model preparation failed: " + error : std::move(error);
        ++state.postLoadRevision;
        return;
    }
}

bool publishPostLoadData(UiState& state, const PostLoadIdentity& identity, PostLoadData data)
{
    if (!postLoadCurrent(identity, state)) { return false; }
    auto& file = *std::find_if(state.files.begin(), state.files.end(), [&](const auto& value) { return value.objectId == identity.fileId; });
    if (data.groups.size() != file.groupSettings.size()) { throw std::invalid_argument("Prepared group count differs from the model."); }
    file.mesh.originalBounds = data.originalBounds;
    file.mesh.sourceData = std::move(data.source);
    file.mesh.annotationCache = std::move(data.annotations);
    file.mesh.preparationDeferred = false;
    for (size_t i = 0; i < data.groups.size(); ++i) {
        auto& group = file.groupSettings[i];
        group.localBounds = data.groups[i].local;
        group.localBoundsValid = true;
        group.originalBounds = data.groups[i].original;
        group.center = group.localBounds.center;
    }
    setPostLoadPhase(state, file.objectId, PostLoadPhase::ready);
    refreshSceneTreeFolderCenters(state);
    recalculateSceneBounds(state);
    validateAnnotationTargets(state, false);
    // Derived publication invalidates caches, but does not create a user edit or dirty the scene.
    ++state.revisions.geometry; ++state.revisions.visibility; ++state.revisions.appearance;
    ++state.revisions.analysis; ++state.revisions.annotations; ++state.revisions.picking;
    return true;
}
} // namespace woby

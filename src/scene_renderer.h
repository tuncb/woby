#pragma once

#include "model_mesh.h"
#include "scene_mesh_upload.h"
#include "scene_mesh_preparation.h"
#include "ui_state.h"
#include "scene_pick.h"
#include "scene_draw_plan.h"
#include "scene_queries.h"
#include "point_cloud.h"

#include "graphics.h"

#include <cstdint>
#include <vector>

namespace woby {

struct MarkerDrawContext;
struct AdaptivePointRuntime;

// CPU scratch and queries owned by the viewport/export runtime, never by UiState.
// Borrowed part views are resolved afresh before use; queries retain owned metadata.
struct SceneRenderScratch {
    SceneQueryRuntime queries;
    SceneDrawCache drawCache;
    std::vector<ScenePickPart> parts;
    std::vector<std::array<float, 3>> positions;
    std::vector<std::array<float, 3>> focusPoints;
    AnnotationQuery annotationPreview;
};

[[nodiscard]] uint32_t vertexPointSize(float masterSize, float groupScale);
// Runtime parameters preserve integer offsets and keep minimized viewports nonzero.
[[nodiscard]] graphics::PointParameters pointSpriteParameters(
    float pointSize, uint32_t viewWidth, uint32_t viewHeight, uint32_t pointOffset);

struct TriangleEdgePrograms {
    woby::graphics::ProgramHandle surface = WOBY_GPU_INVALID_HANDLE, markerSurface = WOBY_GPU_INVALID_HANDLE,
        transparentSurface = WOBY_GPU_INVALID_HANDLE,
        lines = WOBY_GPU_INVALID_HANDLE, markerLines = WOBY_GPU_INVALID_HANDLE;
    bool nativeBarycentrics = false;
};
[[nodiscard]] TriangleEdgePrograms createTriangleEdgePrograms(const std::filesystem::path& assets,
    bool forceVertexPulling = false);
void destroyTriangleEdgePrograms(TriangleEdgePrograms& programs);

struct TransparentSurfacePrograms {
    graphics::ProgramHandle mesh = WOBY_GPU_INVALID_HANDLE;
    std::array<graphics::ProgramHandle, 2> resolve{};
};
[[nodiscard]] TransparentSurfacePrograms createTransparentSurfacePrograms(const std::filesystem::path& assets);
void destroyTransparentSurfacePrograms(TransparentSurfacePrograms& programs);

void submitSceneFiles(
    woby::graphics::ViewId viewId,
    const SceneDrawPlan& plan,
    const std::vector<LoadedModelRuntime>& runtimes,
    woby::graphics::ProgramHandle meshProgram,
    woby::graphics::ProgramHandle colorProgram,
    woby::graphics::ProgramHandle pointSpriteProgram,
    const TriangleEdgePrograms& edgePrograms,
    uint32_t sceneViewportWidth,
    uint32_t viewportHeight,
    MarkerDrawContext* markers = nullptr,
    bool importedLinesOnly = false,
    AdaptivePointRuntime* adaptivePoints = nullptr,
    const TransparentSurfacePrograms* transparency = nullptr); // Line pass follows surfaces/analyses; colorProgram is vs_line_sprite.

void submitSceneHelpers(
    woby::graphics::ViewId viewId,
    const UiState& state,
    const woby::graphics::VertexLayout& layout,
    woby::graphics::ProgramHandle program);

void submitSceneSelection(woby::graphics::ViewId viewId, std::span<const ScenePickPart> parts, const UiState& state,
    const woby::graphics::VertexLayout& layout, woby::graphics::ProgramHandle program,
    SceneRenderScratch& scratch);

} // namespace woby

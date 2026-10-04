#pragma once

#include "surface_annotation.h"
#include "scene_renderer.h"

namespace woby {
struct AnnotationExecutor;
struct AnnotationPointerRuntime;
struct AnnotationInteraction {
    AnnotationExecutor* executor = nullptr;
    std::shared_ptr<AnnotationPointerRuntime> pending;
    std::optional<AnnotationShape> tool;
    bool dragging = false;
    // Runtime-only coarse guide on dense meshes; commit resolves the full edge.
    bool sampledPreview = false;
    SceneObjectId editing = 0;
    int handle = -1;
    // Editing with handle == -1 translates the entire outline.
    std::array<float, 2> grabControl{};
    uint64_t generation = 0, revision = 0;
    AnnotationProjection projection, currentProjection;
    ScenePickView view;
    UiAnnotation preview;
    std::array<float, 2> start{}, end{}, pointerStart{}, pointerEnd{};
    std::string error;
    // Re-evaluate handle visibility once after navigation/scene changes settle.
    PickMatrix overlayView{}, overlayProjection{};
    uint32_t overlayWidth = 0, overlayHeight = 0;
    SceneObjectId overlaySelection = 0;
    uint64_t overlayRevision = 0;
    bool overlayReady = false;
    std::vector<PickPoint> overlayHandles;
    std::optional<PickPoint> overlayEdgePoint;
    bool overlayEdgeHit = false;
};
// ImGui editing state remains outside the logical scene.
struct AnnotationNameEdit {
    SceneObjectId objectId = invalidSceneObjectId;
    uint64_t generation = 0;
    std::string text;
    bool focus = false;
};
void drawAnnotationTools(const UiState& state, AnnotationInteraction& interaction, bool disabled);
void drawAnnotationObjects(UiState& state, AnnotationNameEdit& edit);
void drawAnnotationInspector(UiState& state);
// True consumes the left-button gesture, including invalid placement attempts.
// Unprepared geometry returns false so camera navigation can handle the gesture.
bool beginAnnotationPointer(UiState& state, AnnotationInteraction& interaction,
    const ScenePickView& view, PickPoint point);
void moveAnnotationPointer(const UiState& state, AnnotationInteraction& interaction, PickPoint point);
void endAnnotationPointer(UiState& state, AnnotationInteraction& interaction, bool allowed);
void cancelAnnotationPointer(AnnotationInteraction& interaction);
// Poll on the UI thread, before history recording. Never waits for a worker.
void updateAnnotationPointer(UiState& state, AnnotationInteraction& interaction, const ScenePickView* view = nullptr);
// Returns the message banner's bottom edge in window coordinates (zero if absent),
// so other viewport notifications can be stacked below it.
float drawAnnotationOverlay(const UiState& state, AnnotationInteraction& interaction,
    const ScenePickView& view, float windowX, float pixelsToWindow, bool pointerAllowed, float windowY = 0.0f);
// Requires the annotation vertex shader, which consumes projected NDC positions.
void submitSceneAnnotations(woby::graphics::ViewId viewId, const UiState& state, const ScenePickView& view,
    const woby::graphics::VertexLayout& layout, woby::graphics::ProgramHandle program, woby::graphics::UniformHandle colorUniform,
    SceneRenderScratch& scratch,
    const AnnotationInteraction* interaction = nullptr);
} // namespace woby

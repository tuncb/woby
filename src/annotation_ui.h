#pragma once

#include "surface_annotation.h"
#include "scene_renderer.h"

namespace woby {
struct AnnotationInteraction {
    std::optional<AnnotationShape> tool;
    bool dragging = false;
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
void drawAnnotationTools(const UiState& state, AnnotationInteraction& interaction, bool disabled);
void drawAnnotationObjects(UiState& state);
void drawAnnotationInspector(UiState& state);
// True consumes the left-button gesture, including invalid placement attempts.
bool beginAnnotationPointer(UiState& state, AnnotationInteraction& interaction,
    const ScenePickView& view, PickPoint point);
void moveAnnotationPointer(const UiState& state, AnnotationInteraction& interaction, PickPoint point);
void endAnnotationPointer(UiState& state, AnnotationInteraction& interaction, bool allowed);
void cancelAnnotationPointer(AnnotationInteraction& interaction);
void drawAnnotationOverlay(const UiState& state, AnnotationInteraction& interaction,
    const ScenePickView& view, float windowX, float pixelsToWindow, bool pointerAllowed);
void submitSceneAnnotations(bgfx::ViewId viewId, const UiState& state, const ScenePickView& view,
    const bgfx::VertexLayout& layout, bgfx::ProgramHandle program, bgfx::UniformHandle colorUniform,
    const AnnotationInteraction* interaction = nullptr);
} // namespace woby

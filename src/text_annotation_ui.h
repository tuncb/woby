#pragma once
#include "text_annotation.h"
#include <unordered_map>
#include <imgui.h>
struct ImDrawList;
struct ImFont;
struct ImGuiContext;
namespace woby {
struct AnnotationInteraction;
struct TextAnnotationGlyph {
    std::array<PickPoint, 4> positions, uv;
    ImTextureRef texture;
};
struct TextAnnotationLayout {
    std::string text;
    ImFont* font = nullptr;
    int atlasId = 0, atlasWidth = 0, atlasHeight = 0;
    std::vector<TextAnnotationGlyph> glyphs;
    bool ready = false;
};
struct TextAnnotationDrawCache {
    ImGuiContext* context = nullptr;
    std::unordered_map<SceneObjectId, TextAnnotationLayout> layouts;
    uint64_t builds = 0;
};
void drawTextAnnotationInspector(UiState& state);
void drawTextAnnotations(ImDrawList& draw, const UiState& state, const ScenePickView& view,
    PickPoint origin, float pixelsToWindow, TextAnnotationDrawCache& cache, const AnnotationInteraction* interaction = nullptr);
bool beginTextAnnotationPointer(UiState& state, AnnotationInteraction& interaction, const ScenePickView& view, PickPoint point);
void moveTextAnnotationPointer(AnnotationInteraction& interaction, PickPoint point);
void endTextAnnotationPointer(UiState& state, AnnotationInteraction& interaction, bool allowed);
} // namespace woby

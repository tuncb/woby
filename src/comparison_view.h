#pragma once
#include "comparison_runtime.h"

namespace woby {
// Transient scene-panel editor state; the committed name belongs to UiState.
struct ComparisonNameEdit {
    SceneObjectId objectId = invalidSceneObjectId;
    std::string text;
    bool focus = false;
    int lastFrame = -1;
};

void drawAnalysisCreationMenu(UiState& state);
void drawAnalysisCreationMenu(UiState& state, const std::vector<SceneObjectId>& sources);
void drawComparisonObjects(UiState& state, ComparisonNameEdit& edit, const ComparisonRuntimes& runtimes = {});
void drawComparisonPanelContents(UiState& state, ComparisonRuntimes& runtimes);
void submitComparisonScenes(woby::graphics::ViewId view, const UiState& state, const ComparisonRuntimes& runtimes,
    woby::graphics::ProgramHandle colorProgram, woby::graphics::UniformHandle colorUniform, SceneRenderScratch& scratch,
    woby::graphics::ProgramHandle markerProgram = WOBY_GPU_INVALID_HANDLE);
void appendVisibleComparisonPickParts(std::vector<ScenePickPart>& parts, const UiState& state,
    const ComparisonRuntimes& runtimes);
} // namespace woby

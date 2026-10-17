#pragma once

#include "ui_operations.h"
#include <imgui.h>

namespace woby {
// Visible row order is adapter data. Selection and range anchors live in UiState.
struct UiSelectionRows {
    const UiState* owner = nullptr;
    uint64_t generation = 0;
    uint64_t scope = 0;
    int lastFrame = -1;
    std::vector<uint64_t> previous, current;
    ImVec2 top;
};
void beginSelectionRows(UiSelectionRows& rows, const UiState& state);
std::span<const uint64_t> registerSelectionRow(UiSelectionRows& rows, uint64_t id);
// Returns true for Ctrl+A while the pointer is over this list and no editor is active.
bool endSelectionRows(UiSelectionRows& rows);
bool selectionContains(std::span<const uint64_t> selection, uint64_t id);
std::string bulkMenuLabel(const char* single, const char* verb, size_t count, const char* plural);
void drawBulkMenuHeader(size_t count, const char* noun = "items");
bool drawSelectAllMenu();
void drawSceneVisibilityMenu(UiState& state);
void drawModelBulkMenu(UiState& state);
void drawAnnotationStyleMenu(UiState& state);
} // namespace woby

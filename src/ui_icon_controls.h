#pragma once

#include <cstddef>

namespace woby {

enum class RenderModeState { off, mixed, on };

float uiSize(float value);
float renderModeButtonSize();
bool drawRenderModeIconButton(const char* id, const char* icon, const char* tooltip,
    RenderModeState state, bool disabled);
bool drawTriStateMasterIconButton(const char* id, const char* icon, const char* label,
    size_t enabledCount, size_t totalCount);
bool drawTriStateVisibilityButton(const char* id, const char* label,
    size_t visibleCount, size_t totalCount);
bool drawVisibilityButton(const char* id, bool visible, const char* itemName);
// Edit a local value; callers apply it through ui_operations when this returns true.
bool drawVisibilityField(const char* label, bool& visible, bool mixed = false);
bool drawRemoveButton(const char* id, const char* tooltip);
bool drawResetIconButton(const char* id, const char* tooltip);
bool drawSceneItemButton(const char* label, float width, bool selected);
void drawSceneItemOutline();

} // namespace woby

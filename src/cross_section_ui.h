#pragma once
#include "annotation_ui.h"
namespace woby {
void drawCrossSectionInspector(UiState& state, SceneQueryRuntime* queries);
bool beginSectionPointer(UiState& state, AnnotationInteraction& interaction, const ScenePickView& view,
                         PickPoint point);
void moveSectionPointer(const UiState& state, AnnotationInteraction& interaction, PickPoint point, bool lockDirection);
void endSectionPointer(UiState& state, AnnotationInteraction& interaction, bool allowed);
} // namespace woby

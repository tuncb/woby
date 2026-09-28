#pragma once
#include "marker_pick_logic.h"
#include "scene_viewport.h"
#include "ui_state.h"
#include <optional>

namespace woby::ng {
enum class Scenario { overlapping, translucent, hidden, zeroOpacity, occluded, count };
constexpr uint32_t rearId = 16777217u;
constexpr uint32_t frontId = 33554435u;
struct Fixture {
    UiState state;
    MarkerDrawList markers;
    Scenario scenario = Scenario::overlapping;
};
struct Pixel { int x = 0, y = 0; };
struct Clip { int x = 0, y = 0; uint32_t width = 0, height = 0; };
struct Selection {
    uint64_t epoch = 1, sequence = 0;
    uint32_t id = 0;
    uint32_t accepted = 0, rejected = 0;
};
const char* scenarioName(Scenario scenario);
Fixture makeFixture(Scenario scenario);
uint32_t expectedPick(Scenario scenario);
std::optional<Pixel> cursorPixel(float x, float y, float scaleX, float scaleY, const SceneViewport& viewport);
Clip clipRect(float x1, float y1, float x2, float y2, float originX, float originY,
    float scaleX, float scaleY, uint32_t width, uint32_t height);
bool completePick(Selection& selection, uint64_t epoch, uint64_t sequence, uint32_t id);
void invalidateSelection(Selection& selection);
}

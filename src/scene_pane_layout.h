#pragma once

#include <array>
#include <cstddef>

namespace woby {

enum class ScenePaneSection : size_t { models, analyses, annotations, views, count };
constexpr size_t scenePaneSectionCount = static_cast<size_t>(ScenePaneSection::count);
using ScenePaneHeights = std::array<float, scenePaneSectionCount>;
using ScenePaneOpen = std::array<bool, scenePaneSectionCount>;

// Window layout belongs to the UI adapter, not to the saved scene.
struct ScenePaneLayout {
    ScenePaneOpen open{true, true, false, true};
    ScenePaneHeights weights{1, 1, 1, 1};
    ScenePaneHeights heights{};
    float availableHeight = 0;
    float minimumHeight = 0;
    float dividerHeight = 0;
};

// Keep one usable row per open section. A short pane scrolls as a whole.
ScenePaneHeights scenePaneSectionHeights(const ScenePaneOpen& open,
    const ScenePaneHeights& weights, float availableHeight, float minimumHeight);
void resizeScenePaneSections(ScenePaneLayout& layout, ScenePaneSection section, float delta);

void beginScenePaneLayout(ScenePaneLayout& layout);
// Call endScenePaneSection even when beginScenePaneSection returns false.
bool beginScenePaneSection(ScenePaneLayout& layout, ScenePaneSection section, bool open);
void endScenePaneSection(ScenePaneLayout& layout, ScenePaneSection section);

} // namespace woby

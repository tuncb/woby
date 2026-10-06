#pragma once

#include "scene_queries.h"
#include "ui_operations.h"

namespace woby {

struct PropertiesKey {
    uint64_t generation = 0, revision = 0;
    std::vector<SceneObjectId> selection;
    bool operator==(const PropertiesKey&) const = default;
};

struct InspectorTargetLabel {
    SceneObjectKind kind{};
    std::string name, fileName, path;
};

// Owned values only: no pointers into mutable scene vectors survive preparation.
struct SceneInspectorSnapshot {
    std::string selectionId;
    std::vector<InspectorTargetLabel> targets;
    std::array<UiPropertyValue, uiObjectPropertyCount> properties{};
    UiPropertyValue visibility;
    std::vector<std::string> statistics;
    std::optional<std::array<Coordinate, 2>> bounds;
    std::optional<SceneDimensions> dimensions;
};

struct SceneInspectorRuntime {
    SceneQueryRuntime localQueries;
    SceneQueryRuntime* sharedQueries = nullptr;
    const SceneQueryRuntime* queryOwner = nullptr;
    const ComparisonRuntimes* comparisons = nullptr;
    const UiState* owner = nullptr;
    std::optional<PropertiesKey> key;
    SceneInspectorSnapshot snapshot;
    uint64_t builds = 0, dimensionBuilds = 0;
};

// Call before drawing. Edits are applied after drawing, so this snapshot remains stable.
void updateSceneInspector(SceneInspectorRuntime& runtime, const UiState& state);
UiPropertyValue inspectorProperty(const SceneInspectorSnapshot& snapshot, UiObjectProperty property);

enum class InspectorEditKind { property, reset, visibility };
struct InspectorEdit {
    InspectorEditKind kind = InspectorEditKind::property;
    UiObjectProperty property{};
    float value = 0;
    UiPropertyGroup group{};
};
void applyInspectorEdits(UiState& state, std::span<const InspectorEdit> edits);

} // namespace woby

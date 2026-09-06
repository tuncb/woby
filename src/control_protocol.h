#pragma once

#include "scene_objects.h"
#include <nlohmann/json_fwd.hpp>
#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace woby {

enum class ControlAction {
    status, capabilities, sceneInfo, sceneTree, sceneBounds,
    visibility, render, transformGet, transformSet, transformReset,
    opacity, colorSet, colorReset, vertexSize, grid, origin, upAxis,
    cameraGet, cameraFrame, cameraOrbit, cameraPan, cameraRoll, cameraDolly, cameraMove,
    modelAdd, modelRemove, folderAdd, importersList, importersAdd, importersScan,
    importersForget, stats, performance, pane,
    comparisonCreate, comparisonDelete, comparisonSet, comparisonAdd, comparisonRemove,
    comparisonClear, comparisonSwap, comparisonResults,
};

// Validated, owned command data. No scene pointers or runtime resources cross threads.
struct ControlOperation {
    ControlAction action = ControlAction::status;
    std::string target;
    SceneObjectId objectId = invalidSceneObjectId;
    std::filesystem::path path;
    std::string axis;
    bool tree = false;
    bool remember = false;
    std::optional<bool> visible, solid, triangles, vertices;
    std::optional<std::array<float, 3>> translation, rotationDegrees, rgb;
    std::optional<float> scale, value, pixels, width;
    std::optional<float> yawDegrees, pitchDegrees, rollDegrees, right, up, forward, factor;
    std::optional<std::string> name, mode, side, a, b, object;
    SceneObjectId aId = invalidSceneObjectId, bId = invalidSceneObjectId, memberId = invalidSceneObjectId;
    std::optional<bool> distanceOnA, showEdges, showBoundaries, showNonManifold;
    std::optional<float> tolerance, colorRange;
};

struct ControlMethod {
    ControlAction action;
    std::string method;
    std::string cli;
    std::string positional;
    std::vector<std::string> options;
    std::vector<std::string> required;
    bool requiresValues = false;
    bool mutating = false;
};

const std::vector<ControlMethod>& controlMethods();
const ControlMethod* findControlMethod(const std::string& method);
const ControlMethod& controlMethod(ControlAction action);
std::string controlMethodUsage(const ControlMethod& method);
ControlOperation parseControlOperation(const ControlMethod& method, const nlohmann::json& params);
nlohmann::json controlOperationParams(const ControlOperation& operation);
nlohmann::json controlCapabilities();
struct ControlArguments;
bool parseExtendedControlArguments(int argc, char** argv, ControlArguments& arguments);

} // namespace woby

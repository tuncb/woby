#pragma once
#include "comparison_runtime.h"
#include <nlohmann/json_fwd.hpp>
namespace woby
{
void updateDifferenceRuntime(DifferenceRuntime &runtime, const UiState &state, SceneObjectId id, uint64_t signature);
void destroyDifferenceRuntime(DifferenceRuntime &runtime);
void uploadDifferenceMesh(DifferenceRuntime &runtime);
void requestDifferenceExport(DifferenceRuntime &runtime, const std::filesystem::path &path);
struct ControlOperation;
[[nodiscard]] nlohmann::json controlDifference(UiState &state, DifferenceRuntime &runtime,
                                               const ControlOperation &command);
[[nodiscard]] nlohmann::json differenceStatus(const DifferenceRuntime &runtime, const UiState &state, SceneObjectId id);
void drawDifferenceInspector(UiState &state, ComparisonRuntime &runtime, SceneObjectId id);
void submitDifferenceMesh(graphics::ViewId view, const UiComparison &comparison, const DifferenceRuntime &runtime,
                          graphics::ProgramHandle program, graphics::ProgramHandle markerProgram);
void appendDifferencePickParts(std::vector<ScenePickPart> &parts, const UiComparison &comparison,
                               const DifferenceRuntime &runtime);
} // namespace woby

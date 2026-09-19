#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace woby {

enum class ManualPathKind { models, folder, openScene, saveScene, saveScreenshot };

// Runtime dialog state, separate from the logical scene/UI state.
struct ManualPathDialog {
    ManualPathKind kind = ManualPathKind::models;
    bool active = false;
    bool requestOpen = false;
    float anchorX = 0;
    float anchorY = 0;
    std::string nativeError;
    std::string input;
    std::string error;
    std::optional<std::filesystem::path> overwritePath;
};

void requestManualPathDialog(ManualPathDialog& dialog, std::string nativeError);
// No selection is returned until all paths validate and any overwrite is confirmed.
[[nodiscard]] std::optional<std::vector<std::filesystem::path>> submitManualPathDialog(ManualPathDialog& dialog);
// An empty selection means cancellation; nullopt means the dialog is still open.
[[nodiscard]] std::optional<std::vector<std::filesystem::path>> drawManualPathDialog(
    const char* title, ManualPathDialog& dialog);

} // namespace woby

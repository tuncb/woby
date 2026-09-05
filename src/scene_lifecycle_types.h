#pragma once
#include <filesystem>
#include <string>

namespace woby {

enum class SceneAction { save, saveAs, open, newScene, quit };
enum class DirtyPolicy { error, save, discard };

struct SceneLifecycleCommand {
    SceneAction action = SceneAction::save;
    std::filesystem::path path;
    DirtyPolicy onDirty = DirtyPolicy::error;
    std::filesystem::path savePath;
    bool overwrite = false;
};

struct SceneLifecycleError {
    std::string reason;
    std::string message;
    int code = -32015;
};

} // namespace woby

#pragma once
#include "freeform.h"
#include <filesystem>
#include <string_view>
namespace woby {
struct ObjFreeformInput {
    std::string polygonText;
    std::vector<FreeformPatch> patches;
};
[[nodiscard]] bool objFreeformStatement(std::string_view line);
[[nodiscard]] ObjFreeformInput readObjFreeform(const std::filesystem::path& path,
    const ModelLoadProgressCallback& progress = {});
} // namespace woby

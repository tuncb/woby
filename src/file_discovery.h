#pragma once

#include <filesystem>
#include <vector>

namespace woby {

[[nodiscard]] bool isObjPath(const std::filesystem::path& path);
[[nodiscard]] bool isStlPath(const std::filesystem::path& path);
[[nodiscard]] bool isModelPath(const std::filesystem::path& path);
[[nodiscard]] bool isWobyPath(const std::filesystem::path& path);
[[nodiscard]] std::vector<std::filesystem::path> collectModelPathsRecursive(
    const std::filesystem::path& folder);
[[nodiscard]] std::vector<std::filesystem::path> collectObjPathsRecursive(
    const std::filesystem::path& folder);

} // namespace woby

#pragma once
#include "update.h"
#include <nlohmann/json.hpp>

namespace woby {
[[nodiscard]] nlohmann::json readUpdateJson(const std::filesystem::path& path);
void writeUpdateJson(const std::filesystem::path& path, const nlohmann::json& value);
void commitUpdateFile(const std::filesystem::path& temporary, const std::filesystem::path& destination);
void syncUpdateFile(const std::filesystem::path& path);
void validateUpdateJob(const std::filesystem::path& root, const std::filesystem::path& job);
void launchUpdateHelper(const std::filesystem::path& root, const std::filesystem::path& job, bool restart = false);
void launchUpdatedViewer(const std::filesystem::path& root);
void waitForUpdateParent(uint64_t pid, const std::filesystem::path& job);
void verifyUpdatedExecutable(const std::filesystem::path& root, const std::filesystem::path& job,
    const std::string& version);
} // namespace woby

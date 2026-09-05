#pragma once
#include "control_protocol.h"

namespace woby {
nlohmann::json controlImporterInfo(const std::vector<std::filesystem::path>& remembered);
nlohmann::json applyControlImporterOperation(const ControlOperation& command,
    const std::filesystem::path& settingsPath, std::vector<std::filesystem::path>& remembered);
} // namespace woby

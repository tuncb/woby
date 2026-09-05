#pragma once
#include "utf8_path.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace woby {

struct AutomationInstance {
    std::string id;
    std::string token;
    int port = 0;
    uint64_t pid = 0;
};

struct AutomationRegistration {
    AutomationInstance instance;
    std::filesystem::path recordPath;
    intptr_t lockHandle = -1;
    bool published = false;
};

[[nodiscard]] std::filesystem::path automationRegistryDirectory();
[[nodiscard]] std::string automationRandomHex(size_t byteCount);
void reserveAutomationInstance(
    AutomationRegistration& registration,
    const std::filesystem::path& directory,
    const std::string& id);
void publishAutomationInstance(AutomationRegistration& registration);
void releaseAutomationInstance(AutomationRegistration& registration);
[[nodiscard]] AutomationInstance readAutomationInstance(
    const std::filesystem::path& directory, const std::string& id);
[[nodiscard]] std::vector<AutomationInstance> readAutomationInstances(const std::filesystem::path& directory);

} // namespace woby

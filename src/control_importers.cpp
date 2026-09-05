#include "control_importers.h"
#include "importer_host.h"
#include "utf8_path.h"
#include <algorithm>
#include <stdexcept>

namespace woby {
using Json = nlohmann::json;
Json controlImporterInfo(const std::vector<std::filesystem::path>& remembered)
{
    Json loaded = Json::array(), registrations = Json::array();
    for (const auto& importer : loadedImporters()) {
        loaded.push_back({{"id", importer.id}, {"name", importer.name}, {"version", importer.version},
            {"path", pathToUtf8(importer.path)}, {"extensions", importer.extensions}});
    }
    for (const auto& path : remembered) { registrations.push_back(pathToUtf8(path)); }
    return {{"builtinExtensions", {".obj", ".stl"}}, {"loaded", loaded}, {"remembered", registrations}};
}

Json applyControlImporterOperation(const ControlOperation& command,
    const std::filesystem::path& settingsPath, std::vector<std::filesystem::path>& remembered)
{
    if (command.action == ControlAction::importersList) { return controlImporterInfo(remembered); }
    if (command.action == ControlAction::importersForget) {
        const auto path = std::filesystem::weakly_canonical(command.path);
        auto updated = remembered;
        updated.erase(std::remove(updated.begin(), updated.end(), path), updated.end());
        const bool changed = updated != remembered;
        if (changed) {
            if (settingsPath.empty()) { throw std::runtime_error("Importer settings path is unavailable."); }
            writeImporterSettings(settingsPath, updated);
            remembered = std::move(updated);
        }
        auto result = controlImporterInfo(remembered);
        result["forgotten"] = changed;
        result["path"] = pathToUtf8(path);
        return result;
    }
    if (command.remember && settingsPath.empty()) { throw std::runtime_error("Importer settings path is unavailable."); }
    const auto paths = command.action == ControlAction::importersScan
        ? discoverImporterFiles(command.path) : std::vector<std::filesystem::path>{command.path};
    auto updated = remembered;
    Json outcomes = Json::array();
    for (const auto& path : paths) {
        Json outcome = {{"path", pathToUtf8(path)}, {"loaded", false}, {"remembered", false}};
        try {
            loadImporter(path);
            outcome["loaded"] = true;
            const auto canonical = std::filesystem::canonical(path);
            outcome["path"] = pathToUtf8(canonical);
            if (command.remember && std::find(updated.begin(), updated.end(), canonical) == updated.end()) { updated.push_back(canonical); }
        } catch (const std::exception& error) { outcome["error"] = error.what(); }
        outcomes.push_back(std::move(outcome));
    }
    std::string registrationError;
    if (updated != remembered) {
        try { writeImporterSettings(settingsPath, updated); remembered = std::move(updated); }
        catch (const std::exception& error) { registrationError = error.what(); }
    }
    size_t failed = 0;
    for (auto& outcome : outcomes) {
        const auto path = pathFromUtf8(outcome["path"].get<std::string>());
        outcome["remembered"] = std::find(remembered.begin(), remembered.end(), path) != remembered.end();
        if (!outcome["loaded"].get<bool>()) { ++failed; }
    }
    auto result = controlImporterInfo(remembered);
    result["outcomes"] = outcomes;
    result["failedCount"] = failed;
    result["loadedCount"] = outcomes.size() - failed;
    result["registrationError"] = registrationError.empty() ? Json(nullptr) : Json(registrationError);
    return result;
}
} // namespace woby

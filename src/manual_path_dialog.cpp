#include "manual_path_dialog.h"
#include "utf8_path.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <utility>

namespace woby {

void requestManualPathDialog(ManualPathDialog& dialog, std::string nativeError)
{
    dialog.active = true;
    dialog.requestOpen = true;
    dialog.nativeError = std::move(nativeError);
    dialog.input.clear();
    dialog.error.clear();
    dialog.overwritePath.reset();
}

std::optional<std::vector<std::filesystem::path>> submitManualPathDialog(ManualPathDialog& dialog)
{
    dialog.error.clear();
    const auto reject = [&](const std::string& error) -> std::optional<std::vector<std::filesystem::path>> {
        dialog.error = error;
        dialog.overwritePath.reset();
        return std::nullopt;
    };
    const bool saving = dialog.kind == ManualPathKind::saveScene || dialog.kind == ManualPathKind::saveScreenshot;
    std::vector<std::filesystem::path> paths;
    try {
        std::istringstream lines(dialog.input);
        std::string line;
        while (std::getline(lines, line)) {
            const auto first = line.find_first_not_of(" \t\r");
            if (first == std::string::npos) { continue; }
            line = line.substr(first, line.find_last_not_of(" \t\r") - first + 1);
            if (line.size() >= 2 && (line.front() == '"' || line.front() == '\'') && line.back() == line.front()) {
                line = line.substr(1, line.size() - 2);
            }
            if (line.find('\0') != std::string::npos) { return reject("Paths cannot contain a null character."); }
            auto path = pathFromUtf8(line);
            if (!path.is_absolute()) { return reject("Paste an absolute path, starting from the filesystem root."); }
            if (saving) {
                auto extension = pathToUtf8(path.extension());
                std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char value) {
                    return static_cast<char>(std::tolower(value));
                });
                if (dialog.kind == ManualPathKind::saveScene && extension != ".woby") { path += ".woby"; }
                if (dialog.kind == ManualPathKind::saveScreenshot && extension != ".png") { path.replace_extension(".png"); }
            }
            // Check the original destination too, before adding a save extension.
            if (saving && std::filesystem::is_directory(pathFromUtf8(line))) {
                return reject("Enter a file path, including its name, rather than a folder.");
            }
            if (dialog.kind == ManualPathKind::folder) {
                if (!std::filesystem::is_directory(path)) { return reject("Folder does not exist: " + pathToUtf8(path)); }
            } else if (saving) {
                if (!std::filesystem::is_directory(path.parent_path())) {
                    return reject("The destination folder does not exist: " + pathToUtf8(path.parent_path()));
                }
                if (std::filesystem::exists(path) && !std::filesystem::is_regular_file(path)) {
                    return reject("The destination is not a regular file: " + pathToUtf8(path));
                }
            } else if (!std::filesystem::is_regular_file(path)) {
                return reject("File does not exist or is not a regular file: " + pathToUtf8(path));
            }
            paths.push_back(std::move(path));
        }
        if (paths.empty()) { return reject("Paste a path to continue."); }
        if (dialog.kind != ManualPathKind::models && paths.size() != 1) { return reject("Enter exactly one path."); }
        if (saving && std::filesystem::exists(paths.front()) && dialog.overwritePath != paths.front()) {
            dialog.overwritePath = paths.front();
            return std::nullopt;
        }
    } catch (const std::exception& error) {
        return reject(std::string("Cannot use this path: ") + error.what());
    }
    dialog.active = false;
    dialog.requestOpen = false;
    dialog.overwritePath.reset();
    return paths;
}

} // namespace woby

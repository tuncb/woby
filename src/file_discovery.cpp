#include "file_discovery.h"
#include "importer_host.h"
#include "utf8_path.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>
#include <system_error>

namespace woby {
namespace {

std::string lowercase(std::string value)
{
    for (char& character : value) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }

    return value;
}

} // namespace

bool isObjPath(const std::filesystem::path& path)
{
    return lowercase(pathToUtf8(path.extension())) == ".obj";
}

bool isModelPath(const std::filesystem::path& path)
{
    return isObjPath(path) || hasImporterForPath(path);
}

bool isWobyPath(const std::filesystem::path& path)
{
    return lowercase(pathToUtf8(path.extension())) == ".woby";
}

std::vector<std::filesystem::path> collectModelPathsRecursive(
    const std::filesystem::path& folder)
{
    std::error_code error;
    if (!std::filesystem::is_directory(folder, error)) {
        throw std::runtime_error("Folder path is not a folder: " + pathToUtf8(folder));
    }

    std::vector<std::filesystem::path> modelPaths;
    const auto options = std::filesystem::directory_options::skip_permission_denied;
    std::filesystem::recursive_directory_iterator iterator(folder, options, error);
    if (error) {
        throw std::runtime_error("Failed to scan folder: " + pathToUtf8(folder));
    }

    const std::filesystem::recursive_directory_iterator end;
    while (iterator != end) {
        const std::filesystem::directory_entry& entry = *iterator;
        error.clear();
        if (entry.is_regular_file(error) && isModelPath(entry.path())) {
            modelPaths.push_back(entry.path());
        }

        error.clear();
        iterator.increment(error);
        if (error) {
            throw std::runtime_error("Failed to scan folder: " + pathToUtf8(folder));
        }
    }

    std::sort(modelPaths.begin(), modelPaths.end());
    return modelPaths;
}

} // namespace woby

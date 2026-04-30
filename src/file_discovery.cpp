#include "file_discovery.h"

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
    return lowercase(path.extension().string()) == ".obj";
}

bool isStlPath(const std::filesystem::path& path)
{
    return lowercase(path.extension().string()) == ".stl";
}

bool isModelPath(const std::filesystem::path& path)
{
    return isObjPath(path) || isStlPath(path);
}

bool isWobyPath(const std::filesystem::path& path)
{
    return lowercase(path.extension().string()) == ".woby";
}

std::vector<std::filesystem::path> collectModelPathsRecursive(
    const std::filesystem::path& folder)
{
    std::error_code error;
    if (!std::filesystem::is_directory(folder, error)) {
        throw std::runtime_error("Folder path is not a folder: " + folder.string());
    }

    std::vector<std::filesystem::path> modelPaths;
    const auto options = std::filesystem::directory_options::skip_permission_denied;
    std::filesystem::recursive_directory_iterator iterator(folder, options, error);
    if (error) {
        throw std::runtime_error("Failed to scan folder: " + folder.string());
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
            throw std::runtime_error("Failed to scan folder: " + folder.string());
        }
    }

    std::sort(modelPaths.begin(), modelPaths.end());
    return modelPaths;
}

std::vector<std::filesystem::path> collectObjPathsRecursive(
    const std::filesystem::path& folder)
{
    std::vector<std::filesystem::path> objPaths;
    for (const auto& path : collectModelPathsRecursive(folder)) {
        if (isObjPath(path)) {
            objPaths.push_back(path);
        }
    }
    return objPaths;
}

} // namespace woby

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

bool isWobyPath(const std::filesystem::path& path)
{
    return lowercase(path.extension().string()) == ".woby";
}

std::vector<std::filesystem::path> collectObjPathsRecursive(
    const std::filesystem::path& folder)
{
    std::error_code error;
    if (!std::filesystem::is_directory(folder, error)) {
        throw std::runtime_error("Folder path is not a folder: " + folder.string());
    }

    std::vector<std::filesystem::path> objPaths;
    const auto options = std::filesystem::directory_options::skip_permission_denied;
    std::filesystem::recursive_directory_iterator iterator(folder, options, error);
    if (error) {
        throw std::runtime_error("Failed to scan folder: " + folder.string());
    }

    const std::filesystem::recursive_directory_iterator end;
    while (iterator != end) {
        const std::filesystem::directory_entry& entry = *iterator;
        error.clear();
        if (entry.is_regular_file(error) && isObjPath(entry.path())) {
            objPaths.push_back(entry.path());
        }

        error.clear();
        iterator.increment(error);
        if (error) {
            throw std::runtime_error("Failed to scan folder: " + folder.string());
        }
    }

    std::sort(objPaths.begin(), objPaths.end());
    return objPaths;
}

} // namespace woby

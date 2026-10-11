#pragma once

#include "file_associations.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace woby {

// All registry paths are relative to userRoot. Tests use an isolated key.
[[nodiscard]] std::wstring fileAssociationOpenCommand(FileAssociationType type,
    const std::filesystem::path& executable);
[[nodiscard]] FileAssociationStatus readFileAssociationRegistration(HKEY userRoot,
    FileAssociationType type, const std::filesystem::path& executable);
void registerFileAssociation(HKEY userRoot, FileAssociationType type,
    const std::filesystem::path& executable);
void removeFileAssociation(HKEY userRoot, FileAssociationType type,
    const std::filesystem::path& executable);

} // namespace woby
#endif

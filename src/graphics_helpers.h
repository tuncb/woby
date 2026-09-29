#pragma once

#include "graphics.h"

#include <filesystem>

namespace woby {

// Returns the missing requirement, or nullptr when the renderer is supported.
[[nodiscard]] const char* unsupportedRendererReason(const woby::graphics::Caps& caps);
void validateRendererCapabilities(const woby::graphics::Caps& caps);
const char* rendererShaderFolder(woby::graphics::RendererType::Enum renderer);
woby::graphics::ShaderHandle loadShader(const std::filesystem::path& path);
woby::graphics::ProgramHandle loadProgram(
    const std::filesystem::path& assetRoot,
    const char* vertexShaderName,
    const char* fragmentShaderName);

} // namespace woby

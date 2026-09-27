#pragma once

#include <bgfx/bgfx.h>

#include <filesystem>

namespace woby {

// Returns the missing requirement, or nullptr when the renderer is supported.
[[nodiscard]] const char* unsupportedRendererReason(const bgfx::Caps& caps);
void validateRendererCapabilities(const bgfx::Caps& caps);
struct OpenGlCapabilities {
    int major = 0, minor = 0;
    int vertexStorageBlocks = 0, storageBindings = 0;
};
// bgfx's generic caps do not expose GL versions or vertex-stage SSBO limits.
[[nodiscard]] const char* unsupportedOpenGlReason(bool es, const OpenGlCapabilities& caps);

const char* rendererShaderFolder(bgfx::RendererType::Enum renderer);
bgfx::ShaderHandle loadShader(const std::filesystem::path& path);
bgfx::ProgramHandle loadProgram(
    const std::filesystem::path& assetRoot,
    const char* vertexShaderName,
    const char* fragmentShaderName);

} // namespace woby

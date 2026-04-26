#pragma once

#include <bgfx/bgfx.h>

#include <filesystem>

namespace woby {

const char* rendererShaderFolder(bgfx::RendererType::Enum renderer);
bgfx::ShaderHandle loadShader(const std::filesystem::path& path);
bgfx::ProgramHandle loadProgram(
    const std::filesystem::path& assetRoot,
    const char* vertexShaderName,
    const char* fragmentShaderName);

} // namespace woby

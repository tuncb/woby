#include "bgfx_helpers.h"

#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace woby {

const char* rendererShaderFolder(bgfx::RendererType::Enum renderer)
{
    if (renderer == bgfx::RendererType::Direct3D11 || renderer == bgfx::RendererType::Direct3D12) {
        return "dx11";
    }
    if (renderer == bgfx::RendererType::Metal) {
        return "metal";
    }
    if (renderer == bgfx::RendererType::OpenGL) {
        return "glsl";
    }
    if (renderer == bgfx::RendererType::OpenGLES) {
        return "essl";
    }
    if (renderer == bgfx::RendererType::Vulkan) {
        return "spirv";
    }

    return "glsl";
}

bgfx::ShaderHandle loadShader(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        throw std::runtime_error("Failed to open shader: " + path.string());
    }

    const auto size = file.tellg();
    if (size <= 0) {
        throw std::runtime_error("Shader is empty: " + path.string());
    }

    std::vector<char> data(static_cast<size_t>(size));
    file.seekg(0, std::ios::beg);
    file.read(data.data(), size);

    const bgfx::Memory* memory = bgfx::copy(data.data(), static_cast<uint32_t>(data.size()));
    bgfx::ShaderHandle shader = bgfx::createShader(memory);
    bgfx::setName(shader, path.filename().string().c_str());
    return shader;
}

bgfx::ProgramHandle loadProgram(
    const std::filesystem::path& assetRoot,
    const char* vertexShaderName,
    const char* fragmentShaderName)
{
    const auto rendererFolder = rendererShaderFolder(bgfx::getRendererType());
    const auto shaderRoot = assetRoot / "shaders" / rendererFolder;

    bgfx::ShaderHandle vertexShader = loadShader(shaderRoot / vertexShaderName);
    bgfx::ShaderHandle fragmentShader = loadShader(shaderRoot / fragmentShaderName);
    return bgfx::createProgram(vertexShader, fragmentShader, true);
}

} // namespace woby

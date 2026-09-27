#include "bgfx_helpers.h"

#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace woby {

const char* unsupportedOpenGlReason(bool es, const OpenGlCapabilities& caps)
{
    const int version = caps.major * 10 + caps.minor;
    if (version < (es ? 31 : 43)) {
        return es ? "OpenGL ES 3.1 / GLSL ES 3.10 or newer is required."
                  : "OpenGL 4.3 / GLSL 4.30 or newer is required.";
    }
    // ES 3.1 permits zero vertex-stage storage blocks, even with compute support.
    if (caps.vertexStorageBlocks < 2 || caps.storageBindings < 2) {
        return "Vertex shaders must support at least two shader storage buffers.";
    }
    return nullptr;
}

const char* unsupportedRendererReason(const bgfx::Caps& caps)
{
    switch (caps.rendererType) {
    case bgfx::RendererType::Direct3D11:
        // bgfx can expose COMPUTE on feature-level 10 hardware. Our packaged
        // s_5_0 shaders require 11_0; bgfx exposes PRIMITIVE_ID at that level.
        if ((caps.supported & BGFX_CAPS_PRIMITIVE_ID) == 0) {
            return "Direct3D feature level 11_0 (Shader Model 5.0) is required.";
        }
        break;
    case bgfx::RendererType::Direct3D12:
    case bgfx::RendererType::Metal:
    case bgfx::RendererType::Vulkan:
    case bgfx::RendererType::OpenGL:
    case bgfx::RendererType::OpenGLES:
        break;
    case bgfx::RendererType::Noop:
    case bgfx::RendererType::Agc:
    case bgfx::RendererType::Gnm:
    case bgfx::RendererType::Nvn:
    case bgfx::RendererType::Count:
        return "A supported hardware graphics renderer is required.";
    }
    if ((caps.supported & BGFX_CAPS_COMPUTE) == 0) {
        return "Shader-readable storage buffers are required (OpenGL 4.3 / OpenGL ES 3.1 or equivalent).";
    }
    if ((caps.supported & BGFX_CAPS_VERTEX_ID) == 0) {
        return "Shader-generated vertices (vertex ID) are required.";
    }
    if ((caps.supported & BGFX_CAPS_INSTANCING) == 0) {
        return "Instanced drawing is required.";
    }
    if ((caps.supported & BGFX_CAPS_INDEX32) == 0) {
        return "32-bit index buffers are required.";
    }
    if (caps.limits.maxComputeBindings < 2) {
        return "At least two shader storage buffer bindings are required.";
    }
    return nullptr;
}

void validateRendererCapabilities(const bgfx::Caps& caps)
{
    if (const auto* reason = unsupportedRendererReason(caps)) {
        throw std::runtime_error(std::string("This graphics device or driver cannot run Woby.\n\n")
            + reason + "\nRenderer: " + bgfx::getRendererName(caps.rendererType)
            + "\n\nUpdate your graphics driver or use a device with these graphics features.");
    }
}

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
    if (!bgfx::isValid(shader)) {
        throw std::runtime_error("Failed to create shader: " + path.string());
    }
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
    bgfx::ShaderHandle fragmentShader = BGFX_INVALID_HANDLE;
    try {
        fragmentShader = loadShader(shaderRoot / fragmentShaderName);
    } catch (...) {
        bgfx::destroy(vertexShader);
        throw;
    }
    const auto program = bgfx::createProgram(vertexShader, fragmentShader, true);
    if (!bgfx::isValid(program)) {
        throw std::runtime_error(std::string("Failed to create shader program: ") + vertexShaderName);
    }
    return program;
}

} // namespace woby

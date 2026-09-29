#include "graphics_helpers.h"

#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace woby {

const char* unsupportedRendererReason(const woby::graphics::Caps& caps)
{
    if (caps.rendererType != graphics::RendererType::Vulkan
        && caps.rendererType != graphics::RendererType::Metal) {
        return "A supported NoGraphicsAPI hardware renderer is required.";
    }
    if ((caps.supported & WOBY_GPU_CAPS_COMPUTE) == 0) {
        return "Shader-readable storage buffers are required.";
    }
    if ((caps.supported & WOBY_GPU_CAPS_VERTEX_ID) == 0) {
        return "Shader-generated vertices (vertex ID) are required.";
    }
    if ((caps.supported & WOBY_GPU_CAPS_INSTANCING) == 0) {
        return "Instanced drawing is required.";
    }
    if ((caps.supported & WOBY_GPU_CAPS_INDEX32) == 0) {
        return "32-bit index buffers are required.";
    }
    if (caps.limits.maxComputeBindings < 2) {
        return "At least two shader storage buffer bindings are required.";
    }
    return nullptr;
}

void validateRendererCapabilities(const woby::graphics::Caps& caps)
{
    if (const auto* reason = unsupportedRendererReason(caps)) {
        throw std::runtime_error(std::string("This graphics device or driver cannot run Woby.\n\n")
            + reason + "\nRenderer: " + woby::graphics::getRendererName(caps.rendererType)
            + "\n\nUpdate your graphics driver or use a device with these graphics features.");
    }
}

const char* rendererShaderFolder(woby::graphics::RendererType::Enum renderer)
{
    if (renderer == graphics::RendererType::Metal) return "metal";
    if (renderer == graphics::RendererType::Vulkan) return "spirv";
    throw std::runtime_error("No shader binaries exist for this renderer.");
}

woby::graphics::ShaderHandle loadShader(const std::filesystem::path& path)
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

    const woby::graphics::Memory* memory = woby::graphics::copy(data.data(), static_cast<uint32_t>(data.size()));
    woby::graphics::ShaderHandle shader = woby::graphics::createShader(memory);
    if (!woby::graphics::isValid(shader)) {
        throw std::runtime_error("Failed to create shader: " + path.string());
    }
    woby::graphics::setName(shader, path.filename().string().c_str());
    return shader;
}

woby::graphics::ProgramHandle loadProgram(
    const std::filesystem::path& assetRoot,
    const char* vertexShaderName,
    const char* fragmentShaderName)
{
    const auto rendererFolder = rendererShaderFolder(woby::graphics::getRendererType());
    const auto shaderRoot = assetRoot / "shaders" / rendererFolder;

    woby::graphics::ShaderHandle vertexShader = loadShader(shaderRoot / vertexShaderName);
    woby::graphics::ShaderHandle fragmentShader = WOBY_GPU_INVALID_HANDLE;
    try {
        fragmentShader = loadShader(shaderRoot / fragmentShaderName);
    } catch (...) {
        woby::graphics::destroy(vertexShader);
        throw;
    }
    const auto program = woby::graphics::createProgram(vertexShader, fragmentShader, true);
    if (!woby::graphics::isValid(program)) {
        throw std::runtime_error(std::string("Failed to create shader program: ") + vertexShaderName);
    }
    return program;
}

} // namespace woby

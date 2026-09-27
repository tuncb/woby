#include "renderer_startup.h"
#include "bgfx_helpers.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>

namespace woby {
namespace {

using GetGlInteger = void (APIENTRY*)(GLenum, GLint*);
struct GlCapabilityQuery {
    GetGlInteger getInteger = nullptr;
    OpenGlCapabilities caps;
    std::array<uint16_t, 3> indices{};
    std::atomic<bool> ready = false;
};

void readGlCapabilities(void*, void* userData)
{
    // A static-buffer upload releases its data on bgfx's render thread, where
    // its GL context is current. Shared ownership also covers startup failure.
    const std::unique_ptr<std::shared_ptr<GlCapabilityQuery>> owner(
        static_cast<std::shared_ptr<GlCapabilityQuery>*>(userData));
    auto& query = **owner;
    query.getInteger(GL_MAJOR_VERSION, &query.caps.major);
    query.getInteger(GL_MINOR_VERSION, &query.caps.minor);
    query.getInteger(GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS, &query.caps.vertexStorageBlocks);
    query.getInteger(GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS, &query.caps.storageBindings);
    query.ready.store(true, std::memory_order_release);
}

} // namespace

void validateRendererStartup()
{
    const auto& caps = *bgfx::getCaps();
    validateRendererCapabilities(caps);
    if (caps.rendererType != bgfx::RendererType::OpenGL
        && caps.rendererType != bgfx::RendererType::OpenGLES) { return; }

    const bool es = caps.rendererType == bgfx::RendererType::OpenGLES;
    if (es) { SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES); }
    if (!SDL_GL_LoadLibrary(nullptr)) {
        throw std::runtime_error(std::string("Cannot check the OpenGL graphics requirements: ") + SDL_GetError());
    }
    auto query = std::make_shared<GlCapabilityQuery>();
    query->getInteger = reinterpret_cast<GetGlInteger>(SDL_GL_GetProcAddress("glGetIntegerv"));
    if (query->getInteger == nullptr) {
        SDL_GL_UnloadLibrary();
        throw std::runtime_error("Cannot check the OpenGL graphics requirements: glGetIntegerv is unavailable.");
    }
    const auto buffer = bgfx::createIndexBuffer(bgfx::makeRef(query->indices.data(),
        static_cast<uint32_t>(sizeof(query->indices)), readGlCapabilities,
        new std::shared_ptr<GlCapabilityQuery>(query)));
    if (!bgfx::isValid(buffer)) {
        throw std::runtime_error("Cannot check the OpenGL graphics requirements: buffer allocation failed.");
    }
    bgfx::destroy(buffer);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!query->ready.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= deadline) {
            // Keep GL loaded until shutdown processes any pending release callback.
            throw std::runtime_error("Cannot check the OpenGL graphics requirements: the renderer did not respond.");
        }
        bgfx::frame();
    }
    SDL_GL_UnloadLibrary();
    if (const auto* reason = unsupportedOpenGlReason(es, query->caps)) {
        throw std::runtime_error(std::string("This graphics device or driver cannot run Woby.\n\n")
            + reason + "\n\nUpdate your graphics driver or use a device with these graphics features.");
    }
}

} // namespace woby

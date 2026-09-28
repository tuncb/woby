#include "imgui_graphics.h"

#include "graphics_helpers.h"

#include <bx/math.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

namespace woby::imgui_graphics {
namespace {

struct RendererState {
    woby::graphics::ViewId viewId = 255;
    woby::graphics::VertexLayout layout{};
    woby::graphics::ProgramHandle program = WOBY_GPU_INVALID_HANDLE;
    woby::graphics::UniformHandle textureSampler = WOBY_GPU_INVALID_HANDLE;
};

RendererState state;

ImTextureID encodeTexture(woby::graphics::TextureHandle handle)
{
    return static_cast<ImTextureID>(handle.idx) + 1u;
}

woby::graphics::TextureHandle decodeTexture(ImTextureID textureId)
{
    if (textureId == ImTextureID_Invalid) {
        return WOBY_GPU_INVALID_HANDLE;
    }
    return woby::graphics::TextureHandle{static_cast<uint32_t>(textureId - 1u)};
}

template <typename HandleT>
bool valid(HandleT handle)
{
    return woby::graphics::isValid(handle);
}

uint16_t toUint16(int value, const char* name)
{
    if (value < 0 || value > static_cast<int>(std::numeric_limits<uint16_t>::max())) {
        throw std::runtime_error(std::string("Dear ImGui texture ") + name + " is outside graphics uint16 range.");
    }
    return static_cast<uint16_t>(value);
}

uint32_t textureUploadSize(int pitch, int rowBytes, int height)
{
    if (height <= 0) {
        return 0;
    }
    return static_cast<uint32_t>(pitch * (height - 1) + rowBytes);
}

const woby::graphics::Memory* copyRgba32TextureRect(ImTextureData* texture, const ImTextureRect& rect, uint16_t& pitch)
{
    const int sourcePitch = texture->GetPitch();
    const int rowBytes = rect.w * texture->BytesPerPixel;
    pitch = toUint16(sourcePitch, "pitch");
    return woby::graphics::copy(
        texture->GetPixelsAt(rect.x, rect.y),
        textureUploadSize(sourcePitch, rowBytes, rect.h));
}

const woby::graphics::Memory* copyAlpha8TextureRectAsRgba32(ImTextureData* texture, const ImTextureRect& rect, uint16_t& pitch)
{
    const int targetPitch = rect.w * 4;
    pitch = toUint16(targetPitch, "pitch");

    const woby::graphics::Memory* memory = woby::graphics::alloc(static_cast<uint32_t>(targetPitch * rect.h));
    auto* target = memory->data;
    const auto* sourceBase = static_cast<const unsigned char*>(texture->GetPixelsAt(rect.x, rect.y));
    const int sourcePitch = texture->GetPitch();

    for (uint16_t y = 0; y < rect.h; ++y) {
        const unsigned char* source = sourceBase + y * sourcePitch;
        unsigned char* row = target + y * targetPitch;
        for (uint16_t x = 0; x < rect.w; ++x) {
            row[x * 4 + 0] = 255;
            row[x * 4 + 1] = 255;
            row[x * 4 + 2] = 255;
            row[x * 4 + 3] = source[x];
        }
    }

    return memory;
}

const woby::graphics::Memory* copyTextureRectPixels(ImTextureData* texture, const ImTextureRect& rect, uint16_t& pitch)
{
    if (texture->Format == ImTextureFormat_RGBA32) {
        return copyRgba32TextureRect(texture, rect, pitch);
    }
    if (texture->Format == ImTextureFormat_Alpha8) {
        return copyAlpha8TextureRectAsRgba32(texture, rect, pitch);
    }

    throw std::runtime_error("Unsupported Dear ImGui texture format.");
}

void uploadTextureRect(ImTextureData* texture, woby::graphics::TextureHandle handle, const ImTextureRect& rect)
{
    uint16_t pitch = 0;
    const woby::graphics::Memory* memory = copyTextureRectPixels(texture, rect, pitch);
    woby::graphics::updateTexture2D(
        handle,
        0,
        0,
        rect.x,
        rect.y,
        rect.w,
        rect.h,
        memory,
        pitch);
}

void destroyTexture(ImTextureData* texture)
{
    const woby::graphics::TextureHandle handle = decodeTexture(texture->GetTexID());
    if (valid(handle)) {
        woby::graphics::destroy(handle);
    }

    texture->SetTexID(ImTextureID_Invalid);
    texture->BackendUserData = nullptr;
    texture->SetStatus(ImTextureStatus_Destroyed);
}

void createTexture(ImTextureData* texture)
{
    if (texture->TexID != ImTextureID_Invalid) {
        destroyTexture(texture);
    }

    const woby::graphics::TextureHandle handle = woby::graphics::createTexture2D(
        toUint16(texture->Width, "width"),
        toUint16(texture->Height, "height"),
        false,
        1,
        woby::graphics::TextureFormat::RGBA8,
        WOBY_GPU_SAMPLER_U_CLAMP | WOBY_GPU_SAMPLER_V_CLAMP,
        nullptr);

    if (!valid(handle)) {
        throw std::runtime_error("graphics failed to create Dear ImGui texture.");
    }

    woby::graphics::setName(handle, "Dear ImGui Texture");

    const ImTextureRect rect{
        0,
        0,
        toUint16(texture->Width, "width"),
        toUint16(texture->Height, "height"),
    };
    uploadTextureRect(texture, handle, rect);

    texture->SetTexID(encodeTexture(handle));
    texture->BackendUserData = nullptr;
    texture->SetStatus(ImTextureStatus_OK);
}

void updateTexture(ImTextureData* texture)
{
    if (texture->Status == ImTextureStatus_WantCreate) {
        createTexture(texture);
        return;
    }

    if (texture->Status == ImTextureStatus_WantUpdates) {
        const woby::graphics::TextureHandle handle = decodeTexture(texture->GetTexID());
        if (!valid(handle)) {
            createTexture(texture);
            return;
        }

        // Publish one immutable version for all atlas updates this frame.
        uploadTextureRect(texture, handle, {0, 0,
            toUint16(texture->Width, "width"), toUint16(texture->Height, "height")});
        texture->SetStatus(ImTextureStatus_OK);
        return;
    }

    if (texture->Status == ImTextureStatus_WantDestroy && texture->UnusedFrames > 0) {
        destroyTexture(texture);
    }
}

void updateTextures(ImDrawData* drawData)
{
    if (drawData->Textures == nullptr) {
        return;
    }

    for (ImTextureData* texture : *drawData->Textures) {
        if (texture->Status != ImTextureStatus_OK) {
            updateTexture(texture);
        }
    }
}

} // namespace

void init(const std::filesystem::path& assetRoot, woby::graphics::ViewId viewId)
{
    state.viewId = viewId;

    state.layout = {static_cast<uint16_t>(sizeof(ImDrawVert))};

    state.textureSampler = woby::graphics::createUniform("s_tex", woby::graphics::UniformType::Sampler);
    state.program = loadProgram(assetRoot, "vs_imgui.bin", "fs_imgui.bin");

    ImGuiIO& io = ImGui::GetIO();
    io.BackendRendererName = "woby_imgui_graphics";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures;

    ImGuiPlatformIO& platformIo = ImGui::GetPlatformIO();
    platformIo.Renderer_TextureMaxWidth = static_cast<int>(woby::graphics::getCaps()->limits.maxTextureSize);
    platformIo.Renderer_TextureMaxHeight = static_cast<int>(woby::graphics::getCaps()->limits.maxTextureSize);
}

void shutdown()
{
    for (ImTextureData* texture : ImGui::GetPlatformIO().Textures) {
        if (texture->RefCount == 1) {
            destroyTexture(texture);
        }
    }

    ImGuiIO& io = ImGui::GetIO();
    io.BackendRendererName = nullptr;
    io.BackendFlags &= ~(ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures);
    ImGui::GetPlatformIO().ClearRendererHandlers();

    if (valid(state.textureSampler)) {
        woby::graphics::destroy(state.textureSampler);
    }
    if (valid(state.program)) {
        woby::graphics::destroy(state.program);
    }

    state = RendererState{};
}

void render(ImDrawData* drawData)
{
    renderToView(drawData, state.viewId);
}

void renderToView(ImDrawData* drawData, woby::graphics::ViewId viewId)
{
    const int32_t framebufferWidth = static_cast<int32_t>(drawData->DisplaySize.x * drawData->FramebufferScale.x);
    const int32_t framebufferHeight = static_cast<int32_t>(drawData->DisplaySize.y * drawData->FramebufferScale.y);
    if (framebufferWidth <= 0 || framebufferHeight <= 0) {
        return;
    }

    updateTextures(drawData);

    woby::graphics::setViewName(viewId, "Dear ImGui");
    woby::graphics::setViewMode(viewId, woby::graphics::ViewMode::Sequential);
    woby::graphics::setViewRect(
        viewId,
        0,
        0,
        static_cast<uint16_t>(framebufferWidth),
        static_cast<uint16_t>(framebufferHeight));

    float ortho[16];
    const float left = drawData->DisplayPos.x;
    const float right = drawData->DisplayPos.x + drawData->DisplaySize.x;
    const float top = drawData->DisplayPos.y;
    const float bottom = drawData->DisplayPos.y + drawData->DisplaySize.y;
    bx::mtxOrtho(ortho, left, right, bottom, top, 0.0f, 1000.0f, 0.0f, woby::graphics::getCaps()->homogeneousDepth);
    woby::graphics::setViewTransform(viewId, nullptr, ortho);

    const ImVec2 clipOffset = drawData->DisplayPos;
    const ImVec2 clipScale = drawData->FramebufferScale;

    for (int commandListIndex = 0; commandListIndex < drawData->CmdListsCount; ++commandListIndex) {
        const ImDrawList* commandList = drawData->CmdLists[commandListIndex];
        const auto vertexCount = static_cast<uint32_t>(commandList->VtxBuffer.Size);
        const auto indexCount = static_cast<uint32_t>(commandList->IdxBuffer.Size);

        if (woby::graphics::getAvailTransientVertexBuffer(vertexCount, state.layout) < vertexCount
            || woby::graphics::getAvailTransientIndexBuffer(indexCount, sizeof(ImDrawIdx) == 4) < indexCount) {
            if (viewId != state.viewId) { throw std::runtime_error("Insufficient GPU buffer space for export annotations."); }
            break;
        }

        woby::graphics::TransientVertexBuffer vertexBuffer;
        woby::graphics::TransientIndexBuffer indexBuffer;
        woby::graphics::allocTransientVertexBuffer(&vertexBuffer, vertexCount, state.layout);
        woby::graphics::allocTransientIndexBuffer(&indexBuffer, indexCount, sizeof(ImDrawIdx) == 4);

        std::memcpy(vertexBuffer.data, commandList->VtxBuffer.Data, vertexCount * sizeof(ImDrawVert));
        std::memcpy(indexBuffer.data, commandList->IdxBuffer.Data, indexCount * sizeof(ImDrawIdx));

        for (const ImDrawCmd& command : commandList->CmdBuffer) {
            if (command.UserCallback != nullptr) {
                if (command.UserCallback != ImDrawCallback_ResetRenderState)
                    command.UserCallback(commandList, &command);
                continue;
            }

            const ImVec4 clipRect{
                (command.ClipRect.x - clipOffset.x) * clipScale.x,
                (command.ClipRect.y - clipOffset.y) * clipScale.y,
                (command.ClipRect.z - clipOffset.x) * clipScale.x,
                (command.ClipRect.w - clipOffset.y) * clipScale.y,
            };

            if (clipRect.x >= framebufferWidth || clipRect.y >= framebufferHeight || clipRect.z < 0.0f || clipRect.w < 0.0f) {
                continue;
            }

            if (clipRect.z <= std::max(clipRect.x, 0.0f) || clipRect.w <= std::max(clipRect.y, 0.0f)) continue;
            const auto scissorX = static_cast<uint16_t>(std::max(clipRect.x, 0.0f));
            const auto scissorY = static_cast<uint16_t>(std::max(clipRect.y, 0.0f));
            const auto scissorW = static_cast<uint16_t>(std::min(clipRect.z, static_cast<float>(framebufferWidth)) - scissorX);
            const auto scissorH = static_cast<uint16_t>(std::min(clipRect.w, static_cast<float>(framebufferHeight)) - scissorY);

            const woby::graphics::TextureHandle texture = decodeTexture(command.GetTexID());
            if (!valid(texture)) {
                continue;
            }

            woby::graphics::setScissor(scissorX, scissorY, scissorW, scissorH);
            woby::graphics::setState(
                WOBY_GPU_STATE_WRITE_RGB
                | WOBY_GPU_STATE_WRITE_A
                | WOBY_GPU_STATE_MSAA
                | WOBY_GPU_STATE_BLEND_FUNC(WOBY_GPU_STATE_BLEND_SRC_ALPHA, WOBY_GPU_STATE_BLEND_INV_SRC_ALPHA));
            woby::graphics::setTexture(0, state.textureSampler, texture);
            woby::graphics::setVertexBuffer(0, &vertexBuffer, command.VtxOffset, vertexCount - command.VtxOffset);
            woby::graphics::setIndexBuffer(&indexBuffer, command.IdxOffset, command.ElemCount);
            woby::graphics::submit(viewId, state.program);
        }
    }
}

} // namespace woby::imgui_graphics

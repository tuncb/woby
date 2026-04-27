#include "imgui_bgfx.h"

#include "bgfx_helpers.h"

#include <bx/math.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

namespace woby::imgui_bgfx {
namespace {

struct RendererState {
    bgfx::ViewId viewId = 255;
    bgfx::VertexLayout layout{};
    bgfx::ProgramHandle program = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle textureSampler = BGFX_INVALID_HANDLE;
};

RendererState state;

ImTextureID encodeTexture(bgfx::TextureHandle handle)
{
    return static_cast<ImTextureID>(handle.idx) + 1u;
}

bgfx::TextureHandle decodeTexture(ImTextureID textureId)
{
    if (textureId == ImTextureID_Invalid) {
        return BGFX_INVALID_HANDLE;
    }
    return bgfx::TextureHandle{static_cast<uint16_t>(textureId - 1u)};
}

template <typename HandleT>
bool valid(HandleT handle)
{
    return bgfx::isValid(handle);
}

uint16_t toUint16(int value, const char* name)
{
    if (value < 0 || value > static_cast<int>(std::numeric_limits<uint16_t>::max())) {
        throw std::runtime_error(std::string("Dear ImGui texture ") + name + " is outside bgfx uint16 range.");
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

const bgfx::Memory* copyRgba32TextureRect(ImTextureData* texture, const ImTextureRect& rect, uint16_t& pitch)
{
    const int sourcePitch = texture->GetPitch();
    const int rowBytes = rect.w * texture->BytesPerPixel;
    pitch = toUint16(sourcePitch, "pitch");
    return bgfx::copy(
        texture->GetPixelsAt(rect.x, rect.y),
        textureUploadSize(sourcePitch, rowBytes, rect.h));
}

const bgfx::Memory* copyAlpha8TextureRectAsRgba32(ImTextureData* texture, const ImTextureRect& rect, uint16_t& pitch)
{
    const int targetPitch = rect.w * 4;
    pitch = toUint16(targetPitch, "pitch");

    const bgfx::Memory* memory = bgfx::alloc(static_cast<uint32_t>(targetPitch * rect.h));
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

const bgfx::Memory* copyTextureRectPixels(ImTextureData* texture, const ImTextureRect& rect, uint16_t& pitch)
{
    if (texture->Format == ImTextureFormat_RGBA32) {
        return copyRgba32TextureRect(texture, rect, pitch);
    }
    if (texture->Format == ImTextureFormat_Alpha8) {
        return copyAlpha8TextureRectAsRgba32(texture, rect, pitch);
    }

    throw std::runtime_error("Unsupported Dear ImGui texture format.");
}

void uploadTextureRect(ImTextureData* texture, bgfx::TextureHandle handle, const ImTextureRect& rect)
{
    uint16_t pitch = 0;
    const bgfx::Memory* memory = copyTextureRectPixels(texture, rect, pitch);
    bgfx::updateTexture2D(
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
    const bgfx::TextureHandle handle = decodeTexture(texture->GetTexID());
    if (valid(handle)) {
        bgfx::destroy(handle);
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

    const bgfx::TextureHandle handle = bgfx::createTexture2D(
        toUint16(texture->Width, "width"),
        toUint16(texture->Height, "height"),
        false,
        1,
        bgfx::TextureFormat::RGBA8,
        BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP,
        nullptr);

    if (!valid(handle)) {
        throw std::runtime_error("bgfx failed to create Dear ImGui texture.");
    }

    bgfx::setName(handle, "Dear ImGui Texture");

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
        const bgfx::TextureHandle handle = decodeTexture(texture->GetTexID());
        if (!valid(handle)) {
            createTexture(texture);
            return;
        }

        for (const ImTextureRect& rect : texture->Updates) {
            uploadTextureRect(texture, handle, rect);
        }
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

void init(const std::filesystem::path& assetRoot, bgfx::ViewId viewId)
{
    state.viewId = viewId;

    state.layout
        .begin()
        .add(bgfx::Attrib::Position, 2, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true)
        .end();

    state.textureSampler = bgfx::createUniform("s_tex", bgfx::UniformType::Sampler);
    state.program = loadProgram(assetRoot, "vs_imgui.bin", "fs_imgui.bin");

    ImGuiIO& io = ImGui::GetIO();
    io.BackendRendererName = "woby_imgui_bgfx";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures;

    ImGuiPlatformIO& platformIo = ImGui::GetPlatformIO();
    platformIo.Renderer_TextureMaxWidth = static_cast<int>(bgfx::getCaps()->limits.maxTextureSize);
    platformIo.Renderer_TextureMaxHeight = static_cast<int>(bgfx::getCaps()->limits.maxTextureSize);
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
        bgfx::destroy(state.textureSampler);
    }
    if (valid(state.program)) {
        bgfx::destroy(state.program);
    }

    state = RendererState{};
}

void render(ImDrawData* drawData)
{
    const int32_t framebufferWidth = static_cast<int32_t>(drawData->DisplaySize.x * drawData->FramebufferScale.x);
    const int32_t framebufferHeight = static_cast<int32_t>(drawData->DisplaySize.y * drawData->FramebufferScale.y);
    if (framebufferWidth <= 0 || framebufferHeight <= 0) {
        return;
    }

    updateTextures(drawData);

    bgfx::setViewName(state.viewId, "Dear ImGui");
    bgfx::setViewMode(state.viewId, bgfx::ViewMode::Sequential);
    bgfx::setViewRect(
        state.viewId,
        0,
        0,
        static_cast<uint16_t>(framebufferWidth),
        static_cast<uint16_t>(framebufferHeight));

    float ortho[16];
    const float left = drawData->DisplayPos.x;
    const float right = drawData->DisplayPos.x + drawData->DisplaySize.x;
    const float top = drawData->DisplayPos.y;
    const float bottom = drawData->DisplayPos.y + drawData->DisplaySize.y;
    bx::mtxOrtho(ortho, left, right, bottom, top, 0.0f, 1000.0f, 0.0f, bgfx::getCaps()->homogeneousDepth);
    bgfx::setViewTransform(state.viewId, nullptr, ortho);

    const ImVec2 clipOffset = drawData->DisplayPos;
    const ImVec2 clipScale = drawData->FramebufferScale;

    for (int commandListIndex = 0; commandListIndex < drawData->CmdListsCount; ++commandListIndex) {
        const ImDrawList* commandList = drawData->CmdLists[commandListIndex];
        const auto vertexCount = static_cast<uint32_t>(commandList->VtxBuffer.Size);
        const auto indexCount = static_cast<uint32_t>(commandList->IdxBuffer.Size);

        if (bgfx::getAvailTransientVertexBuffer(vertexCount, state.layout) < vertexCount
            || bgfx::getAvailTransientIndexBuffer(indexCount, sizeof(ImDrawIdx) == 4) < indexCount) {
            break;
        }

        bgfx::TransientVertexBuffer vertexBuffer;
        bgfx::TransientIndexBuffer indexBuffer;
        bgfx::allocTransientVertexBuffer(&vertexBuffer, vertexCount, state.layout);
        bgfx::allocTransientIndexBuffer(&indexBuffer, indexCount, sizeof(ImDrawIdx) == 4);

        std::memcpy(vertexBuffer.data, commandList->VtxBuffer.Data, vertexCount * sizeof(ImDrawVert));
        std::memcpy(indexBuffer.data, commandList->IdxBuffer.Data, indexCount * sizeof(ImDrawIdx));

        for (const ImDrawCmd& command : commandList->CmdBuffer) {
            if (command.UserCallback != nullptr) {
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

            const auto scissorX = static_cast<uint16_t>(std::max(clipRect.x, 0.0f));
            const auto scissorY = static_cast<uint16_t>(std::max(clipRect.y, 0.0f));
            const auto scissorW = static_cast<uint16_t>(std::min(clipRect.z, 65535.0f) - scissorX);
            const auto scissorH = static_cast<uint16_t>(std::min(clipRect.w, 65535.0f) - scissorY);

            const bgfx::TextureHandle texture = decodeTexture(command.GetTexID());
            if (!valid(texture)) {
                continue;
            }

            bgfx::setScissor(scissorX, scissorY, scissorW, scissorH);
            bgfx::setState(
                BGFX_STATE_WRITE_RGB
                | BGFX_STATE_WRITE_A
                | BGFX_STATE_MSAA
                | BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA, BGFX_STATE_BLEND_INV_SRC_ALPHA));
            bgfx::setTexture(0, state.textureSampler, texture);
            bgfx::setVertexBuffer(0, &vertexBuffer, command.VtxOffset, vertexCount - command.VtxOffset);
            bgfx::setIndexBuffer(&indexBuffer, command.IdxOffset, command.ElemCount);
            bgfx::submit(state.viewId, state.program);
        }
    }
}

} // namespace woby::imgui_bgfx

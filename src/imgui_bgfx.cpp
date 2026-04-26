#include "imgui_bgfx.h"

#include "bgfx_helpers.h"

#include <bx/math.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace woby::imgui_bgfx {
namespace {

struct RendererState {
    bgfx::ViewId viewId = 255;
    bgfx::VertexLayout layout{};
    bgfx::ProgramHandle program = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle textureSampler = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle fontTexture = BGFX_INVALID_HANDLE;
};

RendererState state;

ImTextureID encodeTexture(bgfx::TextureHandle handle)
{
    return static_cast<ImTextureID>(static_cast<uintptr_t>(handle.idx));
}

bgfx::TextureHandle decodeTexture(ImTextureID textureId)
{
    return bgfx::TextureHandle{static_cast<uint16_t>(static_cast<uintptr_t>(textureId))};
}

template <typename HandleT>
bool valid(HandleT handle)
{
    return bgfx::isValid(handle);
}

void createFontTexture()
{
    ImGuiIO& io = ImGui::GetIO();

    unsigned char* pixels = nullptr;
    int width = 0;
    int height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

    const bgfx::Memory* memory = bgfx::copy(pixels, static_cast<uint32_t>(width * height * 4));
    state.fontTexture = bgfx::createTexture2D(
        static_cast<uint16_t>(width),
        static_cast<uint16_t>(height),
        false,
        1,
        bgfx::TextureFormat::RGBA8,
        0,
        memory);

    bgfx::setName(state.fontTexture, "Dear ImGui Font Texture");
    io.Fonts->SetTexID(encodeTexture(state.fontTexture));
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
    createFontTexture();

    ImGuiIO& io = ImGui::GetIO();
    io.BackendRendererName = "woby_imgui_bgfx";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
}

void shutdown()
{
    ImGuiIO& io = ImGui::GetIO();
    io.BackendRendererName = nullptr;
    io.BackendFlags &= ~ImGuiBackendFlags_RendererHasVtxOffset;
    io.Fonts->SetTexID(0);

    if (valid(state.fontTexture)) {
        bgfx::destroy(state.fontTexture);
    }
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

            bgfx::TextureHandle texture = state.fontTexture;
            if (command.GetTexID() != 0) {
                texture = decodeTexture(command.GetTexID());
            }
            if (!valid(texture)) {
                texture = state.fontTexture;
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

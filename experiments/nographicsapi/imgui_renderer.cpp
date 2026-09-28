#include "renderer.h"
#include <algorithm>
#include <cstring>

namespace woby::ng {
using namespace gpu;
namespace {
void retire(Renderer& r, UiTexture* old)
{
    if (!old) return;
    std::erase(r.uiTextures,old);
    r.deletes->defer(r.submitted,[&r,old]() noexcept {
        destroyImage(r,old->image);
        r.freeDescriptors.push_back(old->descriptor);
        delete old;
    });
}
}
void prepareUiTextures(Renderer& r, Frame& f, CommandBuffer* cmd, ImDrawData* data)
{
    if (!data || !data->Textures) return;
    for (auto* texture : *data->Textures) {
        if (texture->Status == ImTextureStatus_WantDestroy) {
            retire(r,static_cast<UiTexture*>(texture->BackendUserData));
            texture->BackendUserData = nullptr;
            texture->SetTexID(ImTextureID_Invalid);
            texture->SetStatus(ImTextureStatus_Destroyed);
            ++r.uiDestroys;
            continue;
        }
        if (texture->Status != ImTextureStatus_WantCreate && texture->Status != ImTextureStatus_WantUpdates) continue;
        require(texture->Width > 0 && texture->Height > 0 && texture->Width <= int(maxExtent) && texture->Height <= int(maxExtent),
            "ImGui texture exceeds prototype limits");
        require(texture->Format == ImTextureFormat_RGBA32 || texture->Format == ImTextureFormat_Alpha8,"Unsupported ImGui texture format");
        require(!r.freeDescriptors.empty(),"Prototype descriptor heap exhausted");
        // Copy-on-write preserves both old pixels and descriptor slots for in-flight frames.
        // Full copies simplify atlas growth/partial update correctness in this experiment.
        auto* image = new UiTexture;
        image->owner = texture;
        image->descriptor = r.freeDescriptors.back(); r.freeDescriptors.pop_back();
        image->image = createImage(r,cmd,static_cast<uint32_t>(texture->Width),static_cast<uint32_t>(texture->Height),
            Format::rgba8_unorm,TextureUsage::sampled | TextureUsage::transfer_destination);
        const auto pixelCount = uint64_t(texture->Width)*texture->Height;
        auto upload = allocate<byte>(f,pixelCount*4);
        if (texture->Format == ImTextureFormat_RGBA32) std::memcpy(upload.cpu,texture->Pixels,static_cast<size_t>(pixelCount*4));
        else {
            auto* rgba = reinterpret_cast<uint8_t*>(upload.cpu);
            for (size_t i = 0; i < pixelCount; ++i) {
                rgba[i*4] = rgba[i*4+1] = rgba[i*4+2] = 255;
                rgba[i*4+3] = texture->Pixels[i];
            }
        }
        copy_memory_to_texture(cmd,{upload.gpu,pixelCount*4},image->image.allocation.texture);
        write_texture_descriptor(r.descriptors,image->descriptor,image->image.allocation.texture,TextureDescriptorType::sampled);
        retire(r,static_cast<UiTexture*>(texture->BackendUserData));
        r.uiTextures.push_back(image);
        if (texture->Status == ImTextureStatus_WantCreate) ++r.uiCreates; else ++r.uiUpdates;
        texture->BackendUserData = image;
        texture->SetTexID(static_cast<ImTextureID>(image->descriptor+1));
        texture->SetStatus(ImTextureStatus_OK);
    }
    barrier(cmd,Stage::transfer,Access::transfer_write,Stage::fragment,Access::shader_read);
}
void drawUi(Renderer& r, Frame& f, CommandBuffer* cmd, ImDrawData* data)
{
    if (!data || data->DisplaySize.x <= 0 || data->DisplaySize.y <= 0) return;
    for (const auto* list : data->CmdLists) {
        if (list->VtxBuffer.empty() || list->IdxBuffer.empty()) continue;
        const auto vertices = allocate<UiVertex>(f,static_cast<uint64_t>(list->VtxBuffer.Size));
        const auto indices = allocate<ImDrawIdx>(f,static_cast<uint64_t>(list->IdxBuffer.Size));
        std::memcpy(vertices.cpu,list->VtxBuffer.Data,size_t(list->VtxBuffer.Size)*sizeof(ImDrawVert));
        std::memcpy(indices.cpu,list->IdxBuffer.Data,size_t(list->IdxBuffer.Size)*sizeof(ImDrawIdx));
        for (const auto& draw : list->CmdBuffer) {
            if (draw.UserCallback) {
                if (draw.UserCallback != ImDrawCallback_ResetRenderState) draw.UserCallback(list,&draw);
                continue;
            }
            const auto clip = clipRect(draw.ClipRect.x,draw.ClipRect.y,draw.ClipRect.z,draw.ClipRect.w,
                data->DisplayPos.x,data->DisplayPos.y,data->FramebufferScale.x,data->FramebufferScale.y,f.targets.width,f.targets.height);
            if (clip.width == 0 || clip.height == 0) { ++r.uiClippedDraws; continue; }
            if (draw.VtxOffset != 0) ++r.uiOffsetDraws;
            const auto texture = draw.GetTexID();
            require(texture != ImTextureID_Invalid && texture <= 256,"Invalid ImGui texture descriptor");
            auto root = allocate<UiRoot>(f);
            *root.cpu = {.vertices = vertices.gpu+draw.VtxOffset,
                .scale = {2.0f/data->DisplaySize.x,2.0f/data->DisplaySize.y},
                .translate = {-1.0f-data->DisplayPos.x*2.0f/data->DisplaySize.x,-1.0f-data->DisplayPos.y*2.0f/data->DisplaySize.y},
                .texture = static_cast<uint32_t>(texture-1)};
            bind_pso(cmd,r.uiPso);
            set_depth_stencil(cmd,{});
            set_scissor(cmd,{clip.x,clip.y,clip.width,clip.height});
            draw_indexed(cmd,root.gpu,gpu_range(indices),sizeof(ImDrawIdx) == 2 ? IndexType::uint16 : IndexType::uint32,
                draw.ElemCount,1,draw.IdxOffset);
        }
    }
}
void destroyUiTextures(Renderer& r)
{
    for (auto* texture : r.uiTextures) {
        texture->owner->BackendUserData = nullptr;
        texture->owner->SetTexID(ImTextureID_Invalid);
        texture->owner->SetStatus(ImTextureStatus_Destroyed);
        destroyImage(r,texture->image);
        delete texture;
    }
    r.uiTextures.clear();
}
}

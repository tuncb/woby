#pragma once
#include "logic.h"
#include "shared.h"
#include <NoGraphicsAPI/NoGraphicsAPI.hpp>
#include <NoGraphicsAPIUtility/bump_allocator.hpp>
#include <NoGraphicsAPIUtility/delete_queue.hpp>
#include <NoGraphicsAPIUtility/texture_allocator.hpp>
#include <NoGraphicsAPIUtility/upload_queue.hpp>
#include <imgui.h>
#include <array>
#include <memory>
#include <vector>

namespace woby::ng {
constexpr uint32_t maxExtent = 2048;
constexpr size_t frameCount = 3;
constexpr uint64_t imageOffset = 256;
struct Image {
    gpu::PlacedTexture allocation{};
    gpu::RenderView* view = nullptr;
};
struct Targets {
    Image color, ids, depth, resolved, output;
    uint32_t width = 0, height = 0, samples = 0;
};
struct Frame {
    gpu::CommandPool* pool = nullptr;
    gpu::GpuHeap memory{}, readback{};
    std::unique_ptr<gpu::BumpAllocator> allocator;
    Targets targets;
    uint64_t completion = 0, epoch = 0, sequence = 0;
    bool pending = false, captured = false;
    uint32_t descriptorBase = 0;
    PickResult result{};
};
struct UiTexture {
    Image image;
    uint32_t descriptor = 0;
    ImTextureData* owner = nullptr;
};
struct Renderer {
    gpu::Device* device = nullptr;
    gpu::TimelineSemaphore* timeline = nullptr;
    gpu::TextureHeap textureHeap{};
    gpu::TextureDescriptorHeap* descriptors = nullptr;
    gpu::SamplerDescriptorHeap* samplers = nullptr;
    std::unique_ptr<gpu::TextureAllocator> textures;
    std::unique_ptr<gpu::UploadQueue> uploads;
    std::unique_ptr<gpu::DeleteQueue> deletes;
    std::array<Frame,frameCount> frames;
    std::array<gpu::PSO*,2> scenePsos{}, markerPsos{}, linePsos{}, pickPsos{};
    gpu::PSO* compositePso = nullptr;
    gpu::PSO* presentPso = nullptr;
    gpu::PSO* uiPso = nullptr;
    gpu::GpuHeap geometry{};
    std::array<MeshVertex*,3> vertices{};
    gpu::GpuRange triangleIndices{}, edgeIndices{};
    std::vector<uint32_t> freeDescriptors;
    std::vector<UiTexture*> uiTextures;
    uint64_t submitted = 0, nextSequence = 0;
    size_t nextFrame = 0;
    bool windowed = false;
    uint32_t retiredMeshes = 0, uiCreates = 0, uiUpdates = 0, uiDestroys = 0;
    uint32_t uiOffsetDraws = 0, uiClippedDraws = 0;
    Selection selection;
};
struct RenderOptions {
    uint32_t width = 640, height = 480, samples = 4;
    Pixel cursor{320,240};
    bool capture = false, highlight = true;
};
void require(bool success, const char* message);
void initialize(Renderer& renderer, void* window);
void shutdown(Renderer& renderer);
void uploadFixture(Renderer& renderer, const Fixture& fixture);
void collect(Renderer& renderer);
Frame& render(Renderer& renderer, const Fixture& fixture, RenderOptions options, ImDrawData* ui = nullptr);
void finish(Renderer& renderer, Frame& frame);
std::vector<uint8_t> capturedPixels(const Frame& frame);
void savePng(const Frame& frame, const std::filesystem::path& path);
Image createImage(Renderer& renderer, gpu::CommandBuffer* commands, uint32_t width, uint32_t height,
    gpu::Format format, gpu::TextureUsage usage, uint32_t samples = 1);
void destroyImage(Renderer& renderer, Image& image);
void prepareUiTextures(Renderer& renderer, Frame& frame, gpu::CommandBuffer* commands, ImDrawData* data);
void drawUi(Renderer& renderer, Frame& frame, gpu::CommandBuffer* commands, ImDrawData* data);
void destroyUiTextures(Renderer& renderer);

template<typename T> gpu::GpuCpuRange<T> allocate(Frame& frame, uint64_t count = 1) {
    auto result = frame.allocator->allocate<T>(count);
    require(result.cpu != nullptr, "Prototype frame arena exhausted (16 MiB)");
    return result;
}
}

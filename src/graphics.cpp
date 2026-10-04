#include "graphics.h"
#include "graphics_diagnostics.h"
#include "frame_pacing.h"
#include "root.h"
#include <NoGraphicsAPI/NoGraphicsAPI.hpp>
#include <NoGraphicsAPIUtility/bump_allocator.hpp>
#include <NoGraphicsAPIUtility/delete_queue.hpp>
#include <NoGraphicsAPIUtility/texture_allocator.hpp>
#include <NoGraphicsAPIUtility/upload_queue.hpp>
#include <SDL3/SDL.h>
#include <bx/math.h>
#if defined(__APPLE__)
#include <SDL3/SDL_metal.h>
#else
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>
#endif
#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace woby::graphics
{
namespace
{
constexpr size_t framesInFlight = 3;
constexpr uint64_t arenaPageBytes = 4ull * 1024 * 1024, arenaLimit = 256ull * 1024 * 1024;
constexpr uint32_t descriptorCapacity = 16384;
using Clock = std::chrono::steady_clock;
int64_t ticks()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}
void require(bool value, const char *message)
{
    if (!value)
        throw std::runtime_error(message);
}

struct Buffer
{
    gpu::GpuHeap heap{};
    uint32_t bytes = 0;
    uint16_t stride = 0;
    bool index32 = false;
};
struct TexturePage
{
    gpu::TextureHeap heap{};
    std::unique_ptr<gpu::TextureAllocator> allocator;
};
struct Image
{
    std::shared_ptr<TexturePage> page;
    gpu::PlacedTexture placed{};
    gpu::RenderView *view = nullptr;
    gpu::Format format = gpu::Format::undefined;
    uint32_t sampled = kInvalidHandle, storage = kInvalidHandle, samples = 1;
};
struct Texture
{
    std::shared_ptr<Image> image, multisample;
    uint16_t width = 0, height = 0;
    TextureFormat::Enum format = TextureFormat::RGBA8;
    uint64_t flags = 0;
    std::vector<uint8_t> pixels;
};
struct Framebuffer
{
    std::vector<std::shared_ptr<Texture>> textures;
};
struct Shader
{
    std::vector<uint32_t> code;
    std::string entry;
    uint32_t bytes = 0;
};
struct Program
{
    std::shared_ptr<Shader> vertex, fragment, compute;
    uint32_t id = 0;
};
struct Uniform
{
    std::string name;
    UniformType::Enum type = UniformType::Vec4;
    uint16_t count = 1;
};
struct Geometry
{
    std::shared_ptr<Buffer> owner;
    void *gpu = nullptr;
    uint32_t bytes = 0, count = 0;
    uint16_t stride = 0;
    bool index32 = false;
};
struct Encoder
{
    WobyRoot root{};
    std::array<float, 16> model{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    Geometry vertices, indices, points, freeform;
    uint32_t vertexCount = 0, instances = 1;
    uint64_t state = WOBY_GPU_STATE_WRITE_RGB | WOBY_GPU_STATE_WRITE_A;
    gpu::Scissor scissor{};
    bool hasScissor = false;
    std::array<std::shared_ptr<Texture>, 2> textures;
    std::shared_ptr<Texture> storage;
};
struct View
{
    std::shared_ptr<Framebuffer> framebuffer;
    uint16_t x = 0, y = 0, width = 1, height = 1, clearFlags = 0;
    float depth = 1;
    bool reversedDepth = false;
    std::array<uint32_t, 2> colors{0x20242aff, 0};
    std::array<float, 16> view{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    std::array<float, 16> projection{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
};
enum class OperationType
{
    draw,
    compute,
    blit,
    touch
};
struct Operation
{
    ViewId view = 0;
    OperationType type = OperationType::touch;
    Encoder encoder;
    std::shared_ptr<Program> program;
    std::shared_ptr<Texture> source, destination;
    gpu::uint32x3 groups{1, 1, 1};
    uint16_t x = 0, y = 0, sourceX = 0, sourceY = 0, width = 0, height = 0;
};
struct ArenaPage
{
    gpu::GpuHeap heap{};
    std::unique_ptr<gpu::BumpAllocator> allocator;
};
struct Frame
{
    gpu::CommandPool *pool = nullptr;
    gpu::CommandBuffer *preparations = nullptr;
    uint64_t completion = 0, arenaUsed = 0;
    size_t activeArena = 0;
    std::vector<ArenaPage> arenas;
    std::vector<std::unique_ptr<uint8_t[]>> cpuAllocations;
    std::vector<Operation> operations;
    std::vector<std::shared_ptr<void>> retained;
    std::shared_ptr<Framebuffer> output;
    uint32_t width = 0, height = 0, samples = 0;
    std::array<uint64_t, 2> timestamps{};
};
struct ReadRequest
{
    std::shared_ptr<Texture> texture;
    void *destination = nullptr;
    uint32_t ready = 0;
    std::shared_ptr<Buffer> buffer;
};
struct Readback
{
    gpu::GpuHeap heap{};
    void *destination = nullptr;
    uint64_t completion = 0;
    uint32_t ready = 0, bytes = 0;
};
using PipelineKey = std::array<uint64_t, 7>;
struct Context
{
    gpu::Device *device = nullptr;
    SDL_Window *window = nullptr;
    FramePacingState pacing;
#if defined(__APPLE__)
    SDL_MetalView metalView = nullptr;
#endif
    gpu::TimelineSemaphore *timeline = nullptr;
    gpu::TextureDescriptorHeap *descriptors = nullptr;
    gpu::SamplerDescriptorHeap *samplers = nullptr;
    std::unique_ptr<gpu::UploadQueue> uploads;
    std::unique_ptr<gpu::DeleteQueue> deletes;
    Caps caps;
    Stats stats;
    uint32_t width = 1, height = 1, flags = 0, frameNumber = 0;
    uint64_t submitted = 0, textureBytes = 0;
    bool windowed = false, frameOpen = false;
    std::array<Frame, framesInFlight> frames;
    std::array<View, 256> views;
    std::array<uint32_t, 256> palette{};
    Encoder encoder;
    std::vector<std::shared_ptr<Buffer>> vertexBuffers, indexBuffers;
    std::vector<std::shared_ptr<Texture>> textures;
    std::vector<std::shared_ptr<Framebuffer>> framebuffers;
    std::vector<std::shared_ptr<Shader>> shaders;
    std::vector<std::shared_ptr<Program>> programs;
    std::vector<std::shared_ptr<Uniform>> uniforms;
    std::vector<std::shared_ptr<TexturePage>> pages;
    std::vector<uint32_t> freeDescriptors;
    std::vector<ReadRequest> reads;
    std::vector<Readback> readbacks;
    std::unique_ptr<std::vector<std::shared_ptr<void>>> trash = std::make_unique<std::vector<std::shared_ptr<void>>>();
    std::map<PipelineKey, gpu::PSO *> pipelines;
    std::map<uint32_t, gpu::PSO *> computePipelines;
};
std::unique_ptr<Context> context;
std::string initError;
Context &state()
{
    require(bool(context), "NoGraphicsAPI renderer is not initialized");
    return *context;
}
bool noop()
{
    return state().caps.rendererType == RendererType::Noop;
}
Frame &current()
{
    auto &c = state();
    return c.frames[c.frameNumber % framesInFlight];
}
bool reached(uint32_t now, uint32_t target)
{
    return now - target < (uint32_t{1} << 31);
}

template <typename T, typename Tag>
std::shared_ptr<T> resource(const std::vector<std::shared_ptr<T>> &items, Handle<Tag> handle)
{
    require(handle.idx < items.size() && bool(items[handle.idx]), "Invalid or destroyed graphics handle");
    return items[handle.idx];
}
template <typename H, typename T> H insert(std::vector<std::shared_ptr<T>> &items, std::shared_ptr<T> value)
{
    require(items.size() < kInvalidHandle, "Graphics handle namespace exhausted");
    // IDs never reuse within one device, so stale handles cannot alias new resources.
    H handle{static_cast<uint32_t>(items.size())};
    items.push_back(std::move(value));
    return handle;
}
template <typename T, typename H> void release(std::vector<std::shared_ptr<T>> &items, H handle)
{
    if (!isValid(handle))
        return;
    auto value = resource(items, handle);
    state().trash->push_back(std::move(value));
    items[handle.idx].reset();
}
void freeMemory(const Memory *memory)
{
    if (!memory)
        return;
    if (memory->release)
        memory->release(memory->data, memory->user);
    if (memory->owned)
        delete[] memory->data;
    delete memory;
}
using MemoryOwner = std::unique_ptr<const Memory, decltype(&freeMemory)>;
void collectReadbacks(bool force)
{
    auto &c = state();
    std::erase_if(c.readbacks,
                  [&](Readback &read)
                  {
                      if (!force && !reached(c.frameNumber, read.ready))
                          return false;
                      gpu::wait_timeline({c.timeline, read.completion});
                      std::memcpy(read.destination, read.heap.range.cpu, read.bytes);
                      gpu::destroy_gpu_heap(read.heap);
                      return true;
                  });
}
void beginFrame()
{
    auto &c = state();
    if (c.frameOpen)
        return;
    auto &f = current();
    if (!noop())
    {
        gpu::wait_timeline({c.timeline, f.completion});
        gpu::read_timestamps(f.pool);
        if (f.timestamps[1] > f.timestamps[0])
        {
            c.stats.gpuTimeBegin =
                static_cast<int64_t>(double(f.timestamps[0]) * gpu::get_device_caps(c.device).timestamp_period_ns);
            c.stats.gpuTimeEnd =
                static_cast<int64_t>(double(f.timestamps[1]) * gpu::get_device_caps(c.device).timestamp_period_ns);
        }
        c.deletes->tick();
        c.uploads->reclaim();
        gpu::reset_command_pool(f.pool);
    }
    f.operations.clear();
    f.retained.clear();
    f.cpuAllocations.clear();
    f.arenaUsed = 0;
    f.activeArena = 0;
    for (auto &page : f.arenas)
        page.allocator->reset();
    if (!noop())
        f.preparations = gpu::begin_commands(f.pool);
    c.frameOpen = true;
}
gpu::GpuCpuRange<byte> allocate(uint64_t bytes)
{
    beginFrame();
    auto &f = current();
    bytes = (bytes + 15) & ~uint64_t{15};
    require(bytes > 0 && bytes <= arenaLimit - f.arenaUsed, "NoGraphicsAPI frame upload arena exceeds 256 MiB");
    f.arenaUsed += bytes;
    if (noop())
    {
        auto data = std::make_unique<uint8_t[]>(static_cast<size_t>(bytes));
        auto *pointer = data.get();
        f.cpuAllocations.push_back(std::move(data));
        return {pointer, pointer, bytes};
    }
    while (f.activeArena < f.arenas.size())
    {
        auto result = f.arenas[f.activeArena].allocator->allocate(bytes);
        if (result.cpu)
            return result;
        ++f.activeArena;
    }
    ArenaPage page;
    page.heap = gpu::create_gpu_heap(state().device, std::max(arenaPageBytes, bytes));
    require(page.heap.range.cpu != nullptr, "Cannot allocate GPU frame arena");
    page.allocator = std::make_unique<gpu::BumpAllocator>(page.heap.range);
    f.arenas.push_back(std::move(page));
    return f.arenas.back().allocator->allocate(bytes);
}
template <typename T> gpu::GpuCpuRange<T> allocateRoot(const T &value)
{
    auto range = allocate(sizeof(T));
    std::memcpy(range.cpu, &value, sizeof(T));
    return {reinterpret_cast<T *>(range.cpu), reinterpret_cast<T *>(range.gpu), sizeof(T)};
}
gpu::Format nativeFormat(TextureFormat::Enum format)
{
    switch (format)
    {
    case TextureFormat::BGRA8:
        return gpu::Format::bgra8_unorm;
    case TextureFormat::RGBA8:
        return gpu::Format::rgba8_unorm;
    case TextureFormat::RGBA32F:
        return gpu::Format::rgba32_float;
    case TextureFormat::D24S8:
        return gpu::Format::d32_float;
    }
    throw std::runtime_error("Unknown graphics texture format");
}
uint32_t pixelBytes(TextureFormat::Enum format)
{
    return format == TextureFormat::RGBA32F ? 16u : 4u;
}
uint32_t descriptor()
{
    auto &free = state().freeDescriptors;
    require(!free.empty(), "NoGraphicsAPI texture descriptor heap exhausted");
    auto result = free.back();
    free.pop_back();
    return result;
}
std::shared_ptr<Image> createImage(uint16_t width, uint16_t height, gpu::Format format, gpu::TextureUsage usage,
                                   uint32_t samples)
{
    beginFrame();
    auto &c = state();
    auto image = std::shared_ptr<Image>(new Image,
                                        [](Image *value)
                                        {
                                            if (value->view)
                                                gpu::destroy_render_view(value->view);
                                            if (value->placed.texture)
                                                value->page->allocator->free(value->placed);
                                            if (context)
                                            {
                                                if (value->sampled != kInvalidHandle)
                                                    context->freeDescriptors.push_back(value->sampled);
                                                if (value->storage != kInvalidHandle)
                                                    context->freeDescriptors.push_back(value->storage);
                                            }
                                            delete value;
                                        });
    image->format = format;
    image->samples = samples;
    if (noop())
        return image;
    const gpu::TextureDesc desc{
        .extent = {width, height, 1}, .format = format, .usage = usage, .sample_count = samples};
    for (const auto &page : c.pages)
    {
        image->placed = page->allocator->allocate(current().preparations, desc);
        if (image->placed.texture)
        {
            image->page = page;
            break;
        }
    }
    if (!image->placed.texture)
    {
        // Reclaim empty pages after retired frames have released their images.
        std::erase_if(c.pages,
                      [&](const std::shared_ptr<TexturePage> &page)
                      {
                          if (page.use_count() != 1)
                              return false;
                          c.textureBytes -= page->heap.size;
                          return true;
                      });
        const auto required = gpu::get_texture_size_align(c.device, desc);
        const uint64_t bytes = std::max<uint64_t>(64ull * 1024 * 1024, (required.size + required.align - 1) /
                                                                           required.align * required.align);
        require(bytes <= 2ull * 1024 * 1024 * 1024 - c.textureBytes,
                "NoGraphicsAPI texture allocation exceeds the 2 GiB renderer budget");
        auto page = std::shared_ptr<TexturePage>(new TexturePage,
                                                 [](TexturePage *value)
                                                 {
                                                     value->allocator.reset();
                                                     gpu::destroy_texture_heap(value->heap);
                                                     delete value;
                                                 });
        page->heap = gpu::create_texture_heap(c.device, bytes);
        require(page->heap.owner != nullptr, "Cannot allocate texture heap");
        page->allocator = std::make_unique<gpu::TextureAllocator>(c.device, page->heap, 1024);
        image->placed = page->allocator->allocate(current().preparations, desc);
        require(image->placed.texture != nullptr, "Texture does not fit its heap");
        image->page = page;
        c.pages.push_back(page);
        c.textureBytes += bytes;
    }
    const auto has = [&](gpu::TextureUsage bit)
    { return (static_cast<uint32_t>(usage) & static_cast<uint32_t>(bit)) != 0; };
    if (has(gpu::TextureUsage::color_attachment) || has(gpu::TextureUsage::depth_stencil_attachment))
        image->view = gpu::create_render_view(image->placed.texture);
    if (has(gpu::TextureUsage::sampled))
    {
        image->sampled = descriptor();
        gpu::write_texture_descriptor(c.descriptors, image->sampled, image->placed.texture,
                                      gpu::TextureDescriptorType::sampled);
    }
    if (has(gpu::TextureUsage::storage))
    {
        image->storage = descriptor();
        gpu::write_texture_descriptor(c.descriptors, image->storage, image->placed.texture,
                                      gpu::TextureDescriptorType::storage);
    }
    current().retained.push_back(image);
    return image;
}
std::shared_ptr<Texture> createTexture(uint16_t width, uint16_t height, TextureFormat::Enum format, uint64_t flags)
{
    require(isTextureValid(0, false, 1, format, flags) && width && height &&
                width <= state().caps.limits.maxTextureSize && height <= state().caps.limits.maxTextureSize,
            "Texture format or dimensions are unsupported");
    auto result = std::make_shared<Texture>();
    result->width = width;
    result->height = height;
    result->format = format;
    result->flags = flags;
    const bool depth = format == TextureFormat::D24S8;
    const bool msaa = (flags & WOBY_GPU_TEXTURE_RT_MSAA_X4) == WOBY_GPU_TEXTURE_RT_MSAA_X4;
    auto usage = depth ? gpu::TextureUsage::depth_stencil_attachment
                       : gpu::TextureUsage::sampled | gpu::TextureUsage::transfer_source |
                             gpu::TextureUsage::transfer_destination;
    if (!depth && (flags & WOBY_GPU_TEXTURE_RT))
        usage = usage | gpu::TextureUsage::color_attachment;
    if (flags & WOBY_GPU_TEXTURE_COMPUTE_WRITE)
        usage = usage | gpu::TextureUsage::storage;
    if (msaa)
    {
        auto multiUsage = depth ? gpu::TextureUsage::depth_stencil_attachment
                                : gpu::TextureUsage::color_attachment | gpu::TextureUsage::sampled;
        result->multisample = createImage(width, height, nativeFormat(format), multiUsage, 4);
        if (depth || (flags & WOBY_GPU_TEXTURE_MSAA_SAMPLE))
            result->image = result->multisample;
        else
            result->image = createImage(width, height, nativeFormat(format), usage, 1);
    }
    else
        result->image = createImage(width, height, nativeFormat(format), usage, 1);
    return result;
}
void uploadTexture(const std::shared_ptr<Texture> &texture)
{
    if (noop())
        return;
    // Each update has its own image and descriptor; previously recorded draws
    // retain the previous version. Partial atlas updates are composed on the CPU.
    auto memory = allocate(texture->pixels.size());
    std::memcpy(memory.cpu, texture->pixels.data(), texture->pixels.size());
    gpu::copy_memory_to_texture(current().preparations, {memory.gpu, texture->pixels.size()},
                                texture->image->placed.texture);
}
std::shared_ptr<Buffer> createBuffer(const Memory *source, uint16_t stride, bool index32)
{
    MemoryOwner memory(source, freeMemory);
    require(memory && memory->size > 0, "Cannot create an empty GPU buffer");
    require(stride > 0 && memory->size % stride == 0, "GPU buffer size must match its element stride");
    auto result = std::shared_ptr<Buffer>(new Buffer,
                                          [](Buffer *value)
                                          {
                                              gpu::destroy_gpu_heap(value->heap);
                                              delete value;
                                          });
    result->bytes = memory->size;
    result->stride = stride;
    result->index32 = index32;
    if (!noop())
    {
        const auto heapBytes = (uint64_t(memory->size) + 15) & ~uint64_t{15};
#if defined(__APPLE__)
        result->heap = gpu::create_gpu_heap(state().device, heapBytes, gpu::MemoryType::gpu_only);
#else
        gpu::HeapAllocationFailure failure;
        result->heap = gpu::try_create_gpu_heap(state().device, heapBytes, gpu::MemoryType::gpu_only, failure);
        if (!result->heap.owner) {
            const char* reason = failure.api_result == VK_ERROR_OUT_OF_DEVICE_MEMORY ? " (out of device memory)"
                : failure.api_result == VK_ERROR_OUT_OF_HOST_MEMORY ? " (out of host memory)"
                : failure.api_result == VK_ERROR_DEVICE_LOST ? " (device lost)" : "";
            throw std::runtime_error("Cannot allocate GPU geometry: requested " + std::to_string(memory->size)
                + " bytes; " + (failure.operation ? failure.operation : "unknown operation")
                + " returned VkResult " + std::to_string(failure.api_result) + reason
                + " (allocation " + std::to_string(failure.allocation_bytes)
                + " bytes, memory type " + std::to_string(failure.memory_type) + ").");
        }
#endif
        require(result->heap.owner != nullptr, "Cannot allocate GPU geometry");
        // Retain the destination before submitting any upload chunks. A later
        // exception must not free memory that an earlier chunk still writes.
        state().trash->push_back(result);
        const uint64_t paddedBytes = (uint64_t(memory->size) + 3) & ~uint64_t{3};
        if (paddedBytes == memory->size)
            state().uploads->upload_buffer({result->heap.range.gpu, paddedBytes}, {memory->data, paddedBytes});
        else
        {
            std::vector<byte> padded(static_cast<size_t>(paddedBytes));
            std::memcpy(padded.data(), memory->data, memory->size);
            state().uploads->upload_buffer({result->heap.range.gpu, paddedBytes}, {padded.data(), paddedBytes});
        }
    }
    return result;
}
void matrix(float4x4 &destination, const float *columnMajor)
{
    for (size_t row = 0; row < 4; ++row)
        for (size_t col = 0; col < 4; ++col)
            destination.rows[row][col] = columnMajor[col * 4 + row];
}
void allBarrier(gpu::CommandBuffer *commands)
{
    using enum gpu::Access;
    gpu::barrier(commands, gpu::Stage::all_commands, transfer_write | shader_write | color_write | depth_stencil_write,
                 gpu::Stage::all_commands,
                 transfer_read | transfer_write | shader_read | shader_write | color_read | color_write |
                     depth_stencil_read | depth_stencil_write | index_read);
}
std::shared_ptr<Image> attachment(const std::shared_ptr<Texture> &texture)
{
    return texture->multisample ? texture->multisample : texture->image;
}
gpu::ClearColor clearColor(uint32_t rgba)
{
    return {float(rgba >> 24) / 255, float((rgba >> 16) & 255) / 255, float((rgba >> 8) & 255) / 255,
            float(rgba & 255) / 255};
}
struct Attachments
{
    std::array<gpu::ColorAttachment, 2> colors{};
    std::array<gpu::ColorTargetDesc, 2> formats{};
    uint32_t count = 0, samples = 1;
    gpu::DepthAttachment depth;
    gpu::Format depthFormat = gpu::Format::undefined;
};
Attachments attachments(const Framebuffer &framebuffer, const View &view, bool first)
{
    Attachments result;
    for (const auto &texture : framebuffer.textures)
    {
        auto image = attachment(texture);
        result.samples = image->samples;
        if (texture->format == TextureFormat::D24S8)
        {
            result.depth = {.render_view = image->view,
                            .load = first && (view.clearFlags & WOBY_GPU_CLEAR_DEPTH) ? gpu::LoadOp::clear
                                                                                      : gpu::LoadOp::load,
                            .clear = view.reversedDepth ? 1.0f - view.depth : view.depth};
            result.depthFormat = image->format;
        }
        else
        {
            require(result.count < result.colors.size(), "Woby supports at most two color attachments");
            auto &color = result.colors[result.count];
            color = {.render_view = image->view,
                     .load = first && (view.clearFlags & WOBY_GPU_CLEAR_COLOR) ? gpu::LoadOp::clear : gpu::LoadOp::load,
                     .clear = clearColor(view.colors[result.count]),
                     .resolve_view = texture->image != image ? texture->image->view : nullptr};
            result.formats[result.count++].format = image->format;
        }
    }
    return result;
}
gpu::ShaderStage shaderStage(const std::shared_ptr<Shader> &shader)
{
    return {.code = {reinterpret_cast<const byte *>(shader->code.data()), shader->bytes},
            .entry_point = shader->entry.c_str()};
}
gpu::PSO *pipeline(const Operation &op, const Attachments &targets)
{
    auto &c = state();
    const auto flags = op.encoder.state;
    const uint64_t mask = WOBY_GPU_STATE_WRITE_RGB | WOBY_GPU_STATE_WRITE_A | WOBY_GPU_STATE_BLEND_ALPHA |
                          WOBY_GPU_STATE_PT_LINES | WOBY_GPU_STATE_PT_TRISTRIP;
    PipelineKey key{op.program->id,
                    flags & mask,
                    targets.samples,
                    targets.count,
                    static_cast<uint64_t>(targets.formats[0].format),
                    static_cast<uint64_t>(targets.formats[1].format),
                    static_cast<uint64_t>(targets.depthFormat)};
    if (auto it = c.pipelines.find(key); it != c.pipelines.end())
        return it->second;
    auto colors = targets.formats;
    colors[0].write_mask =
        static_cast<uint8_t>(((flags & WOBY_GPU_STATE_WRITE_RGB) ? 7 : 0) | ((flags & WOBY_GPU_STATE_WRITE_A) ? 8 : 0));
    if (flags & WOBY_GPU_STATE_BLEND_ALPHA)
        colors[0].blend = {.enabled = true,
                           .color = {gpu::BlendFactor::source_alpha, gpu::BlendFactor::one_minus_source_alpha},
                           .alpha = {gpu::BlendFactor::one, gpu::BlendFactor::one_minus_source_alpha}};
    // Marker provenance is always unblended, including transparent surfaces.
    colors[1].blend.enabled = false;
    auto *pso = gpu::create_graphics_pso(
        c.device, {.vertex = shaderStage(op.program->vertex),
                   .fragment = shaderStage(op.program->fragment),
                   .color_targets = {colors.data(), targets.count},
                   .depth_format = targets.depthFormat,
                   .sample_count = targets.samples,
                   .topology = (flags & WOBY_GPU_STATE_PT_LINES)      ? gpu::PrimitiveTopology::lines
                               : (flags & WOBY_GPU_STATE_PT_TRISTRIP) ? gpu::PrimitiveTopology::triangle_strip
                                                                      : gpu::PrimitiveTopology::triangles});
    require(pso != nullptr, "NoGraphicsAPI graphics pipeline creation failed");
    c.pipelines.emplace(key, pso);
    return pso;
}
WobyRoot rootData(const Encoder &encoder, const View &view)
{
    auto root = encoder.root;
    float modelView[16], modelViewProjection[16];
    bx::mtxMul(modelView, encoder.model.data(), view.view.data());
    bx::mtxMul(modelViewProjection, modelView, view.projection.data());
    matrix(root.modelViewProj, modelViewProjection);
    matrix(root.model, encoder.model.data());
    root.vertices = static_cast<float *>(encoder.vertices.gpu);
    root.stride = encoder.vertices.stride / 4;
    root.pointIds = static_cast<uint32_t *>(encoder.points.gpu);
    root.freeform = static_cast<float *>(encoder.freeform.gpu);
    for (size_t i = 0; i < encoder.textures.size(); ++i)
        if (encoder.textures[i])
        {
            const auto slot = encoder.textures[i]->image->sampled;
            require(slot != kInvalidHandle, "Texture has no sampled descriptor");
            if (i == 0)
                root.texture0 = slot;
            else
                root.texture1 = slot;
        }
    if (encoder.storage)
        root.image1 = encoder.storage->image->storage;
    return root;
}
void drawOperation(gpu::CommandBuffer *commands, const Operation &op, const View &view, const Attachments &target)
{
    const auto &encoder = op.encoder;
    const auto root = allocateRoot(rootData(encoder, view));
    gpu::bind_pso(commands, pipeline(op, target));
    const auto flags = encoder.state;
    const bool test = (flags & (WOBY_GPU_STATE_DEPTH_TEST_LESS | WOBY_GPU_STATE_DEPTH_TEST_LEQUAL)) != 0;
    gpu::set_depth_stencil(commands,
                           {.depth_test = test,
                            .depth_write = (flags & WOBY_GPU_STATE_WRITE_Z) != 0,
                            .depth_compare = (flags & WOBY_GPU_STATE_DEPTH_TEST_LESS)
                                ? (view.reversedDepth ? gpu::CompareOp::greater : gpu::CompareOp::less)
                                : (view.reversedDepth ? gpu::CompareOp::greater_equal : gpu::CompareOp::less_equal)});
    const gpu::Scissor full{view.x, view.y, view.width, view.height};
    gpu::set_scissor(commands, encoder.hasScissor ? encoder.scissor : full);
    if (encoder.indices.gpu)
        gpu::draw_indexed(commands, root.gpu, {encoder.indices.gpu, encoder.indices.bytes},
                          encoder.indices.index32 ? gpu::IndexType::uint32 : gpu::IndexType::uint16,
                          encoder.indices.count, encoder.instances);
    else
        gpu::draw(commands, root.gpu, encoder.vertexCount ? encoder.vertexCount : encoder.vertices.count,
                  encoder.instances);
    ++state().stats.numDraw;
}
void computeOperation(gpu::CommandBuffer *commands, const Operation &op, const View &view)
{
    auto &c = state();
    auto it = c.computePipelines.find(op.program->id);
    if (it == c.computePipelines.end())
    {
        auto *pso = gpu::create_compute_pso(c.device, shaderStage(op.program->compute));
        require(pso != nullptr, "NoGraphicsAPI compute pipeline creation failed");
        it = c.computePipelines.emplace(op.program->id, pso).first;
    }
    auto root = allocateRoot(rootData(op.encoder, view));
    gpu::bind_pso(commands, it->second);
    gpu::dispatch(commands, root.gpu, op.groups);
    ++c.stats.numCompute;
}
void blitOperation(gpu::CommandBuffer *commands, const Operation &op)
{
    require(op.source->format == op.destination->format, "Blit requires identical texture formats");
    const uint64_t bytes = uint64_t(op.width) * op.height * pixelBytes(op.source->format);
    auto heap = gpu::create_gpu_heap(state().device, (bytes + 15) & ~uint64_t{15}, gpu::MemoryType::gpu_only);
    require(heap.owner != nullptr, "Cannot allocate texture transfer buffer");
    auto retained = std::shared_ptr<gpu::GpuHeap>(new gpu::GpuHeap(heap),
                                                  [](gpu::GpuHeap *value)
                                                  {
                                                      gpu::destroy_gpu_heap(*value);
                                                      delete value;
                                                  });
    current().retained.push_back(retained);
    const gpu::GpuRange range{heap.range.gpu, bytes};
    gpu::copy_texture_to_memory(commands, op.source->image->placed.texture, range,
                                {.offset = {op.sourceX, op.sourceY, 0}, .extent = {op.width, op.height, 1}});
    gpu::barrier(commands, gpu::Stage::transfer, gpu::Access::transfer_write, gpu::Stage::transfer,
                 gpu::Access::transfer_read);
    gpu::copy_memory_to_texture(commands, range, op.destination->image->placed.texture,
                                {.offset = {op.x, op.y, 0}, .extent = {op.width, op.height, 1}});
}
void ensureOutput(Frame &f)
{
    auto &c = state();
    const uint32_t samples = (c.flags & WOBY_GPU_RESET_MSAA_X4) ? 4u : 1u;
    if (f.output && f.width == c.width && f.height == c.height && f.samples == samples)
        return;
    f.output = std::make_shared<Framebuffer>();
    const auto flags = samples == 4 ? WOBY_GPU_TEXTURE_RT_MSAA_X4 : WOBY_GPU_TEXTURE_RT;
    f.output->textures = {
        createTexture(static_cast<uint16_t>(c.width), static_cast<uint16_t>(c.height), TextureFormat::BGRA8, flags),
        createTexture(static_cast<uint16_t>(c.width), static_cast<uint16_t>(c.height), TextureFormat::D24S8, flags)};
    f.width = c.width;
    f.height = c.height;
    f.samples = samples;
}
void resetEncoder()
{
    state().encoder = Encoder{};
}
} // namespace

bool init(const Init &options)
{
    if (context)
    {
        initError = "Renderer already initialized";
        return false;
    }
    initError.clear();
    context = std::make_unique<Context>();
    auto &c = *context;
    c.width = std::max(1u, options.resolution.width);
    c.height = std::max(1u, options.resolution.height);
    c.flags = options.resolution.reset;
    if (options.type == RendererType::Noop)
        return true;
    try
    {
        auto *window = static_cast<SDL_Window *>(options.platformData.window);
        gpu::DeviceDesc deviceOptions{.swapchain_format = gpu::Format::bgra8_unorm};
#if defined(_WIN32)
        deviceOptions.allow_mailbox_presentation = window != nullptr;
#endif
#if defined(__APPLE__)
        if (window)
        {
            c.metalView = SDL_Metal_CreateView(window);
            require(c.metalView != nullptr, SDL_GetError());
            deviceOptions.window = SDL_Metal_GetLayer(c.metalView);
        }
#else
        deviceOptions.window = window;
        if (window)
        {
            deviceOptions.surface_extensions = SDL_Vulkan_GetInstanceExtensions(&deviceOptions.surface_extension_count);
            require(deviceOptions.surface_extensions != nullptr, SDL_GetError());
            deviceOptions.create_surface = [](void *handle, void *instance) -> uint64_t
            {
                VkSurfaceKHR surface = VK_NULL_HANDLE;
                if (!SDL_Vulkan_CreateSurface(static_cast<SDL_Window *>(handle), static_cast<VkInstance>(instance),
                                              nullptr, &surface))
                    return 0;
                return reinterpret_cast<uint64_t>(surface);
            };
            deviceOptions.drawable_extent = [](void *handle) -> gpu::uint32x2
            {
                int width = 0, height = 0;
                SDL_GetWindowSizeInPixels(static_cast<SDL_Window *>(handle), &width, &height);
                return {static_cast<uint32_t>(std::max(0, width)), static_cast<uint32_t>(std::max(0, height))};
            };
        }
#endif
        DeviceCreationDiagnostics diagnostics;
        deviceOptions.diagnostic_context = &diagnostics;
        deviceOptions.diagnostic = collectDeviceDiagnostic;
        auto initialized = gpu::create_device(deviceOptions);
        if (!initialized.device)
            throw std::runtime_error(formatDeviceCreationFailure(diagnostics, initialized.error,
#if defined(__APPLE__)
                true
#else
                false
#endif
                ));
        c.device = initialized.device;
        c.window = window;
        c.windowed = window != nullptr;
#if defined(__APPLE__)
        c.caps.rendererType = RendererType::Metal;
#else
        c.caps.rendererType = RendererType::Vulkan;
#endif
        c.caps.supported = WOBY_GPU_CAPS_COMPUTE | WOBY_GPU_CAPS_VERTEX_ID | WOBY_GPU_CAPS_INSTANCING |
                           WOBY_GPU_CAPS_INDEX32 | WOBY_GPU_CAPS_TEXTURE_READ_BACK | WOBY_GPU_CAPS_TEXTURE_BLIT |
                           WOBY_GPU_CAPS_BLEND_INDEPENDENT | WOBY_GPU_CAPS_PRIMITIVE_ID;
        c.timeline = gpu::create_timeline_semaphore(c.device);
        c.descriptors = gpu::create_texture_descriptor_heap(c.device, descriptorCapacity);
        c.samplers = gpu::create_sampler_descriptor_heap(c.device, 2);
        gpu::write_sampler_descriptor(
            c.samplers, 0,
            {.address_u = gpu::AddressMode::clamp_to_edge, .address_v = gpu::AddressMode::clamp_to_edge});
        gpu::write_sampler_descriptor(c.samplers, 1,
                                      {.min_filter = gpu::Filter::nearest,
                                       .mag_filter = gpu::Filter::nearest,
                                       .mip_filter = gpu::Filter::nearest,
                                       .address_u = gpu::AddressMode::clamp_to_edge,
                                       .address_v = gpu::AddressMode::clamp_to_edge});
        for (uint32_t i = 0; i < descriptorCapacity; ++i)
            c.freeDescriptors.push_back(i);
        c.uploads = std::make_unique<gpu::UploadQueue>(c.device, 16ull * 1024 * 1024);
        c.deletes = std::make_unique<gpu::DeleteQueue>(c.timeline, 64);
        for (auto &f : c.frames)
            f.pool = gpu::create_command_pool(c.device);
        return true;
    }
    catch (const std::exception &error)
    {
        initError = error.what();
        shutdown();
        return false;
    }
}
const char *initializationError()
{
    return initError.c_str();
}
const Caps *getCaps()
{
    return &state().caps;
}
const Stats *getStats()
{
    return &state().stats;
}
RendererType::Enum getRendererType()
{
    return state().caps.rendererType;
}
const char *getRendererName(RendererType::Enum type)
{
    switch (type)
    {
    case RendererType::Vulkan:
        return "NoGraphicsAPI Vulkan";
    case RendererType::Metal:
        return "NoGraphicsAPI Metal";
    case RendererType::Noop:
        return "CPU test renderer";
    case RendererType::Count:
        return "NoGraphicsAPI";
    }
    return "NoGraphicsAPI";
}
void reset(uint32_t width, uint32_t height, uint32_t flags)
{
    auto &c = state();
    c.width = std::max(1u, width);
    c.height = std::max(1u, height);
    c.flags = flags;
}
void setDebug(uint32_t) {}
void dbgTextClear() {}
const Memory *alloc(uint32_t bytes)
{
    return new Memory{new uint8_t[bytes], bytes, true};
}
const Memory *copy(const void *data, uint32_t bytes)
{
    auto *memory = alloc(bytes);
    std::memcpy(memory->data, data, bytes);
    return memory;
}
const Memory *makeRef(const void *data, uint32_t bytes, void (*callback)(void *, void *), void *user)
{
    return new Memory{const_cast<uint8_t *>(static_cast<const uint8_t *>(data)), bytes, false, callback, user};
}
VertexBufferHandle createVertexBuffer(const Memory *memory, const VertexLayout &layout, uint16_t)
{
    return insert<VertexBufferHandle>(state().vertexBuffers, createBuffer(memory, layout.stride, false));
}
IndexBufferHandle createIndexBuffer(const Memory *memory, uint16_t flags)
{
    const bool index32 = (flags & WOBY_GPU_BUFFER_INDEX32) != 0;
    return insert<IndexBufferHandle>(state().indexBuffers, createBuffer(memory, index32 ? 4u : 2u, index32));
}
bool isTextureValid(uint16_t depth, bool cube, uint16_t layers, TextureFormat::Enum format, uint64_t flags)
{
    if (depth || cube || layers != 1)
        return false;
    if (noop())
        return true;
    auto usage =
        format == TextureFormat::D24S8 ? gpu::TextureUsage::depth_stencil_attachment : gpu::TextureUsage::sampled;
    if (format != TextureFormat::D24S8 && (flags & WOBY_GPU_TEXTURE_RT))
        usage = usage | gpu::TextureUsage::color_attachment;
    if (flags & WOBY_GPU_TEXTURE_COMPUTE_WRITE)
        usage = usage | gpu::TextureUsage::storage;
    return gpu::supports_texture_format(state().device, nativeFormat(format), usage);
}
TextureHandle createTexture2D(uint16_t width, uint16_t height, bool mipmaps, uint16_t layers,
                              TextureFormat::Enum format, uint64_t flags, const Memory *data)
{
    MemoryOwner memory(data, freeMemory);
    require(!mipmaps && layers == 1, "Woby uses single-mip 2D textures");
    auto texture = createTexture(width, height, format, flags);
    if (data)
    {
        const auto size = size_t(width) * height * pixelBytes(format);
        require(data->size >= size, "Insufficient texture pixels");
        texture->pixels.assign(data->data, data->data + size);
        uploadTexture(texture);
    }
    return insert<TextureHandle>(state().textures, std::move(texture));
}
void updateTexture2D(TextureHandle handle, uint16_t layer, uint8_t mip, uint16_t x, uint16_t y, uint16_t width,
                     uint16_t height, const Memory *data, uint16_t pitch)
{
    MemoryOwner memory(data, freeMemory);
    auto old = resource(state().textures, handle);
    require(layer == 0 && mip == 0 && uint32_t(x) + width <= old->width && uint32_t(y) + height <= old->height,
            "Texture update is out of bounds");
    auto texture = createTexture(old->width, old->height, old->format, old->flags);
    const auto bpp = pixelBytes(old->format), rowBytes = uint32_t(width) * bpp;
    const uint32_t sourcePitch = pitch == UINT16_MAX ? rowBytes : pitch;
    require(memory && height && sourcePitch >= rowBytes &&
                uint64_t(sourcePitch) * (height - 1) + rowBytes <= memory->size,
            "Invalid texture update pitch or size");
    texture->pixels = old->pixels;
    texture->pixels.resize(size_t(old->width) * old->height * bpp);
    for (uint32_t row = 0; row < height; ++row)
        std::memcpy(texture->pixels.data() + (size_t(y + row) * old->width + x) * bpp,
                    memory->data + size_t(row) * sourcePitch, rowBytes);
    uploadTexture(texture);
    state().textures[handle.idx] = std::move(texture);
    state().trash->push_back(std::move(old));
}
FrameBufferHandle createFrameBuffer(uint8_t count, const TextureHandle *handles, bool destroyTextures)
{
    auto target = std::make_shared<Framebuffer>();
    require(count > 0 && count <= 3, "Invalid framebuffer attachment count");
    uint32_t colors = 0, depths = 0;
    for (uint8_t i = 0; i < count; ++i)
    {
        auto texture = resource(state().textures, handles[i]);
        require((texture->flags & WOBY_GPU_TEXTURE_RT) != 0, "Framebuffer texture is not a render target");
        if (!target->textures.empty())
        {
            const auto &first = target->textures[0];
            require(texture->width == first->width && texture->height == first->height &&
                        attachment(texture)->samples == attachment(first)->samples,
                    "Framebuffer dimensions and sample counts must match");
        }
        if (texture->format == TextureFormat::D24S8)
            ++depths;
        else
            ++colors;
        require(colors <= 2 && depths <= 1, "Unsupported framebuffer attachment combination");
        target->textures.push_back(std::move(texture));
    }
    if (destroyTextures)
        for (uint8_t i = 0; i < count; ++i)
            destroy(handles[i]);
    return insert<FrameBufferHandle>(state().framebuffers, std::move(target));
}
ShaderHandle createShader(const Memory *data)
{
    MemoryOwner memory(data, freeMemory);
    require(memory && memory->size, "Invalid native shader binary");
    auto shader = std::make_shared<Shader>();
    shader->bytes = memory->size;
    shader->code.resize((memory->size + 3) / 4);
    std::memcpy(shader->code.data(), memory->data, memory->size);
    return insert<ShaderHandle>(state().shaders, std::move(shader));
}
void setName(ShaderHandle handle, const char *name)
{
    auto shader = resource(state().shaders, handle);
    shader->entry = name;
    shader->entry = shader->entry.substr(0, shader->entry.find('.'));
}
ProgramHandle createProgram(ShaderHandle vertex, ShaderHandle fragment, bool destroyShaders)
{
    auto program = std::make_shared<Program>();
    program->vertex = resource(state().shaders, vertex);
    program->fragment = resource(state().shaders, fragment);
    program->id = static_cast<uint32_t>(state().programs.size());
    auto handle = insert<ProgramHandle>(state().programs, std::move(program));
    if (destroyShaders)
    {
        destroy(vertex);
        destroy(fragment);
    }
    return handle;
}
ProgramHandle createProgram(ShaderHandle compute, bool destroyShader)
{
    auto program = std::make_shared<Program>();
    program->compute = resource(state().shaders, compute);
    program->id = static_cast<uint32_t>(state().programs.size());
    auto handle = insert<ProgramHandle>(state().programs, std::move(program));
    if (destroyShader)
        destroy(compute);
    return handle;
}
UniformHandle createUniform(const char *name, UniformType::Enum type, uint16_t count)
{
    return insert<UniformHandle>(state().uniforms, std::make_shared<Uniform>(Uniform{name, type, count}));
}
void destroy(VertexBufferHandle h)
{
    release(state().vertexBuffers, h);
}
void destroy(IndexBufferHandle h)
{
    release(state().indexBuffers, h);
}
void destroy(TextureHandle h)
{
    release(state().textures, h);
}
void destroy(ShaderHandle h)
{
    release(state().shaders, h);
}
void destroy(ProgramHandle h)
{
    release(state().programs, h);
}
void destroy(UniformHandle h)
{
    release(state().uniforms, h);
}
void destroy(FrameBufferHandle h)
{
    if (!isValid(h))
        return;
    auto target = resource(state().framebuffers, h);
    for (auto &view : state().views)
        if (view.framebuffer == target)
            view.framebuffer.reset();
    release(state().framebuffers, h);
}
uint32_t getAvailTransientVertexBuffer(uint32_t count, const VertexLayout &layout)
{
    beginFrame();
    return layout.stride
               ? static_cast<uint32_t>(std::min(uint64_t(count), (arenaLimit - current().arenaUsed) / layout.stride))
               : 0;
}
uint32_t getAvailTransientIndexBuffer(uint32_t count, bool index32)
{
    beginFrame();
    return static_cast<uint32_t>(std::min(uint64_t(count), (arenaLimit - current().arenaUsed) / (index32 ? 4 : 2)));
}
void allocTransientVertexBuffer(TransientVertexBuffer *buffer, uint32_t count, const VertexLayout &layout)
{
    auto memory = allocate(uint64_t(count) * layout.stride);
    *buffer = {memory.cpu, count * layout.stride, layout.stride, memory.gpu};
}
void allocTransientIndexBuffer(TransientIndexBuffer *buffer, uint32_t count, bool index32)
{
    auto bytes = count * (index32 ? 4u : 2u);
    auto memory = allocate(bytes);
    *buffer = {memory.cpu, bytes, index32, memory.gpu};
}
Geometry geometry(std::shared_ptr<Buffer> owner, uint32_t start, uint32_t count)
{
    require(owner->stride && start <= owner->bytes / owner->stride, "GPU buffer offset is out of bounds");
    const auto available = owner->bytes / owner->stride - start;
    count = std::min(count, available);
    return {owner,
            owner->heap.range.gpu ? owner->heap.range.gpu + uint64_t(start) * owner->stride : nullptr,
            count * owner->stride,
            count,
            owner->stride,
            owner->index32};
}
void setVertexBuffer(uint8_t, VertexBufferHandle h, uint32_t start, uint32_t count)
{
    state().encoder.vertices = geometry(resource(state().vertexBuffers, h), start, count);
}
void setVertexBuffer(uint8_t, const TransientVertexBuffer *b, uint32_t start, uint32_t count)
{
    require(b->stride && start <= b->size / b->stride, "Invalid transient vertex offset");
    count = std::min(count, b->size / b->stride - start);
    state().encoder.vertices = {
        {}, static_cast<byte *>(b->gpu) + uint64_t(start) * b->stride, count * b->stride, count, b->stride, false};
}
void setIndexBuffer(IndexBufferHandle h, uint32_t start, uint32_t count)
{
    state().encoder.indices = geometry(resource(state().indexBuffers, h), start, count);
}
void setIndexBuffer(const TransientIndexBuffer *b, uint32_t start, uint32_t count)
{
    const uint16_t stride = b->index32 ? 4u : 2u;
    require(start <= b->size / stride, "Invalid transient index offset");
    count = std::min(count, b->size / stride - start);
    state().encoder.indices = {
        {}, static_cast<byte *>(b->gpu) + uint64_t(start) * stride, count * stride, count, stride, b->index32};
}
void setBuffer(uint8_t binding, VertexBufferHandle h, Access::Enum)
{
    require(binding == 0 || binding == 2, "Unexpected vertex storage binding");
    if (binding == 0) { setVertexBuffer(0, h); }
    else { state().encoder.freeform = geometry(resource(state().vertexBuffers, h), 0, UINT32_MAX); }
}
void setBuffer(uint8_t binding, IndexBufferHandle h, Access::Enum)
{
    require(binding == 1, "Unexpected point storage binding");
    state().encoder.points = geometry(resource(state().indexBuffers, h), 0, UINT32_MAX);
}
void setVertexCount(uint32_t count)
{
    state().encoder.vertexCount = count;
}
void setInstanceCount(uint32_t count)
{
    state().encoder.instances = count;
}
void setTransform(const float *value)
{
    std::copy_n(value, 16, state().encoder.model.begin());
}
void setUniform(UniformHandle h, const void *data, uint16_t count)
{
    auto uniform = resource(state().uniforms, h);
    require(uniform->type == UniformType::Vec4 && count <= uniform->count, "Invalid uniform write");
    auto &root = state().encoder.root;
    void *destination = nullptr;
    if (uniform->name == "u_color")
        destination = &root.color;
    else if (uniform->name == "u_pointParams")
        destination = root.pointParams;
    else if (uniform->name == "u_comparison")
        destination = &root.comparison;
    else if (uniform->name == "u_uvGrid")
        destination = &root.uvGrid;
    else if (uniform->name == "u_markerBase")
        destination = &root.markerBase;
    else if (uniform->name == "u_markerQuery")
        destination = &root.markerQuery;
    else if (uniform->name == "u_markerOptions")
        destination = &root.markerOptions;
    require(destination != nullptr, "Unknown Woby shader uniform");
    require(data && count > 0 && count <= (uniform->name == "u_pointParams" ? 2 : 1),
            "Uniform write exceeds root field capacity");
    std::memcpy(destination, data, size_t(count) * 16);
}
void setTexture(uint8_t binding, UniformHandle, TextureHandle h, uint32_t flags)
{
    require(binding < 2, "Unexpected texture binding");
    auto texture = resource(state().textures, h);
    state().encoder.textures[binding] = texture;
    state().encoder.root.sampler = ((flags == UINT32_MAX ? texture->flags : flags) & WOBY_GPU_SAMPLER_POINT) ? 1u : 0u;
}
void setImage(uint8_t binding, TextureHandle h, uint8_t mip, Access::Enum, TextureFormat::Enum)
{
    require(binding == 1 && mip == 0, "Unexpected storage image binding");
    state().encoder.storage = resource(state().textures, h);
}
void setState(uint64_t value, uint32_t)
{
    state().encoder.state = value;
}
void setScissor(uint16_t x, uint16_t y, uint16_t width, uint16_t height)
{
    state().encoder.scissor = {x, y, width, height};
    state().encoder.hasScissor = true;
}
void setViewName(ViewId, const char *) {}
void setViewMode(ViewId, ViewMode::Enum) {}
void setViewFrameBuffer(ViewId id, FrameBufferHandle h)
{
    state().views.at(id).framebuffer = isValid(h) ? resource(state().framebuffers, h) : nullptr;
}
void setViewRect(ViewId id, uint16_t x, uint16_t y, uint16_t width, uint16_t height)
{
    auto &v = state().views.at(id);
    v.x = x;
    v.y = y;
    v.width = width;
    v.height = height;
}
void setViewTransform(ViewId id, const float *view, const float *projection, bool reversedDepth)
{
    auto &v = state().views.at(id);
    v.reversedDepth = reversedDepth;
    if (view)
        std::copy_n(view, 16, v.view.begin());
    else
        bx::mtxIdentity(v.view.data());
    if (projection)
        std::copy_n(projection, 16, v.projection.begin());
    else
        bx::mtxIdentity(v.projection.data());
}
void setViewClear(ViewId id, uint16_t flags, uint32_t color, float depth, uint8_t)
{
    auto &v = state().views.at(id);
    v.clearFlags = flags;
    v.colors = {color, 0};
    v.depth = depth;
}
void setViewClear(ViewId id, uint16_t flags, float depth, uint8_t, uint8_t color0, uint8_t color1)
{
    auto &v = state().views.at(id);
    v.clearFlags = flags;
    v.colors = {state().palette[color0], state().palette[color1]};
    v.depth = depth;
}
void setPaletteColor(uint8_t index, uint32_t rgba)
{
    state().palette[index] = rgba;
}
void touch(ViewId id)
{
    beginFrame();
    current().operations.push_back({.view = id, .type = OperationType::touch});
}
void submit(ViewId id, ProgramHandle h)
{
    beginFrame();
    current().operations.push_back({.view = id,
                                    .type = OperationType::draw,
                                    .encoder = state().encoder,
                                    .program = resource(state().programs, h)});
    resetEncoder();
}
void dispatch(ViewId id, ProgramHandle h, uint32_t x, uint32_t y, uint32_t z)
{
    beginFrame();
    current().operations.push_back({.view = id,
                                    .type = OperationType::compute,
                                    .encoder = state().encoder,
                                    .program = resource(state().programs, h),
                                    .groups = {x, y, z}});
    resetEncoder();
}
void blit(ViewId id, TextureHandle destination, uint16_t x, uint16_t y, TextureHandle source, uint16_t sourceX,
          uint16_t sourceY, uint16_t width, uint16_t height)
{
    beginFrame();
    auto from = resource(state().textures, source), to = resource(state().textures, destination);
    require(sourceX < from->width && sourceY < from->height && x < to->width && y < to->height,
            "Invalid texture blit offset");
    width = std::min({width, static_cast<uint16_t>(from->width - sourceX), static_cast<uint16_t>(to->width - x)});
    height = std::min({height, static_cast<uint16_t>(from->height - sourceY), static_cast<uint16_t>(to->height - y)});
    current().operations.push_back({.view = id,
                                    .type = OperationType::blit,
                                    .source = from,
                                    .destination = to,
                                    .x = x,
                                    .y = y,
                                    .sourceX = sourceX,
                                    .sourceY = sourceY,
                                    .width = width,
                                    .height = height});
}
uint32_t readTexture(TextureHandle handle, void *destination)
{
    beginFrame();
    const uint32_t ready = state().frameNumber + 3;
    state().reads.push_back({resource(state().textures, handle), destination, ready});
    return ready;
}

uint32_t readBuffer(VertexBufferHandle handle, void *destination)
{
    beginFrame();
    const uint32_t ready = state().frameNumber + 3;
    state().reads.push_back({{}, destination, ready, resource(state().vertexBuffers, handle)});
    return ready;
}
uint32_t readBuffer(IndexBufferHandle handle, void *destination)
{
    beginFrame();
    const uint32_t ready = state().frameNumber + 3;
    state().reads.push_back({{}, destination, ready, resource(state().indexBuffers, handle)});
    return ready;
}

uint32_t frame()
{
    beginFrame();
    auto &c = state();
    auto &f = current();
    const auto begin = ticks();
    c.stats.numDraw = c.stats.numCompute = 0;
    if (noop())
    {
        c.trash->clear();
        c.frameOpen = false;
        return ++c.frameNumber;
    }
    // Acquire before finalizing the upload command buffer: resize may create targets.
    auto *commands = gpu::begin_commands(f.pool);
    auto swap = c.windowed ? gpu::acquire(commands) : gpu::SwapchainFrame{};
#if defined(_WIN32)
    // FIFO retirement on mixed-refresh Windows desktops can follow a different
    // display's cadence. Mailbox keeps the latest completed image available to
    // the compositor. Rendering at most twice the target refresh rate avoids
    // the observed starvation between those clocks while keeping work bounded.
    const bool inactive = c.window &&
        (SDL_GetWindowFlags(c.window) & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN)) != 0;
    const bool mailbox = swap.render_view && !inactive && gpu::supports_mailbox_presentation(c.device);
    if (mailbox || inactive)
    {
        const auto *mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(c.window));
        // Occluded FIFO presentation may return immediately; keep minimized
        // windows responsive without spinning through thousands of frames.
        const auto delay = advanceFramePacing(c.pacing, SDL_GetTicksNS(), mode ? mode->refresh_rate : 0.0, !inactive);
        if (delay)
            SDL_DelayPrecise(delay);
    }
    else
        resetFramePacing(c.pacing);
    gpu::set_mailbox_presentation(c.device, mailbox);
#endif
    if (swap.render_view)
    {
        c.width = swap.extent.x;
        c.height = swap.extent.y;
    }
    const bool needsOutput = std::any_of(f.operations.begin(), f.operations.end(),
                                         [&](const Operation &op)
                                         {
                                             return !c.views[op.view].framebuffer &&
                                                    (op.type == OperationType::draw || op.type == OperationType::touch);
                                         });
    if (needsOutput || swap.render_view)
        ensureOutput(f);
    gpu::end_commands(f.preparations);
    gpu::set_texture_descriptor_heap(commands, c.descriptors);
    gpu::set_sampler_descriptor_heap(commands, c.samplers);
    gpu::write_timestamp(commands, &f.timestamps[0]);
    std::stable_sort(f.operations.begin(), f.operations.end(),
                     [](const Operation &a, const Operation &b) { return a.view < b.view; });
    size_t index = 0;
    while (index < f.operations.size())
    {
        const auto id = f.operations[index].view;
        const auto &view = c.views[id];
        auto target = view.framebuffer ? view.framebuffer : f.output;
        bool active = false, first = true;
        Attachments renderTargets;
        allBarrier(commands);
        while (index < f.operations.size() && f.operations[index].view == id)
        {
            const auto &op = f.operations[index++];
            if (op.type == OperationType::draw || op.type == OperationType::touch)
            {
                if (!active)
                {
                    require(bool(target), "Render view has no target");
                    renderTargets = attachments(*target, view, first);
                    gpu::begin_render_pass(commands, {.colors = {renderTargets.colors.data(), renderTargets.count},
                                                      .depth = renderTargets.depth});
                    gpu::set_viewport(commands, {float(view.x), float(view.y), float(view.width), float(view.height)});
                    active = true;
                    first = false;
                }
                if (op.type == OperationType::draw)
                    drawOperation(commands, op, view, renderTargets);
            }
            else
            {
                if (active)
                {
                    gpu::end_render_pass(commands);
                    active = false;
                    allBarrier(commands);
                }
                if (op.type == OperationType::compute)
                    computeOperation(commands, op, view);
                else
                    blitOperation(commands, op);
                allBarrier(commands);
            }
        }
        if (active)
            gpu::end_render_pass(commands);
        if (target)
            f.retained.push_back(target);
    }
    allBarrier(commands);
    const auto signal = ++c.submitted;
    for (const auto &read : c.reads)
    {
        const uint32_t bytes = read.buffer ? read.buffer->bytes : uint32_t(read.texture->width) * read.texture->height * pixelBytes(read.texture->format);
        auto heap = gpu::create_gpu_heap(c.device, (uint64_t(bytes) + 15) & ~uint64_t{15}, gpu::MemoryType::readback);
        require(heap.range.cpu != nullptr, "GPU readback allocation failed");
        if (read.buffer) {
            gpu::copy_memory(commands, {read.buffer->heap.range.gpu, bytes}, {heap.range.gpu, bytes});
            f.retained.push_back(read.buffer);
        } else {
            gpu::copy_texture_to_memory(commands, read.texture->image->placed.texture, {heap.range.gpu, bytes});
            f.retained.push_back(read.texture);
        }
        c.readbacks.push_back({heap, read.destination, signal, read.ready, bytes});
    }
    c.reads.clear();
    if (swap.render_view)
    {
        // Use the regular Slang composite shader to copy the resolved image to WSI.
        std::shared_ptr<Program> composite;
        for (const auto &p : c.programs)
            if (p && p->fragment && p->fragment->entry == "fs_marker_composite")
            {
                composite = p;
                break;
            }
        require(bool(composite), "Presentation composite shader is not loaded");
        Operation op;
        op.program = composite;
        op.type = OperationType::draw;
        op.encoder.vertexCount = 4;
        op.encoder.state = WOBY_GPU_STATE_WRITE_RGB | WOBY_GPU_STATE_WRITE_A | WOBY_GPU_STATE_PT_TRISTRIP;
        op.encoder.textures[0] = f.output->textures[0];
        View view;
        view.width = static_cast<uint16_t>(c.width);
        view.height = static_cast<uint16_t>(c.height);
        Attachments target;
        target.count = 1;
        target.colors[0] = {.render_view = swap.render_view, .load = gpu::LoadOp::discard};
        target.formats[0].format = gpu::Format::bgra8_unorm;
        gpu::begin_render_pass(commands, {.colors = {target.colors.data(), 1}});
        drawOperation(commands, op, view, target);
        gpu::end_render_pass(commands);
    }
    gpu::barrier(commands, gpu::Stage::transfer, gpu::Access::transfer_write, gpu::Stage::host, gpu::Access::host_read);
    gpu::write_timestamp(commands, &f.timestamps[1]);
    gpu::end_commands(commands);
    const auto uploaded = c.uploads->flush();
    // SubmitDesc borrows its spans. Initializer-list storage would expire at
    // the declaration below, before submit() reads it in an optimized build.
    const std::array submittedCommands{f.preparations, commands};
    const std::array uploadWaits{uploaded};
    const gpu::SubmitDesc submission{
        .commands = submittedCommands, .waits = uploadWaits, .completion = {c.timeline, signal}};
    if (swap.render_view)
        gpu::submit_and_present(c.device, submission);
    else
        gpu::submit(c.device, submission);
    f.completion = signal;
    if (!c.trash->empty())
    {
        auto *trash = c.trash.release();
        c.deletes->defer(signal, [trash]() noexcept { delete trash; });
        c.trash = std::make_unique<std::vector<std::shared_ptr<void>>>();
    }
    c.frameOpen = false;
    ++c.frameNumber;
    collectReadbacks(false);
    c.stats.cpuTimeBegin = begin;
    c.stats.cpuTimeEnd = ticks();
    c.stats.cpuTimeFrame = c.stats.cpuTimeEnd - begin;
    return c.frameNumber;
}
void shutdown()
{
    if (!context)
        return;
    auto &c = *context;
    if (c.uploads)
        c.uploads->wait();
    if (c.device)
    {
        gpu::wait_idle(c.device);
        collectReadbacks(true);
        if (c.deletes)
            c.deletes->drain();
    }
    c.encoder = Encoder{};
    c.views = {};
    c.reads.clear();
    c.trash->clear();
    c.vertexBuffers.clear();
    c.indexBuffers.clear();
    c.textures.clear();
    c.framebuffers.clear();
    c.shaders.clear();
    c.programs.clear();
    c.uniforms.clear();
    for (auto &f : c.frames)
    {
        f.operations.clear();
        f.retained.clear();
        f.output.reset();
        for (auto &arena : f.arenas)
        {
            arena.allocator.reset();
            gpu::destroy_gpu_heap(arena.heap);
        }
        f.arenas.clear();
        gpu::destroy_command_pool(f.pool);
    }
    for (auto &[key, pso] : c.pipelines)
    {
        (void)key;
        gpu::destroy_pso(pso);
    }
    for (auto &[key, pso] : c.computePipelines)
    {
        (void)key;
        gpu::destroy_pso(pso);
    }
    c.pages.clear();
    c.uploads.reset();
    c.deletes.reset();
    if (c.descriptors)
        gpu::destroy_texture_descriptor_heap(c.descriptors);
    if (c.samplers)
        gpu::destroy_sampler_descriptor_heap(c.samplers);
    if (c.timeline)
        gpu::destroy_timeline_semaphore(c.timeline);
    if (c.device)
        gpu::destroy_device(c.device);
#if defined(__APPLE__)
    if (c.metalView)
        SDL_Metal_DestroyView(c.metalView);
#endif
    context.reset();
}
} // namespace woby::graphics

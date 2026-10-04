#pragma once

#include "scene_buffer_size.h"

// Woby's render submission interface. The implementation owns NoGraphicsAPI
// resources, explicit frame ordering, descriptor lifetimes and timeline fences.
#include <array>
#include <cstddef>
#include <cstdint>

#define WOBY_GPU_INVALID_HANDLE {woby::graphics::kInvalidHandle}
inline constexpr uint64_t WOBY_GPU_STATE_WRITE_RGB = 1ull << 0, WOBY_GPU_STATE_WRITE_A = 1ull << 1,
                          WOBY_GPU_STATE_WRITE_Z = 1ull << 2, WOBY_GPU_STATE_DEPTH_TEST_LESS = 1ull << 3,
                          WOBY_GPU_STATE_DEPTH_TEST_LEQUAL = 1ull << 4, WOBY_GPU_STATE_DEPTH_TEST_ALWAYS = 1ull << 5,
                          WOBY_GPU_STATE_MSAA = 1ull << 6, WOBY_GPU_STATE_PT_LINES = 1ull << 7,
                          WOBY_GPU_STATE_PT_TRISTRIP = 1ull << 8, WOBY_GPU_STATE_BLEND_ALPHA = 1ull << 9,
                          WOBY_GPU_STATE_BLEND_INDEPENDENT = 1ull << 10;
inline constexpr uint64_t WOBY_GPU_STATE_BLEND_SRC_ALPHA = 1, WOBY_GPU_STATE_BLEND_INV_SRC_ALPHA = 2,
                          WOBY_GPU_STATE_BLEND_ONE = 3, WOBY_GPU_STATE_BLEND_ZERO = 0;
constexpr uint64_t WOBY_GPU_STATE_BLEND_FUNC(uint64_t, uint64_t)
{
    return WOBY_GPU_STATE_BLEND_ALPHA;
}
constexpr uint32_t WOBY_GPU_STATE_BLEND_FUNC_RT_1(uint64_t, uint64_t)
{
    return 0;
}
inline constexpr uint64_t WOBY_GPU_TEXTURE_RT = 1ull << 0,
                          WOBY_GPU_TEXTURE_RT_MSAA_X4 = (1ull << 1) | WOBY_GPU_TEXTURE_RT,
                          WOBY_GPU_TEXTURE_MSAA_SAMPLE = 1ull << 2, WOBY_GPU_TEXTURE_COMPUTE_WRITE = 1ull << 3,
                          WOBY_GPU_TEXTURE_READ_BACK = 1ull << 4, WOBY_GPU_TEXTURE_BLIT_DST = 1ull << 5,
                          WOBY_GPU_TEXTURE_RT_WRITE_ONLY = 1ull << 6, WOBY_GPU_SAMPLER_POINT = 1ull << 7,
                          WOBY_GPU_SAMPLER_U_CLAMP = 1ull << 8, WOBY_GPU_SAMPLER_V_CLAMP = 1ull << 9,
                          WOBY_GPU_SAMPLER_UVW_CLAMP = WOBY_GPU_SAMPLER_U_CLAMP | WOBY_GPU_SAMPLER_V_CLAMP;
inline constexpr uint16_t WOBY_GPU_BUFFER_INDEX32 = 1, WOBY_GPU_BUFFER_COMPUTE_READ = 2,
                          WOBY_GPU_BUFFER_COMPUTE_READ_WRITE = 4, WOBY_GPU_BUFFER_COMPUTE_FORMAT_32X4 = 8,
                          WOBY_GPU_BUFFER_COMPUTE_TYPE_UINT = 16;
inline constexpr uint16_t WOBY_GPU_CLEAR_NONE = 0, WOBY_GPU_CLEAR_COLOR = 1, WOBY_GPU_CLEAR_DEPTH = 2;
inline constexpr uint32_t WOBY_GPU_RESET_NONE = 0, WOBY_GPU_RESET_VSYNC = 1, WOBY_GPU_RESET_MSAA_X4 = 2,
                          WOBY_GPU_DEBUG_NONE = 0, WOBY_GPU_DEBUG_TEXT = 1, WOBY_GPU_DEBUG_PROFILER = 2;
inline constexpr uint64_t WOBY_GPU_CAPS_COMPUTE = 1ull << 0, WOBY_GPU_CAPS_VERTEX_ID = 1ull << 1,
                          WOBY_GPU_CAPS_INSTANCING = 1ull << 2, WOBY_GPU_CAPS_INDEX32 = 1ull << 3,
                          WOBY_GPU_CAPS_TEXTURE_READ_BACK = 1ull << 4, WOBY_GPU_CAPS_TEXTURE_BLIT = 1ull << 5,
                          WOBY_GPU_CAPS_BLEND_INDEPENDENT = 1ull << 6, WOBY_GPU_CAPS_PRIMITIVE_ID = 1ull << 7;

namespace woby::graphics
{
constexpr uint32_t kInvalidHandle = UINT32_MAX;
using ViewId = uint16_t;
template <typename Tag> struct Handle
{
    uint32_t idx = kInvalidHandle;
};
using VertexBufferHandle = Handle<struct VertexBufferTag>;
using IndexBufferHandle = Handle<struct IndexBufferTag>;
using DynamicVertexBufferHandle = Handle<struct DynamicBufferTag>;
using TextureHandle = Handle<struct TextureTag>;
using FrameBufferHandle = Handle<struct FramebufferTag>;
using ShaderHandle = Handle<struct ShaderTag>;
using ProgramHandle = Handle<struct ProgramTag>;
using UniformHandle = Handle<struct UniformTag>;
template <typename T> constexpr bool isValid(Handle<T> h)
{
    return h.idx != kInvalidHandle;
}
namespace RendererType
{
enum Enum
{
    Noop,
    Vulkan,
    Metal,
    Count
};
}
namespace TextureFormat
{
enum Enum
{
    BGRA8,
    RGBA8,
    RGBA32F,
    D24S8
};
}
namespace UniformType
{
enum Enum
{
    Vec4,
    Sampler
};
}
namespace Access
{
enum Enum
{
    Read,
    Write,
    ReadWrite
};
}
namespace ViewMode
{
enum Enum
{
    Sequential
};
}
struct VertexLayout
{
    uint16_t stride = 0;
};
struct TransientVertexBuffer
{
    uint8_t *data = nullptr;
    uint32_t size = 0;
    uint16_t stride = 0;
    void *gpu = nullptr;
};
struct TransientIndexBuffer
{
    uint8_t *data = nullptr;
    uint32_t size = 0;
    bool index32 = false;
    void *gpu = nullptr;
};
struct Memory
{
    uint8_t *data = nullptr;
    SceneBufferSize size = 0;
    bool owned = false;
    void (*release)(void *, void *) = nullptr;
    void *user = nullptr;
};
struct PlatformData
{
    void *window = nullptr;
};
struct Init
{
    RendererType::Enum type = RendererType::Count;
    PlatformData platformData;
    struct
    {
        uint32_t width = 1, height = 1, reset = WOBY_GPU_RESET_NONE;
    } resolution;
};
struct Caps
{
    RendererType::Enum rendererType = RendererType::Noop;
    uint64_t supported = 0;
    bool homogeneousDepth = false, originBottomLeft = false;
    struct
    {
        uint32_t maxTextureSize = 16384, maxComputeBindings = 8, maxFBAttachments = 8;
    } limits;
};
struct Stats
{
    int64_t cpuTimeFrame = 0, cpuTimeBegin = 0, cpuTimeEnd = 0, cpuTimerFreq = 1000000000, gpuTimeBegin = 0,
            gpuTimeEnd = 0, gpuTimerFreq = 1000000000;
    uint32_t numDraw = 0, numCompute = 0;
};
bool init(const Init &options);
void shutdown();
const char *initializationError();
const Caps *getCaps();
const Stats *getStats();
RendererType::Enum getRendererType();
const char *getRendererName(RendererType::Enum type);
void reset(uint32_t width, uint32_t height, uint32_t flags);
uint32_t frame();
void setDebug(uint32_t flags);
void dbgTextClear();

const Memory *alloc(SceneBufferSize bytes);
const Memory *copy(const void *data, SceneBufferSize bytes);
const Memory *makeRef(const void *data, SceneBufferSize bytes, void (*release)(void *, void *) = nullptr,
                      void *user = nullptr);
VertexBufferHandle createVertexBuffer(const Memory *, const VertexLayout &, uint16_t flags = 0);
IndexBufferHandle createIndexBuffer(const Memory *, uint16_t flags = 0);
// Allocate without copying. Do not draw until all ranges have been filled.
VertexBufferHandle createVertexBufferStorage(SceneBufferSize bytes, const VertexLayout&);
IndexBufferHandle createIndexBufferStorage(SceneBufferSize bytes, uint16_t flags = 0);
// Copies source into owned staging before returning; offset/bytes must be multiples of four.
void uploadBufferRange(VertexBufferHandle, uint32_t offset, const void* data, SceneBufferSize bytes);
void uploadBufferRange(IndexBufferHandle, uint32_t offset, const void* data, SceneBufferSize bytes);

TextureHandle createTexture2D(uint16_t width, uint16_t height, bool mipmaps, uint16_t layers,
                              TextureFormat::Enum format, uint64_t flags = 0, const Memory *data = nullptr);
bool isTextureValid(uint16_t depth, bool cube, uint16_t layers, TextureFormat::Enum format, uint64_t flags);
void updateTexture2D(TextureHandle, uint16_t layer, uint8_t mip, uint16_t x, uint16_t y, uint16_t width,
                     uint16_t height, const Memory *, uint16_t pitch = UINT16_MAX);
FrameBufferHandle createFrameBuffer(uint8_t count, const TextureHandle *attachments, bool destroyTextures = false);
ShaderHandle createShader(const Memory *);
ProgramHandle createProgram(ShaderHandle vertex, ShaderHandle fragment, bool destroyShaders = false);
ProgramHandle createProgram(ShaderHandle compute, bool destroyShader = false);
UniformHandle createUniform(const char *name, UniformType::Enum type, uint16_t count = 1);
void setName(ShaderHandle, const char *name);
template <typename T> void setName(Handle<T>, const char *) {}
void destroy(VertexBufferHandle);
void destroy(IndexBufferHandle);
void destroy(TextureHandle);
void destroy(FrameBufferHandle);
void destroy(ShaderHandle);
void destroy(ProgramHandle);
void destroy(UniformHandle);

uint32_t getAvailTransientVertexBuffer(uint32_t count, const VertexLayout &);
uint32_t getAvailTransientIndexBuffer(uint32_t count, bool index32 = false);
void allocTransientVertexBuffer(TransientVertexBuffer *, uint32_t count, const VertexLayout &);
void allocTransientIndexBuffer(TransientIndexBuffer *, uint32_t count, bool index32 = false);
void setVertexBuffer(uint8_t stream, VertexBufferHandle, uint32_t start = 0, uint32_t count = UINT32_MAX);
void setVertexBuffer(uint8_t stream, const TransientVertexBuffer *, uint32_t start = 0, uint32_t count = UINT32_MAX);
void setIndexBuffer(IndexBufferHandle, uint32_t start = 0, uint32_t count = UINT32_MAX);
void setIndexBuffer(const TransientIndexBuffer *, uint32_t start = 0, uint32_t count = UINT32_MAX);
void setBuffer(uint8_t binding, VertexBufferHandle, Access::Enum);
void setBuffer(uint8_t binding, IndexBufferHandle, Access::Enum);
void setVertexCount(uint32_t count);
void setInstanceCount(uint32_t count);
void setTransform(const float *matrix);
void setUniform(UniformHandle, const void *data, uint16_t count = 1);
void setTexture(uint8_t binding, UniformHandle sampler, TextureHandle, uint32_t flags = UINT32_MAX);
void setImage(uint8_t binding, TextureHandle, uint8_t mip, Access::Enum, TextureFormat::Enum);
void setState(uint64_t state, uint32_t independentBlend = 0);
void setScissor(uint16_t x, uint16_t y, uint16_t width, uint16_t height);
void setViewName(ViewId, const char *);
void setViewMode(ViewId, ViewMode::Enum);
void setViewFrameBuffer(ViewId, FrameBufferHandle);
void setViewRect(ViewId, uint16_t x, uint16_t y, uint16_t width, uint16_t height);
// Reversed views receive an already reversed [1, 0] projection. Depth-test
// flags and clear values retain their forward-depth meaning at this interface.
void setViewTransform(ViewId, const float *view, const float *projection, bool reversedDepth = false);
void setViewClear(ViewId, uint16_t flags, uint32_t color = 0, float depth = 1, uint8_t stencil = 0);
void setViewClear(ViewId, uint16_t flags, float depth, uint8_t stencil, uint8_t color0, uint8_t color1);
void setPaletteColor(uint8_t index, uint32_t rgba);
void touch(ViewId);
void submit(ViewId, ProgramHandle);
void dispatch(ViewId, ProgramHandle, uint32_t x, uint32_t y = 1, uint32_t z = 1);
void blit(ViewId, TextureHandle destination, uint16_t x, uint16_t y, TextureHandle source, uint16_t sourceX = 0,
          uint16_t sourceY = 0, uint16_t width = UINT16_MAX, uint16_t height = UINT16_MAX);
uint32_t readTexture(TextureHandle, void *destination);
// Copies the complete buffer after this frame's recorded work; destination must
// remain alive until the returned frame number, matching readTexture.
uint32_t readBuffer(VertexBufferHandle, void *destination);
uint32_t readBuffer(IndexBufferHandle, void *destination);
} // namespace woby::graphics

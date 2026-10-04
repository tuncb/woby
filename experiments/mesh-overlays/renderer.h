#pragma once
#include "camera.h"
#include "scene_mesh_preparation.h"
#include "root.h"
#include <NoGraphicsAPI/NoGraphicsAPI.hpp>
#include <NoGraphicsAPIUtility/bump_allocator.hpp>
#include <NoGraphicsAPIUtility/texture_allocator.hpp>
#include <NoGraphicsAPIUtility/upload_queue.hpp>
#include <array>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace woby::overlay {
enum class Method { legacy, ordered, barycentric, pulled };
const char* methodName(Method method);
struct Options {
    uint32_t width = 1280, height = 720, samples = 4;
    bool ids = false;
};
struct Display {
    Method method = Method::ordered;
    bool solid = true, edges = true, points = true, xray = false;
    float pointSize = 4, opacity = 1, edgeHalfWidth = .5f;
};
void validate(const Options& options, const Display& display);
struct Group {
    GpuNodeRange range;
    std::array<float,16> model{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    std::array<float,4> color{.35f,.45f,.6f,1};
    bool solid = true, edges = true, points = true;
};
struct Scene {
    gpu::GpuHeap vertices{}, triangles{}, edges{}, points{};
    std::vector<Group> groups;
    size_t vertexCount = 0, triangleCount = 0, markerCount = 0;
    uint64_t bytes = 0;
};
struct Image {
    gpu::PlacedTexture allocation{};
    gpu::RenderView* view = nullptr;
};
struct Renderer {
    gpu::Device* device = nullptr;
    gpu::TimelineSemaphore* timeline = nullptr;
    gpu::CommandPool* pool = nullptr;
    gpu::TextureHeap textureHeap{};
    std::unique_ptr<gpu::TextureAllocator> textures;
    std::unique_ptr<gpu::UploadQueue> uploads;
    gpu::GpuHeap roots{}, readback{}, captureIds{};
    std::unique_ptr<gpu::BumpAllocator> arena;
    gpu::TextureDescriptorHeap* descriptors = nullptr;
    Image color, ids, depth, resolved;
    std::map<std::pair<std::string,bool>,gpu::PSO*> pipelines;
    gpu::PSO* capture = nullptr;
    Options options;
    uint64_t sequence = 0;
    std::array<uint64_t,4> timestamps{};
    Scene scene;
    Renderer() = default;
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    ~Renderer();
};
struct Measurement {
    double totalMs = 0, surfaceMs = 0, edgeMs = 0, pointMs = 0, cpuSubmitMs = 0;
    uint32_t draws = 0;
    // The current group-interleaved control has no meaningful pass separation.
    bool separated = true;
};
struct Capture {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> rgba;
    std::vector<uint32_t> ids;
};
void initialize(Renderer& renderer, Options options);
void upload(Renderer& renderer, const Mesh& mesh);
Measurement render(Renderer& renderer, const Display& display,
    const std::array<float,16>& viewProjection, Capture* capture = nullptr);
std::array<float,16> fittedProjection(const Bounds& bounds, uint32_t width, uint32_t height,
    float distanceScale = 1, bool orthographic = false);
void savePng(const Capture& capture, const std::filesystem::path& path);
Mesh fixtureMesh();
} // namespace woby::overlay

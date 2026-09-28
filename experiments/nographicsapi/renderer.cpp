#include "renderer.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace woby::ng {
using namespace gpu;
static_assert(sizeof(MeshVertex) == sizeof(woby::Vertex));
static_assert(offsetof(MeshVertex, normal) == offsetof(woby::Vertex, normal));
static_assert(sizeof(UiVertex) == sizeof(ImDrawVert));
static_assert(offsetof(UiVertex, color) == offsetof(ImDrawVert, col));
static_assert(sizeof(PickResult) == 16);

void require(bool success, const char* message) { if (!success) throw std::runtime_error(message); }
namespace {
std::vector<uint32_t> shader(const char* name)
{
    const auto path = std::filesystem::path(WOBY_NGAPI_SHADER_DIR) / (std::string(name) + ".spv");
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    require(bool(input), "Cannot open compiled prototype shader");
    const auto length = static_cast<size_t>(input.tellg());
    require(length > 0 && length % 4 == 0, "Invalid SPIR-V length");
    std::vector<uint32_t> code(length / 4);
    input.seekg(0);
    input.read(reinterpret_cast<char*>(code.data()), static_cast<std::streamsize>(length));
    require(bool(input), "Cannot read prototype shader");
    return code;
}
ShaderStage stage(const std::vector<uint32_t>& code, const char* entry)
{
    return {.code = {reinterpret_cast<const byte*>(code.data()), code.size()*4}, .entry_point = entry};
}
BlendState alphaBlend()
{
    return {.enabled = true,
        .color = {.source = BlendFactor::source_alpha, .destination = BlendFactor::one_minus_source_alpha},
        .alpha = {.source = BlendFactor::one, .destination = BlendFactor::one_minus_source_alpha}};
}
PSO* graphics(Renderer& r, const char* vs, const char* fs, Span<const ColorTargetDesc> colors,
    Format depth = Format::undefined, uint32_t samples = 1, PrimitiveTopology topology = PrimitiveTopology::triangles)
{
    const auto vertex = shader(vs), fragment = shader(fs);
    auto* result = create_graphics_pso(r.device, {.vertex = stage(vertex,vs), .fragment = stage(fragment,fs),
        .color_targets = colors, .depth_format = depth, .sample_count = samples, .topology = topology});
    require(result != nullptr, "NoGraphicsAPI graphics PSO creation failed");
    return result;
}
void destroyTargets(Renderer& r, Targets& targets)
{
    destroyImage(r,targets.color); destroyImage(r,targets.ids); destroyImage(r,targets.depth);
    destroyImage(r,targets.resolved); destroyImage(r,targets.output);
    targets = {};
}
void ensureTargets(Renderer& r, Frame& f, CommandBuffer* cmd, const RenderOptions& o)
{
    auto& t = f.targets;
    if (t.width == o.width && t.height == o.height && t.samples == o.samples) return;
    // This frame's fence has completed. Other frames own disjoint targets and slots.
    destroyTargets(r,t);
    t.width = o.width; t.height = o.height; t.samples = o.samples;
    const auto colorUsage = TextureUsage::color_attachment | TextureUsage::sampled;
    t.color = createImage(r,cmd,o.width,o.height,Format::rgba8_unorm,colorUsage,o.samples);
    t.ids = createImage(r,cmd,o.width,o.height,Format::r32_uint,colorUsage,o.samples);
    t.depth = createImage(r,cmd,o.width,o.height,Format::d32_float,TextureUsage::depth_stencil_attachment,o.samples);
    if (o.samples > 1) t.resolved = createImage(r,cmd,o.width,o.height,Format::rgba8_unorm,colorUsage);
    t.output = createImage(r,cmd,o.width,o.height,Format::rgba8_unorm,colorUsage | TextureUsage::transfer_source);
    write_texture_descriptor(r.descriptors,f.descriptorBase,t.ids.allocation.texture,TextureDescriptorType::sampled);
    write_texture_descriptor(r.descriptors,f.descriptorBase+1,
        o.samples > 1 ? t.resolved.allocation.texture : t.color.allocation.texture,TextureDescriptorType::sampled);
    write_texture_descriptor(r.descriptors,f.descriptorBase+2,t.output.allocation.texture,TextureDescriptorType::sampled);
}
void completeFrame(Renderer& r, Frame& f)
{
    if (!f.pending) return;
    std::memcpy(&f.result,f.readback.range.cpu,sizeof(PickResult));
    completePick(r.selection,f.epoch,f.sequence,f.result.id);
    f.pending = false;
}
GpuRange imageReadback(const Frame& frame)
{
    return {.gpu = frame.readback.range.gpu + imageOffset,
        .size = uint64_t(frame.targets.width)*frame.targets.height*4};
}
}
Image createImage(Renderer& r, CommandBuffer* commands, uint32_t width, uint32_t height,
    Format format, TextureUsage usage, uint32_t samples)
{
    Image image;
    image.allocation = r.textures->allocate(commands, {.extent = {width,height,1}, .format = format, .usage = usage, .sample_count = samples});
    require(image.allocation.texture != nullptr, "Prototype texture heap exhausted (768 MiB)");
    if ((static_cast<uint32_t>(usage) & (static_cast<uint32_t>(TextureUsage::color_attachment)
        | static_cast<uint32_t>(TextureUsage::depth_stencil_attachment))) != 0) {
        image.view = create_render_view(image.allocation.texture);
        require(image.view != nullptr, "Render view creation failed");
    }
    return image;
}
void destroyImage(Renderer& r, Image& image)
{
    if (!image.allocation.texture) return;
    destroy_render_view(image.view);
    r.textures->free(image.allocation);
    image = {};
}
void initialize(Renderer& r, void* window)
{
    const auto init = create_device({.window = window, .swapchain_format = Format::bgra8_unorm});
    require(init.device != nullptr, "NoGraphicsAPI requires Vulkan 1.4, descriptor heaps and device-address commands; see README");
    r.device = init.device;
    r.windowed = window != nullptr;
    std::cout << "GPU: " << get_device_caps(r.device).device_name << '\n';
    r.timeline = create_timeline_semaphore(r.device);
    r.textureHeap = create_texture_heap(r.device,768ull*1024*1024);
    r.textures = std::make_unique<TextureAllocator>(r.device,r.textureHeap,256);
    r.uploads = std::make_unique<UploadQueue>(r.device,64*1024);
    r.deletes = std::make_unique<DeleteQueue>(r.timeline,256);
    r.descriptors = create_texture_descriptor_heap(r.device,256);
    r.samplers = create_sampler_descriptor_heap(r.device,1);
    write_sampler_descriptor(r.samplers,0,{.address_u = AddressMode::clamp_to_edge, .address_v = AddressMode::clamp_to_edge});
    for (uint32_t i = 24; i < 256; ++i) r.freeDescriptors.push_back(i);
    for (size_t i = 0; i < r.frames.size(); ++i) {
        auto& frame = r.frames[i];
        frame.pool = create_command_pool(r.device);
        frame.memory = create_gpu_heap(r.device,16*1024*1024);
        frame.readback = create_gpu_heap(r.device,uint64_t(maxExtent)*maxExtent*4+imageOffset,MemoryType::readback);
        frame.allocator = std::make_unique<BumpAllocator>(frame.memory.range);
        frame.descriptorBase = static_cast<uint32_t>(i)*8;
    }
    const ColorTargetDesc sceneColors[] = {{.format = Format::rgba8_unorm, .blend = alphaBlend()}, {.format = Format::r32_uint}};
    for (size_t i = 0; i < 2; ++i) {
        const uint32_t samples = i == 0 ? 1u : 4u;
        r.scenePsos[i] = graphics(r,"sceneVertex","sceneFragment",{sceneColors,2},Format::d32_float,samples);
        r.markerPsos[i] = graphics(r,"markerVertex","sceneFragment",{sceneColors,2},Format::d32_float,samples);
        r.linePsos[i] = graphics(r,"sceneVertex","sceneFragment",{sceneColors,2},Format::d32_float,samples,PrimitiveTopology::lines);
        const char* entry = i == 0 ? "pickSingle" : "pickMsaa";
        const auto code = shader(entry);
        r.pickPsos[i] = create_compute_pso(r.device,stage(code,entry));
        require(r.pickPsos[i] != nullptr, "Pick compute PSO creation failed");
    }
    r.compositePso = graphics(r,"compositeVertex","compositeFragment",{{.format = Format::rgba8_unorm}});
    r.presentPso = graphics(r,"compositeVertex","compositeFragment",{{.format = Format::bgra8_unorm}});
    r.uiPso = graphics(r,"uiVertex","uiFragment",{{.format = Format::rgba8_unorm, .blend = alphaBlend()}});
}
void uploadFixture(Renderer& r, const Fixture& fixture)
{
    invalidateSelection(r.selection);
    if (r.geometry.owner) {
        auto old = r.geometry;
        r.deletes->defer(r.submitted,[old,&r]() noexcept { destroy_gpu_heap(old); ++r.retiredMeshes; });
    }
    // Deliberately exceeds the 64 KiB staging capacity to exercise bounded chunking.
    // Padding is uploaded with the mesh and checked by the GPU test separately.
    constexpr uint64_t heapBytes = 192*1024;
    r.geometry = create_gpu_heap(r.device,heapBytes,MemoryType::gpu_only);
    std::vector<byte> data(heapBytes,byte{0x5a});
    uint64_t offset = 0;
    for (size_t i = 0; i < 3; ++i) {
        const auto& vertices = fixture.state.files[i].mesh.vertices;
        const uint64_t bytes = vertices.size()*sizeof(Vertex);
        std::memcpy(data.data()+offset,vertices.data(),static_cast<size_t>(bytes));
        r.vertices[i] = reinterpret_cast<MeshVertex*>(r.geometry.range.gpu+offset);
        offset += (bytes+15)&~uint64_t{15};
    }
    const uint32_t triangles[] = {1,2,3}, edges[] = {1,2,2,3,3,1};
    std::memcpy(data.data()+offset,triangles,sizeof(triangles));
    r.triangleIndices = {r.geometry.range.gpu+offset,sizeof(triangles)};
    offset += 16;
    std::memcpy(data.data()+offset,edges,sizeof(edges));
    r.edgeIndices = {r.geometry.range.gpu+offset,sizeof(edges)};
    r.uploads->upload_buffer(gpu_range(r.geometry),{data.data(),data.size()});
    (void)r.uploads->flush();
}
void collect(Renderer& r)
{
    const auto completed = timeline_completed_value(r.timeline);
    // Complete in sequence order even when multiple frames finish together.
    std::array<Frame*,frameCount> order{};
    for (size_t i = 0; i < frameCount; ++i) order[i] = &r.frames[i];
    std::sort(order.begin(),order.end(),[](const Frame* a,const Frame* b) { return a->sequence < b->sequence; });
    for (auto* frame : order) if (frame->pending && frame->completion <= completed) completeFrame(r,*frame);
    r.deletes->tick();
    r.uploads->reclaim();
}
Frame& render(Renderer& r, const Fixture& fixture, RenderOptions o, ImDrawData* ui)
{
    require(o.width > 0 && o.height > 0 && o.width <= maxExtent && o.height <= maxExtent,"Prototype extent must be 1..2048");
    require(o.samples == 1 || o.samples == 4,"Prototype sample count must be 1 or 4");
    auto& f = r.frames[r.nextFrame++ % frameCount];
    wait_timeline({r.timeline,f.completion});
    collect(r);
    reset_command_pool(f.pool);
    f.allocator->reset();
    auto* cmd = begin_commands(f.pool);
    const auto swap = r.windowed ? acquire(cmd) : SwapchainFrame{};
    if (swap.render_view) {
        o.width = swap.extent.x; o.height = swap.extent.y;
        require(o.width <= maxExtent && o.height <= maxExtent,"Window exceeds prototype's 2048 pixel limit");
    }
    ensureTargets(r,f,cmd,o);
    prepareUiTextures(r,f,cmd,ui);
    set_texture_descriptor_heap(cmd,r.descriptors);
    set_sampler_descriptor_heap(cmd,r.samplers);
    barrier(cmd,Stage::all_commands,Access::shader_read | Access::transfer_read | Access::color_write | Access::depth_stencil_write,
        Stage::all_commands,Access::color_write | Access::depth_stencil_write | Access::shader_read | Access::index_read);
    auto result = allocate<PickResult>(f);
    *result.cpu = {};
    const ColorAttachment colors[] = {
        {.render_view = f.targets.color.view, .load = LoadOp::clear, .clear = {.x=.035f,.y=.05f,.z=.075f,.w=1},
            .resolve_view = f.targets.resolved.view},
        {.render_view = f.targets.ids.view, .load = LoadOp::clear, .clear = {.w=0}},
    };
    begin_render_pass(cmd,{.colors = {colors,2}, .depth = {.render_view = f.targets.depth.view,.load = LoadOp::clear}});
    set_depth_stencil(cmd,{.depth_test = true,.depth_write = true});
    const size_t psoIndex = o.samples == 1 ? 0 : 1;
    for (size_t file = 0; file < fixture.state.files.size(); ++file) {
        const auto& group = fixture.state.files[file].groupSettings[0];
        if (!group.visible) continue;
        auto root = allocate<DrawRoot>(f);
        *root.cpu = {.vertices = r.vertices[file], .color = {group.color[0],group.color[1],group.color[2],group.opacity},
            .inverseExtent = {1.0f/float(o.width),1.0f/float(o.height)},
            .firstId = file < 2 ? fixture.markers.draws[file].firstId : 0,
            .pointSize = fixture.state.masterVertexPointSize};
        if (group.showTriangles) {
            bind_pso(cmd,r.linePsos[psoIndex]);
            draw_indexed(cmd,root.gpu,r.edgeIndices,IndexType::uint32,6);
        }
        if (group.showSolidMesh) {
            bind_pso(cmd,r.scenePsos[psoIndex]);
            draw_indexed(cmd,root.gpu,r.triangleIndices,IndexType::uint32,3);
        }
        if (group.showVertices) {
            // Root addresses are immutable until frame completion; never overwrite a prior draw's root.
            auto marker = allocate<DrawRoot>(f);
            *marker.cpu = *root.cpu; marker.cpu->marker = 1;
            bind_pso(cmd,r.markerPsos[psoIndex]);
            draw(cmd,marker.gpu,6,4);
        }
    }
    end_render_pass(cmd);
    barrier(cmd,Stage::color_output,Access::color_write,Stage::compute | Stage::fragment,Access::shader_read);
    const auto pick = allocate<PickRoot>(f);
    *pick.cpu = {.result = result.gpu,.cursor = {o.cursor.x,o.cursor.y},.extent = {o.width,o.height},
        .idTexture = f.descriptorBase,.samples = o.samples};
    bind_pso(cmd,r.pickPsos[psoIndex]);
    dispatch(cmd,pick.gpu,{1,1,1});
    barrier(cmd,Stage::compute,Access::shader_write,Stage::fragment | Stage::transfer,Access::shader_read | Access::transfer_read);
    const auto composite = allocate<CompositeRoot>(f);
    *composite.cpu = {.result = result.gpu,.colorTexture = f.descriptorBase+1,.idTexture = f.descriptorBase,
        .samples = o.samples,.highlight = o.highlight ? 1u : 0u};
    begin_render_pass(cmd,{.colors = {{.render_view = f.targets.output.view,.load = LoadOp::discard}}});
    bind_pso(cmd,r.compositePso);
    draw(cmd,composite.gpu,3);
    drawUi(r,f,cmd,ui);
    end_render_pass(cmd);
    barrier(cmd,Stage::color_output,Access::color_write,Stage::transfer | Stage::fragment,Access::transfer_read | Access::shader_read);
    copy_memory(cmd,gpu_range(result),{f.readback.range.gpu,sizeof(PickResult)});
    if (o.capture) copy_texture_to_memory(cmd,f.targets.output.allocation.texture,imageReadback(f));
    if (swap.render_view) {
        auto present = allocate<CompositeRoot>(f);
        *present.cpu = *composite.cpu;
        present.cpu->colorTexture = f.descriptorBase+2;
        present.cpu->highlight = 0;
        begin_render_pass(cmd,{.colors = {{.render_view = swap.render_view,.load = LoadOp::discard}}});
        bind_pso(cmd,r.presentPso);
        draw(cmd,present.gpu,3);
        end_render_pass(cmd);
    }
    barrier(cmd,Stage::transfer,Access::transfer_write,Stage::host,Access::host_read);
    end_commands(cmd);
    f.completion = ++r.submitted;
    f.epoch = r.selection.epoch; f.sequence = ++r.nextSequence; f.pending = true; f.captured = o.capture;
    const auto uploadComplete = r.uploads->flush();
    const std::array submittedCommands{cmd};
    const std::array uploadWaits{uploadComplete};
    const SubmitDesc submitDesc{.commands = submittedCommands, .waits = uploadWaits,.completion = {r.timeline,f.completion}};
    if (swap.render_view) submit_and_present(r.device,submitDesc); else submit(r.device,submitDesc);
    return f;
}
void finish(Renderer& r, Frame& f) { wait_timeline({r.timeline,f.completion}); collect(r); }
std::vector<uint8_t> capturedPixels(const Frame& frame)
{
    require(frame.captured && !frame.pending,"Capture requires a completed captured frame");
    const auto* first = reinterpret_cast<const uint8_t*>(frame.readback.range.cpu+imageOffset);
    return {first,first+uint64_t(frame.targets.width)*frame.targets.height*4};
}
void savePng(const Frame& frame, const std::filesystem::path& path)
{
    auto pixels = capturedPixels(frame);
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
    auto* surface = SDL_CreateSurfaceFrom(static_cast<int>(frame.targets.width),static_cast<int>(frame.targets.height),
        SDL_PIXELFORMAT_RGBA32,pixels.data(),static_cast<int>(frame.targets.width*4));
    require(surface != nullptr,SDL_GetError());
    const auto utf8 = path.u8string();
    const bool saved = SDL_SavePNG(surface,reinterpret_cast<const char*>(utf8.c_str()));
    SDL_DestroySurface(surface);
    require(saved,SDL_GetError());
}
void shutdown(Renderer& r)
{
    if (!r.device) return;
    wait_idle(r.device);
    if (r.deletes) r.deletes->drain();
    destroyUiTextures(r);
    for (auto& f : r.frames) {
        destroyTargets(r,f.targets);
        f.allocator.reset();
        destroy_command_pool(f.pool); destroy_gpu_heap(f.memory); destroy_gpu_heap(f.readback);
    }
    for (auto* pso : r.scenePsos) destroy_pso(pso);
    for (auto* pso : r.markerPsos) destroy_pso(pso);
    for (auto* pso : r.linePsos) destroy_pso(pso);
    for (auto* pso : r.pickPsos) destroy_pso(pso);
    destroy_pso(r.compositePso); destroy_pso(r.presentPso); destroy_pso(r.uiPso);
    destroy_gpu_heap(r.geometry);
    r.uploads.reset(); r.deletes.reset(); r.textures.reset();
    destroy_texture_descriptor_heap(r.descriptors); destroy_sampler_descriptor_heap(r.samplers);
    destroy_texture_heap(r.textureHeap); destroy_timeline_semaphore(r.timeline);
    destroy_device(r.device); r.device = nullptr;
}
}

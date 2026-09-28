#include "renderer.h"
#include <SDL3/SDL.h>
#include <imgui_impl_sdl3.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>

namespace woby::ng {
namespace {
struct Controls {
    int scenario = 0;
    bool msaa = true, exportRequested = false, replaceRequested = false;
};
void newUiFrame(uint32_t width, uint32_t height, bool windowed)
{
    if (windowed) ImGui_ImplSDL3_NewFrame();
    else {
        ImGui::GetIO().DisplaySize = {float(width),float(height)};
        ImGui::GetIO().DisplayFramebufferScale = {1,1};
        ImGui::GetIO().DeltaTime = 1.0f/60.0f;
    }
    ImGui::NewFrame();
}
void diagnosticUi(const Renderer& r, Controls& controls, ImTextureData& tile, bool stress)
{
    ImGui::SetNextWindowPos({12,12},ImGuiCond_Always);
    ImGui::SetNextWindowSize({470,0},ImGuiCond_Always);
    ImGui::Begin("NoGraphicsAPI migration lab",nullptr,ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::TextUnformatted("woby / Vulkan / Slang");
    ImGui::TextUnformatted("Hover markers: gold highlight is computed on the GPU.");
    const char* names[] = {scenarioName(Scenario::overlapping),scenarioName(Scenario::translucent),scenarioName(Scenario::hidden),
        scenarioName(Scenario::zeroOpacity),scenarioName(Scenario::occluded)};
    ImGui::SetNextItemWidth(440);
    ImGui::Combo("##scenario",&controls.scenario,names,5);
    ImGui::Checkbox("Native 4x MSAA (experimental Vulkan extension)",&controls.msaa);
    if (ImGui::Button("Replace scene while frames are pending")) controls.replaceRequested = true;
    if (ImGui::Button("Export PNG")) controls.exportRequested = true;
    ImGui::SameLine(); ImGui::Text("CPU pick: %u",r.selection.id);
    ImGui::Text("Accepted: %u   stale: %u   retired meshes: %u",r.selection.accepted,r.selection.rejected,r.retiredMeshes);
    ImGui::End();
    auto* overlay = ImGui::GetForegroundDrawList();
    const auto size = ImGui::GetIO().DisplaySize;
    overlay->AddText({12,size.y-28},IM_COL32(190,208,230,255),"Blue: rear vertices + native lines   Coral: front vertices   Gold: GPU selection");
    if (tile.Status != ImTextureStatus_WantDestroy)
        overlay->AddImage(tile.GetTexRef(),{size.x-60,12},{size.x-20,52});
    if (stress) {
        // Cross the 16-bit vertex boundary in one draw list, then draw an identifiable pixel.
        for (int i = 0; i < 17000; ++i)
            overlay->AddRectFilled({size.x-15,size.y-15},{size.x-12,size.y-12},IM_COL32(70,90,110,255));
        overlay->AddRectFilled({size.x-28,size.y-28},{size.x-18,size.y-18},IM_COL32(20,240,80,255));
        overlay->PushClipRect({-20,-20},{-10,-10},false);
        overlay->AddRectFilled({0,0},size,IM_COL32(0,0,255,255));
        overlay->PopClipRect();
    }
    ImGui::Render();
}
ImDrawData* withExternalTexture(ImTextureData& tile, ImVector<ImTextureData*>& textures)
{
    auto* data = ImGui::GetDrawData();
    textures = *data->Textures;
    textures.push_back(&tile);
    data->Textures = &textures;
    return data;
}
void fillTile(ImTextureData& tile, uint8_t red, uint8_t green, uint8_t blue)
{
    for (int i = 0; i < tile.Width*tile.Height; ++i) {
        tile.Pixels[i*4] = red; tile.Pixels[i*4+1] = green; tile.Pixels[i*4+2] = blue; tile.Pixels[i*4+3] = 255;
    }
    if (tile.Status == ImTextureStatus_OK) {
        tile.Updates.push_back({0,0,static_cast<unsigned short>(tile.Width),static_cast<unsigned short>(tile.Height)});
        tile.SetStatus(ImTextureStatus_WantUpdates);
    }
}
std::array<uint8_t,4> pixel(const Frame& frame, uint32_t x, uint32_t y)
{
    const auto image = capturedPixels(frame);
    const size_t offset = (size_t(y)*frame.targets.width+x)*4;
    return {image[offset],image[offset+1],image[offset+2],image[offset+3]};
}
void check(bool success, const char* description)
{
    require(success,description);
    std::cout << "PASS " << description << '\n';
}
void checkUpload(Renderer& r)
{
    auto heap = gpu::create_gpu_heap(r.device,16,gpu::MemoryType::readback);
    auto* pool = gpu::create_command_pool(r.device);
    auto* cmd = gpu::begin_commands(pool);
    gpu::barrier(cmd,gpu::Stage::transfer,gpu::Access::transfer_write,gpu::Stage::transfer,gpu::Access::transfer_read);
    gpu::copy_memory(cmd,{r.geometry.range.gpu+r.geometry.range.size-16,16},gpu::gpu_range(heap));
    gpu::barrier(cmd,gpu::Stage::transfer,gpu::Access::transfer_write,gpu::Stage::host,gpu::Access::host_read);
    gpu::end_commands(cmd);
    const auto uploaded = r.uploads->flush();
    gpu::submit(r.device,{.commands = {cmd},.waits = {uploaded},.completion = {r.timeline,++r.submitted}});
    gpu::wait_timeline({r.timeline,r.submitted});
    bool correct = true;
    for (size_t i = 0; i < 16; ++i) correct = correct && heap.range.cpu[i] == gpu::byte{0x5a};
    gpu::destroy_command_pool(pool); gpu::destroy_gpu_heap(heap);
    check(correct && r.uploads->stats().peak_bytes <= 64*1024 && r.uploads->stats().submissions >= 3,
        "192 KiB geometry upload through bounded 64 KiB staging preserves tail bytes");
}
void runTests(Renderer& r, ImTextureData& tile, const std::filesystem::path& output, bool windowed)
{
    auto fixture = makeFixture(Scenario::overlapping);
    uploadFixture(r,fixture);
    checkUpload(r);
    const uint32_t width = windowed ? 960u : 640u, height = windowed ? 720u : 480u;
    for (const uint32_t samples : {1u,4u}) {
        for (int scenario = 0; scenario < static_cast<int>(Scenario::count); ++scenario) {
            fixture = makeFixture(static_cast<Scenario>(scenario));
            uploadFixture(r,fixture);
            auto& frame = render(r,fixture,{width,height,samples,{int(width/2),int(height/2)},true,true});
            check(r.selection.id == 0,"CPU selection stays empty before readback completion");
            finish(r,frame);
            std::cout << "  " << samples << "x: " << scenarioName(fixture.scenario) << " -> " << frame.result.id << '\n';
            check(frame.result.id == expectedPick(fixture.scenario),"GPU picking preserves large IDs, visibility, opacity and depth");
            if (scenario == 0) {
                bool edgeVisible = false;
                const int edgeX = static_cast<int>(float(width)*0.51f), edgeY = static_cast<int>(float(height)*0.7575f);
                for (int y = -2; y <= 2; ++y) for (int x = -2; x <= 2; ++x) {
                    const auto edgeColor = pixel(frame,static_cast<uint32_t>(edgeX+x),static_cast<uint32_t>(edgeY+y));
                    edgeVisible = edgeVisible || (edgeColor[2] > 50 && edgeColor[2] > edgeColor[1] && edgeColor[1] > edgeColor[0]*2);
                }
                check(edgeVisible,"Native line-list mesh edge survives the color resolve");
            }
            const auto center = pixel(frame,width/2,height/2);
            if (frame.result.id) check(center[0] > 250 && center[1] > 210 && center[2] < 40,"GPU highlight consumes selection in the same submission");
            if (!output.empty()) savePng(frame,output/(std::to_string(samples)+"x-case-"+std::to_string(scenario)+".png"));
        }
        fixture = makeFixture(Scenario::translucent);
        uploadFixture(r,fixture);
        auto& blend = render(r,fixture,{width,height,samples,{int(width/2),int(height/2)},true,false});
        finish(r,blend);
        const auto color = pixel(blend,width/2,height/2);
        check(std::abs(int(color[0])-105) <= 2 && std::abs(int(color[1])-139) <= 2 && std::abs(int(color[2])-175) <= 2,
            "Color target alpha-blends while integer ID target remains exact");
        auto& edge = render(r,fixture,{width,height,samples,{int(width/2)+13,int(height/2)},false,true});
        finish(r,edge);
        std::cout << "  mixed sample pixels: " << edge.result.mixedPixels << '\n';
        check(samples == 1 ? edge.result.mixedPixels == 0 : edge.result.mixedPixels > 0,
            "Unresolved ID attachment exposes native MSAA edge coverage");
    }
    const auto rejected = r.selection.rejected;
    const auto retired = r.retiredMeshes;
    for (uint32_t i = 0; i < 24; ++i) {
        fixture = makeFixture(i%2 ? Scenario::hidden : Scenario::overlapping);
        uploadFixture(r,fixture); // Does not collect: previous pick must be rejected, even if GPU already finished.
        const uint32_t w = windowed ? width : 151+i*7, h = windowed ? height : 127+i*3;
        (void)render(r,fixture,{w,h,i%2 ? 1u : 4u,{int(w/2),int(h/2)},false,true});
    }
    gpu::wait_idle(r.device); collect(r);
    check(r.selection.rejected > rejected && r.retiredMeshes >= retired+23,"Scene replacement rejects pending picks and retires old GPU buffers");
    check(r.selection.id == rearId,"Last scene wins after resize/MSAA/replacement stress");

    Controls controls;
    controls.scenario = static_cast<int>(fixture.scenario);
    ImVector<ImTextureData*> textures;
    std::array<Frame*,3> uiFrames{};
    for (int update = 0; update < 3; ++update) {
        fillTile(tile,update == 0 ? 240u : 20u,update == 0 ? 30u : 220u,60);
        newUiFrame(width,height,windowed);
        diagnosticUi(r,controls,tile,true);
        uiFrames[static_cast<size_t>(update)] = &render(r,fixture,{width,height,4,{int(width/2),int(height/2)},true,true},withExternalTexture(tile,textures));
    }
    // No waits between these texture updates: verify each frame kept its own version.
    for (size_t update = 0; update < uiFrames.size(); ++update) {
        auto& frame = *uiFrames[update];
        finish(r,frame);
        const auto texel = pixel(frame,width-40,32);
        check(update == 0 ? texel[0] > 230 && texel[1] < 40 : texel[0] < 30 && texel[1] > 210,
            "ImGui image creation/update appears in offscreen render");
        const auto offsetPixel = pixel(frame,width-23,height-23);
        check(offsetPixel[0] == 20 && offsetPixel[1] == 240,"ImGui draw after 65536 vertices honors VtxOffset");
        if (!output.empty()) savePng(frame,output/("imgui-update-"+std::to_string(update)+".png"));
        if (update == 2) {
            const auto unique = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
            const auto dir = std::filesystem::temp_directory_path()/std::filesystem::path("woby-ngapi-"+unique);
            const auto path = dir/std::filesystem::path(u8"export-\u00e9.png");
            try {
                savePng(frame,path);
                const auto utf8 = path.u8string();
                auto* decoded = SDL_LoadPNG(reinterpret_cast<const char*>(utf8.c_str()));
                auto* rgba = decoded ? SDL_ConvertSurface(decoded,SDL_PIXELFORMAT_RGBA32) : nullptr;
                bool valid = rgba && rgba->w == int(width) && rgba->h == int(height);
                const auto expected = capturedPixels(frame);
                if (valid) for (uint32_t y = 0; y < height; ++y) {
                    valid = valid && std::memcmp(static_cast<const uint8_t*>(rgba->pixels)+size_t(y)*rgba->pitch,
                        expected.data()+size_t(y)*width*4,size_t(width)*4) == 0;
                }
                SDL_DestroySurface(rgba);
                SDL_DestroySurface(decoded);
                check(valid,"PNG export round-trips every RGBA pixel, orientation and Unicode path");
            } catch (...) { std::filesystem::remove_all(dir); throw; }
            std::filesystem::remove_all(dir);
        }
    }
    check(r.uiOffsetDraws > 0 && r.uiClippedDraws > 0 && r.uiUpdates > 0,"ImGui large meshes, empty scissors and dynamic texture updates exercised");
    tile.WantDestroyNextFrame = true;
    tile.SetStatus(ImTextureStatus_WantDestroy);
    newUiFrame(width,height,windowed); diagnosticUi(r,controls,tile,false);
    auto& last = render(r,fixture,{width,height,4,{int(width/2),int(height/2)},false,true},withExternalTexture(tile,textures));
    finish(r,last);
    check(tile.BackendUserData == nullptr && r.uiDestroys > 0,"ImGui texture destruction retires its allocation and descriptor");
    std::cout << "All NoGraphicsAPI hardware acceptance checks passed.\n";
}
}
}

int main(int argc, char** argv)
{
    using namespace woby::ng;
    bool selfTest = false, windowTest = false;
    std::filesystem::path output;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--self-test") selfTest = true;
        else if (arg == "--window-test") windowTest = true;
        else if (arg == "--output" && i+1 < argc) output = std::filesystem::absolute(argv[++i]);
        else { std::cerr << "Usage: woby_nographicsapi_prototype [--self-test | --window-test] [--output directory]\n"; return 2; }
    }
    SDL_Window* window = nullptr;
    Renderer renderer;
    ImTextureData tile;
    bool platform = false, context = false;
    int exitCode = 0;
    try {
        require(SDL_Init(SDL_INIT_VIDEO),SDL_GetError());
        if (!selfTest) {
            window = SDL_CreateWindow("woby - NoGraphicsAPI migration lab",960,720,
                SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | (windowTest ? SDL_WINDOW_HIDDEN : 0));
            require(window != nullptr,SDL_GetError());
            SDL_SetWindowMaximumSize(window,int(maxExtent),int(maxExtent));
        }
        void* hwnd = window ? SDL_GetPointerProperty(SDL_GetWindowProperties(window),SDL_PROP_WINDOW_WIN32_HWND_POINTER,nullptr) : nullptr;
        initialize(renderer,hwnd);
        IMGUI_CHECKVERSION(); ImGui::CreateContext(); context = true;
        ImGui::StyleColorsDark();
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.BackendRendererName = "woby_nographicsapi_experiment";
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset;
        ImGui::GetPlatformIO().Renderer_TextureMaxWidth = int(maxExtent);
        ImGui::GetPlatformIO().Renderer_TextureMaxHeight = int(maxExtent);
        if (window) { require(ImGui_ImplSDL3_InitForOther(window),"SDL ImGui backend initialization failed"); platform = true; }
        tile.Create(ImTextureFormat_RGBA32,32,32);
        fillTile(tile,240,30,60);
        if (selfTest || windowTest) runTests(renderer,tile,output,windowTest);
        else {
            Controls controls;
            auto fixture = makeFixture(Scenario::overlapping);
            uploadFixture(renderer,fixture);
            ImVector<ImTextureData*> textures;
            bool running = true;
            while (running) {
                SDL_Event event;
                while (SDL_PollEvent(&event)) {
                    ImGui_ImplSDL3_ProcessEvent(&event);
                    if (event.type == SDL_EVENT_QUIT) running = false;
                }
                const auto extent = gpu::get_drawable_extent(renderer.device);
                if (!extent.x || !extent.y) { SDL_Delay(16); continue; }
                collect(renderer);
                newUiFrame(extent.x,extent.y,true);
                diagnosticUi(renderer,controls,tile,false);
                if (controls.scenario != static_cast<int>(fixture.scenario) || controls.replaceRequested) {
                    fixture = makeFixture(static_cast<Scenario>(controls.scenario));
                    uploadFixture(renderer,fixture); controls.replaceRequested = false;
                }
                const auto& input = ImGui::GetIO();
                const auto cursor = input.WantCaptureMouse ? std::optional<Pixel>{} :
                    cursorPixel(input.MousePos.x,input.MousePos.y,input.DisplayFramebufferScale.x,input.DisplayFramebufferScale.y,
                        {0,extent.x,extent.y,0});
                auto& frame = render(renderer,fixture,{extent.x,extent.y,controls.msaa ? 4u : 1u,cursor.value_or(Pixel{-100,-100}),
                    controls.exportRequested,true},withExternalTexture(tile,textures));
                if (controls.exportRequested) {
                    finish(renderer,frame);
                    const auto path = (output.empty() ? std::filesystem::current_path() : output)/"nographicsapi-prototype.png";
                    savePng(frame,path); std::cout << "Exported " << path << '\n'; controls.exportRequested = false;
                }
                SDL_Delay(1);
            }
        }
    } catch (const std::exception& error) { std::cerr << "Prototype failed: " << error.what() << '\n'; exitCode = 1; }
    shutdown(renderer);
    if (platform) ImGui_ImplSDL3_Shutdown();
    if (context) ImGui::DestroyContext();
    SDL_DestroyWindow(window); SDL_Quit();
    return exitCode;
}

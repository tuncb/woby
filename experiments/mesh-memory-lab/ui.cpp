#include "ui.h"
#include "utf8_path.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace mesh_lab {
namespace {
constexpr ImVec4 ink{0.88f, 0.92f, 0.95f, 1.0f};
constexpr ImVec4 muted{0.52f, 0.62f, 0.69f, 1.0f};
constexpr ImVec4 mint{0.36f, 0.89f, 0.71f, 1.0f};
constexpr ImVec4 blue{0.44f, 0.70f, 0.97f, 1.0f};
constexpr ImVec4 amber{0.96f, 0.72f, 0.38f, 1.0f};
constexpr ImVec4 purple{0.75f, 0.61f, 0.95f, 1.0f};
constexpr const char* components[] = {"p.x", "p.y", "p.z", "n.x", "n.y", "n.z", "uv.u", "uv.v"};
ImU32 color(ImVec4 value) { return ImGui::ColorConvertFloat4ToU32(value); }
ImVec4 fieldColor(size_t component) { return component < 3 ? blue : (component < 6 ? purple : amber); }
void label(const char* text) { ImGui::TextColored(muted, "%s", text); }
void section(const char* title, const char* detail)
{
    ImGui::TextUnformatted(title);
    if (detail) { ImGui::SameLine(); ImGui::TextColored(muted, "  %s", detail); }
    ImGui::Spacing();
}
void textAt(ImDrawList* draw, ImVec2 p, ImVec4 shade, const char* text)
{
    draw->AddText(p, color(shade), text);
}
void rowHighlight(bool selected)
{
    if (selected) { ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(45, 98, 88, 100)); }
}
void sourcePanel(UiState& state, const Trace& trace, bool follow)
{
    section("OBJ source", "ASCII records / byte offsets");
    ImGui::TextWrapped("Select a record to follow its first use. Highlighted records contribute to the selected triangle.");
    if (ImGui::BeginTable("source", 3, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable, ImVec2(0, -1))) {
        ImGui::TableSetupColumn("Line", ImGuiTableColumnFlags_WidthFixed, 47);
        ImGui::TableSetupColumn("File +byte", ImGuiTableColumnFlags_WidthFixed, 85);
        ImGui::TableSetupColumn("Record"); ImGui::TableSetupScrollFreeze(0, 1); ImGui::TableHeadersRow();
        ImGuiListClipper clipper; clipper.Begin(static_cast<int>(trace.lines.size()));
        const auto selectedLine = trace.faces[trace.triangles[state.triangle].face].line;
        if (follow) { clipper.IncludeItemByIndex(static_cast<int>(selectedLine)); }
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const size_t id = static_cast<size_t>(i);
                const auto& line = trace.lines[id];
                ImGui::PushID(i); ImGui::TableNextRow(); rowHighlight(lineRelated(trace, id, state.triangle));
                ImGui::TableNextColumn(); char number[32]; std::snprintf(number, sizeof(number), "%d", i + 1);
                if (ImGui::Selectable(number, false, ImGuiSelectableFlags_SpanAllColumns)) { selectLine(state, trace, id); }
                ImGui::TableNextColumn(); ImGui::TextColored(muted, "%08zx", line.byteOffset);
                ImGui::TableNextColumn();
                ImGui::TextColored(line.face >= 0 ? mint : (line.position >= 0 ? blue : (line.normal >= 0 ? purple : (line.texcoord >= 0 ? amber : muted))), "%s", line.text.c_str());
                if (follow && id == selectedLine) { ImGui::SetScrollHereY(0.5f); }
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
}
void cornerPanel(UiState& state, const Trace& trace, bool follow)
{
    section("Triangulated corner stream", "resolved 0-based OBJ indices");
    const auto selectedKey = trace.triangles[state.triangle].corners[state.corner];
    const auto& p = trace.positions[static_cast<size_t>(selectedKey.position)];
    ImGui::TextColored(blue, "positions: double[%zu][3] / %zu B", trace.positions.size(), trace.positions.size()*24);
    ImGui::Text("  [%d] = (%.12g, %.12g, %.12g)", selectedKey.position, p[0], p[1], p[2]);
    ImGui::TextColored(purple, "normals: float[%zu][3] / %zu B", trace.normals.size(), trace.normals.size()*12);
    if (selectedKey.normal >= 0) {
        const auto& n = trace.normals[static_cast<size_t>(selectedKey.normal)];
        ImGui::Text("  [%d] = (%.5g, %.5g, %.5g)", selectedKey.normal, n[0], n[1], n[2]);
    }
    ImGui::TextColored(amber, "texcoords: float[%zu][2] / %zu B", trace.texcoords.size(), trace.texcoords.size()*8);
    if (selectedKey.texcoord >= 0) {
        const auto& uv = trace.texcoords[static_cast<size_t>(selectedKey.texcoord)];
        ImGui::Text("  [%d] = (%.5g, %.5g) -> render (%.5g, %.5g)", selectedKey.texcoord, uv[0], uv[1], uv[0], 1.0f-uv[1]);
    }
    label("Trace-owned snapshots of the independent attribute arrays.");
    ImGui::Separator();
    ImGui::TextWrapped("Tuple identity is (position, normal, texcoord). -1 means absent. Repeated tuples share one render vertex.");
    if (ImGui::BeginTable("corners", 6, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg, ImVec2(0, -1))) {
        for (const auto* name : {"T / corner", "face / line", "p", "n", "uv", "render v"}) { ImGui::TableSetupColumn(name); }
        ImGui::TableSetupScrollFreeze(0, 1); ImGui::TableHeadersRow();
        ImGuiListClipper clipper; clipper.Begin(static_cast<int>(trace.mesh.indices.size()));
        const size_t selectedIndex = state.triangle * 3 + state.corner;
        if (follow) { clipper.IncludeItemByIndex(static_cast<int>(selectedIndex)); }
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const size_t id = static_cast<size_t>(i), t = id / 3, c = id % 3;
                const auto& tri = trace.triangles[t]; const auto& key = tri.corners[c];
                ImGui::PushID(i); ImGui::TableNextRow(); rowHighlight(t == state.triangle);
                ImGui::TableNextColumn(); char name[40]; std::snprintf(name, sizeof(name), "T%zu / %zu", t, c);
                if (ImGui::Selectable(name, state.triangle == t && state.corner == c, ImGuiSelectableFlags_SpanAllColumns)) {
                    selectTriangle(state, trace, t); selectCorner(state, c);
                }
                ImGui::TableNextColumn(); ImGui::Text("%zu / L%zu", tri.face, trace.faces[tri.face].line+1);
                ImGui::TableNextColumn(); ImGui::TextColored(blue, "%d", key.position);
                ImGui::TableNextColumn(); ImGui::TextColored(purple, "%d", key.normal);
                ImGui::TableNextColumn(); ImGui::TextColored(amber, "%d", key.texcoord);
                ImGui::TableNextColumn(); ImGui::TextColored(mint, "%u", trace.mesh.indices[id]);
                if (follow && id == selectedIndex) { ImGui::SetScrollHereY(0.5f); }
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
}
void vertexPanel(UiState& state, const Trace& trace, bool follow)
{
    section("CPU / interleaved render vertices", "woby::Vertex");
    ImGui::TextColored(muted, "sizeof = 32 B   alignof = %zu   indices = uint32_t", alignof(woby::Vertex));
    if (ImGui::BeginTable("vertices", 5, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable, ImVec2(0, -1))) {
        ImGui::TableSetupColumn("v / +byte", ImGuiTableColumnFlags_WidthFixed, 97);
        ImGui::TableSetupColumn("p / n / uv", ImGuiTableColumnFlags_WidthFixed, 98);
        ImGui::TableSetupColumn("position [3]"); ImGui::TableSetupColumn("normal [3]"); ImGui::TableSetupColumn("texcoord [2]");
        ImGui::TableSetupScrollFreeze(0, 1); ImGui::TableHeadersRow();
        const auto selected = selectedVertex(state, trace);
        const auto sourcePosition = trace.vertexKeys[selected].position;
        ImGuiListClipper clipper; clipper.Begin(static_cast<int>(trace.mesh.vertices.size()));
        if (follow) { clipper.IncludeItemByIndex(static_cast<int>(selected)); }
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const auto id = static_cast<size_t>(i); const auto& v = trace.mesh.vertices[id]; const auto& key = trace.vertexKeys[id];
                ImGui::PushID(i); ImGui::TableNextRow(); rowHighlight(key.position == sourcePosition);
                ImGui::TableNextColumn(); char name[48]; std::snprintf(name, sizeof(name), "v%d +%04zx", i, id * sizeof(woby::Vertex));
                if (ImGui::Selectable(name, selected == id, ImGuiSelectableFlags_SpanAllColumns)) { selectVertex(state, trace, id); }
                ImGui::TableNextColumn(); ImGui::Text("%d / %d / %d", key.position, key.normal, key.texcoord);
                ImGui::TableNextColumn(); ImGui::TextColored(blue, "%.5g\n%.5g\n%.5g", v.position[0], v.position[1], v.position[2]);
                ImGui::TableNextColumn(); ImGui::TextColored(purple, "%.4g\n%.4g\n%.4g", v.normal[0], v.normal[1], v.normal[2]);
                ImGui::TableNextColumn(); ImGui::TextColored(amber, "%.4g\n%.4g", v.texcoord[0], v.texcoord[1]);
                if (follow && id == selected) { ImGui::SetScrollHereY(0.5f); }
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
}
void gpuPanel(UiState& state, const Trace& trace, const GpuCapture& gpu, bool follow)
{
    section("GPU / actual buffer capture", "NoGraphicsAPI");
    ImGui::TextColored(gpu.complete && gpu.matches ? mint : amber, "%s", gpu.complete ? (gpu.matches ? "Readback verified: every VB + IB byte matches" : "ERROR: readback differs from upload") : "Readback in flight; no GPU bytes reported yet");
    ImGui::Separator();
    ImGui::Text("VB handle %u     %zu bytes     stride 32", gpu.vertices.idx, gpu.vertexUpload.size());
    ImGui::Text("IB handle %u     %zu bytes     uint32 indices", gpu.indices.idx, gpu.indexUpload.size());
    label("Handles are opaque. Addresses below are logical buffer offsets.");
    ImGui::Spacing();
    ImGui::TextColored(muted, "Upload lifecycle");
    ImGui::TextUnformatted("Mesh vectors -> graphics::copy -> owned staging");
    ImGui::TextUnformatted("-> device buffers -> indexed draw -> readback");
    ImGui::Spacing();
    ImGui::TextWrapped("The backend vertex shader pulls attributes from root.vertices. SV_VertexID comes from the uint32 index buffer; root.stride is measured in floats (8), not bytes (32).");
    ImGui::Spacing();
    if (ImGui::BeginTable("indices", 4, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg, ImVec2(0, -1))) {
        for (const auto* name : {"triangle", "IB +byte", "uint32[3]", "readback"}) { ImGui::TableSetupColumn(name); }
        ImGui::TableSetupScrollFreeze(0, 1); ImGui::TableHeadersRow();
        ImGuiListClipper clipper; clipper.Begin(static_cast<int>(trace.triangles.size()));
        if (follow) { clipper.IncludeItemByIndex(static_cast<int>(state.triangle)); }
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const auto t = static_cast<size_t>(i); ImGui::PushID(i); ImGui::TableNextRow(); rowHighlight(t == state.triangle);
                ImGui::TableNextColumn(); char title[32]; std::snprintf(title, sizeof(title), "T%d", i);
                if (ImGui::Selectable(title, t == state.triangle, ImGuiSelectableFlags_SpanAllColumns)) { selectTriangle(state, trace, t); }
                ImGui::TableNextColumn(); ImGui::Text("0x%04zx", t * 12);
                ImGui::TableNextColumn(); ImGui::Text("%u, %u, %u", trace.mesh.indices[t*3], trace.mesh.indices[t*3+1], trace.mesh.indices[t*3+2]);
                ImGui::TableNextColumn(); ImGui::TextColored(gpu.matches ? mint : muted, "%s", gpu.complete ? (gpu.matches ? "equal" : "mismatch") : "pending");
                if (follow && t == state.triangle) { ImGui::SetScrollHereY(0.5f); }
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
}
void lineage(UiState& state, const Trace& trace)
{
    const auto v = selectedVertex(state, trace); const auto key = trace.vertexKeys[v];
    section("Identity / selected corner", "indices are 0-based");
    ImGui::Text("T%zu  corner %zu  ->  source p%d  ->  render v%u", state.triangle, state.corner, key.position, v);
    const auto p = ImGui::GetCursorScreenPos(); const float width = ImGui::GetContentRegionAvail().x;
    auto* draw = ImGui::GetWindowDrawList();
    const float boxWidth = (width - 42) / 3;
    const char* names[] = {"OBJ tuple", "CPU vertex", "GPU fetch"};
    char values[3][72];
    std::snprintf(values[0], sizeof(values[0]), "(%d, %d, %d)", key.position, key.normal, key.texcoord);
    std::snprintf(values[1], sizeof(values[1]), "v%u +0x%04zx", v, static_cast<size_t>(v) * 32);
    std::snprintf(values[2], sizeof(values[2]), "IB[%zu] = %u", state.triangle*3+state.corner, v);
    for (int i = 0; i < 3; ++i) {
        const float x = p.x + static_cast<float>(i) * (boxWidth + 21);
        draw->AddRectFilled({x, p.y}, {x+boxWidth, p.y+65}, IM_COL32(30, 43, 53, 255), 4);
        textAt(draw, {x+10, p.y+9}, muted, names[i]); textAt(draw, {x+10, p.y+35}, i == 0 ? amber : mint, values[i]);
        if (i < 2) { textAt(draw, {x+boxWidth+4, p.y+28}, muted, ">"); }
    }
    ImGui::Dummy({width, 73});
    const auto& aliases = trace.positionVertices[static_cast<size_t>(key.position)];
    ImGui::TextColored(muted, "Source p%d aliases:", key.position);
    for (size_t i = 0; i < std::min(aliases.size(), size_t{10}); ++i) {
        if (i < 5) { ImGui::SameLine(); }
        char title[32]; std::snprintf(title, sizeof(title), "v%u", aliases[i]);
        if (ImGui::Button(title)) { selectVertex(state, trace, aliases[i]); }
    }
    if (aliases.size() > 10) { ImGui::SameLine(); ImGui::Text("+%zu more", aliases.size()-10); }
}
void byteInspector(UiState& state, const Trace& trace, const GpuCapture& gpu)
{
    const auto v = selectedVertex(state, trace); const size_t start = static_cast<size_t>(v) * 32;
    section("BYTE INSPECTOR", "select a float to inspect the exact bit pattern");
    ImGui::Text("vertices[%u]  host %p   |   VB +0x%04zx   |   32-byte AoS record", v,
        static_cast<const void*>(&trace.mesh.vertices[v]), start);
    const float available = ImGui::GetContentRegionAvail().x;
    const float cell = (available - 7 * 7) / 8;
    for (size_t c = 0; c < 8; ++c) {
        if (c) { ImGui::SameLine(0, 7); }
        ImGui::PushID(static_cast<int>(c));
        const auto pos = ImGui::GetCursorScreenPos(); auto* draw = ImGui::GetWindowDrawList();
        const auto shade = fieldColor(c);
        draw->AddRectFilled(pos, {pos.x+cell, pos.y+89}, state.component == c ? IM_COL32(39, 57, 67, 255) : IM_COL32(24, 35, 44, 255), 3);
        draw->AddRectFilled(pos, {pos.x+cell, pos.y+3}, color(shade));
        char name[48], bytes[32], value[48]; float f = 0;
        std::memcpy(&f, gpu.vertexUpload.data()+start+c*4, sizeof(f));
        std::snprintf(name, sizeof(name), "%s  +%02zu", components[c], c * 4);
        const auto* b = gpu.vertexUpload.data()+start+c*4;
        std::snprintf(bytes, sizeof(bytes), "%02X %02X %02X %02X", b[0], b[1], b[2], b[3]);
        std::snprintf(value, sizeof(value), "%.8g", f);
        textAt(draw, {pos.x+9, pos.y+12}, shade, name);
        textAt(draw, {pos.x+9, pos.y+37}, ink, bytes);
        textAt(draw, {pos.x+9, pos.y+63}, muted, value);
        if (ImGui::InvisibleButton(components[c], {cell, 89}, ImGuiButtonFlags_EnableNav)) { selectComponent(state, c); }
        ImGui::PopID();
    }
    ImGui::Spacing();
    const size_t byte = start + state.component * 4;
    uint32_t bits = 0; float f = 0;
    std::memcpy(&bits, gpu.vertexUpload.data()+byte, 4); std::memcpy(&f, &bits, 4);
    ImGui::TextColored(fieldColor(state.component), "%s", components[state.component]); ImGui::SameLine();
    ImGui::Text("float32 %.9g  |  bits 0x%08X  |  sign %u  exponent %u  mantissa 0x%06X",
        f, bits, bits >> 31, (bits >> 23) & 255u, bits & 0x7fffffu);
    const auto key = trace.vertexKeys[v]; const auto& original = trace.positions[static_cast<size_t>(key.position)];
    const size_t axis = std::min(state.component, size_t{2});
    if (state.component < 3) {
        const double local = trace.mesh.precisePositions[v][axis];
        ImGui::Text("double %.17g - origin %.17g = %.17g  ->  float %.9g  |  error %.3g",
            original[axis], trace.mesh.origin[axis], local, f, static_cast<double>(f)-local);
    } else if (state.component < 6) {
        ImGui::TextColored(muted, "%s", trace.generatedNormals ? "Normals: normalized sum of unit face normals; one missing/invalid normal regenerates all render normals." : "Normals: authored OBJ values are copied into the render vertex.");
    } else { ImGui::TextColored(muted, "UV mapping: u = OBJ.u, v = 1 - OBJ.v; missing UVs stay (0, 0)."); }
    ImGui::TextColored(gpu.matches ? mint : muted, "GPU +0x%04zx: %s   |   root.vertices[%u * 8 + %zu]   |   %s",
        byte, gpu.complete ? (gpu.matches ? "4 bytes verified" : "capture mismatch") : "pending readback", v, state.component,
        std::endian::native == std::endian::little ? "little-endian" : "big-endian");
}
} // namespace

void configureStyle(UiRuntime& runtime, const std::filesystem::path& assets)
{
    auto& io = ImGui::GetIO(); io.IniFilename = nullptr; io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    const auto regularPath = woby::pathToUtf8(assets / "fonts/Lato-Regular.ttf");
    const auto monoPath = woby::pathToUtf8(assets / "fonts/RobotoMonoNerdFont-Regular.ttf");
    runtime.regular = io.Fonts->AddFontFromFileTTF(regularPath.c_str(), 17);
    runtime.title = io.Fonts->AddFontFromFileTTF(regularPath.c_str(), 28);
    runtime.mono = io.Fonts->AddFontFromFileTTF(monoPath.c_str(), 14);
    auto& s = ImGui::GetStyle(); ImGui::StyleColorsDark();
    s.WindowPadding = {20, 16}; s.FramePadding = {10, 7}; s.ItemSpacing = {10, 8}; s.CellPadding = {8, 7};
    s.WindowRounding = 0; s.ChildRounding = 6; s.FrameRounding = 4; s.PopupRounding = 5;
    s.WindowBorderSize = 0; s.ChildBorderSize = 1; s.ScrollbarSize = 12;
    s.Colors[ImGuiCol_Text] = ink; s.Colors[ImGuiCol_TextDisabled] = muted;
    s.Colors[ImGuiCol_WindowBg] = {0.055f, 0.079f, 0.10f, 1};
    s.Colors[ImGuiCol_ChildBg] = {0.073f, 0.10f, 0.125f, 1};
    s.Colors[ImGuiCol_Border] = {0.16f, 0.21f, 0.25f, 1};
    s.Colors[ImGuiCol_Button] = {0.13f, 0.19f, 0.23f, 1};
    s.Colors[ImGuiCol_ButtonHovered] = {0.19f, 0.29f, 0.33f, 1};
    s.Colors[ImGuiCol_ButtonActive] = {0.23f, 0.39f, 0.37f, 1};
    s.Colors[ImGuiCol_Header] = {0.14f, 0.28f, 0.27f, 1};
    s.Colors[ImGuiCol_HeaderHovered] = {0.18f, 0.33f, 0.32f, 1};
    s.Colors[ImGuiCol_HeaderActive] = {0.20f, 0.37f, 0.34f, 1};
    s.Colors[ImGuiCol_TableHeaderBg] = {0.10f, 0.15f, 0.18f, 1};
}
void drawUi(UiRuntime& runtime, UiState& state, const Trace& trace, const GpuCapture& gpu, Viewport& view)
{
    auto& io = ImGui::GetIO();
    ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("Mesh memory lab", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PushFont(runtime.title); ImGui::TextUnformatted("Mesh memory lab"); ImGui::PopFont();
    ImGui::SameLine(); ImGui::SetCursorPosX(280); ImGui::SetCursorPosY(23); ImGui::TextColored(muted, "FILE  /  CPU  /  GPU");
    ImGui::SameLine(); ImGui::SetCursorPosX(std::max(600.0f, io.DisplaySize.x - 395));
    ImGui::TextColored(mint, "LIVE CAPTURE"); ImGui::SameLine(); ImGui::TextColored(muted, "  SDL3 + ImGui + NoGraphicsAPI");
    ImGui::Spacing();
    ImGui::BeginDisabled(runtime.loading);
    ImGui::SetNextItemWidth(235);
    if (ImGui::BeginCombo("##sample", trace.name.c_str())) {
        for (size_t i = 0; i < std::size(sampleFiles); ++i) {
            if (ImGui::Selectable(sampleNames[i], trace.name == sampleFiles[i])) {
                runtime.requestedPath = std::filesystem::path(MESH_LAB_SAMPLE_DIRECTORY) / sampleFiles[i];
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Open OBJ...")) { ImGui::OpenPopup("Open capture"); }
    const ImVec2 popup{ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y + 5};
    ImGui::SetNextWindowPos(popup, ImGuiCond_Appearing);
    if (ImGui::BeginPopup("Open capture")) {
        ImGui::TextUnformatted("Absolute OBJ path (or drop a file onto the window)");
        ImGui::SetNextItemWidth(510); ImGui::InputText("##path", runtime.pathInput, sizeof(runtime.pathInput));
        label("Prototype: polygonal OBJ, <=512 KiB, <=20,000 triangles.");
        if (ImGui::Button("Capture")) { runtime.requestedPath = woby::pathFromUtf8(runtime.pathInput); ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
    ImGui::EndDisabled(); ImGui::SameLine();
    ImGui::TextColored(muted, "%zu positions  /  %zu faces  /  %zu triangles  /  %zu seam positions",
        trace.positions.size(), trace.faces.size(), trace.triangles.size(), trace.splitPositions);
    if (runtime.loading) { ImGui::SameLine(); ImGui::TextColored(amber, "Capturing..."); }
    if (!runtime.error.empty()) { ImGui::TextColored(amber, "%s", runtime.error.c_str()); }
    ImGui::Spacing();
    const float width = ImGui::GetContentRegionAvail().x;
    const float stageWidth = (width - 30) / 4;
    const char* stages[] = {"01   FILE", "02   TRIANGULATE", "03   CPU LAYOUT", "04   GPU BUFFERS"};
    char stats[4][100];
    std::snprintf(stats[0], sizeof(stats[0]), "%zu B / OBJ text", trace.source.size());
    std::snprintf(stats[1], sizeof(stats[1]), "%zu polygon corners -> %zu triangle corners", trace.originalCorners, trace.mesh.indices.size());
    std::snprintf(stats[2], sizeof(stats[2]), "%zu vertices x 32 B + %zu B indices", trace.mesh.vertices.size(), trace.mesh.indices.size()*4);
    std::snprintf(stats[3], sizeof(stats[3]), "%zu B vertex + index payload", gpu.vertexUpload.size()+gpu.indexUpload.size());
    for (int i = 0; i < 4; ++i) {
        if (i) { ImGui::SameLine(); }
        ImGui::PushID(i);
        const auto pos = ImGui::GetCursorScreenPos(); auto* draw = ImGui::GetWindowDrawList();
        const bool active = state.stage == static_cast<Stage>(i);
        draw->AddRectFilled(pos, {pos.x+stageWidth, pos.y+66}, active ? IM_COL32(28, 59, 57, 255) : IM_COL32(24, 35, 44, 255), 5);
        textAt(draw, {pos.x+13, pos.y+11}, active ? mint : ink, stages[i]);
        textAt(draw, {pos.x+13, pos.y+38}, muted, stats[i]);
        if (ImGui::InvisibleButton(stages[i], {stageWidth, 66}, ImGuiButtonFlags_EnableNav)) { selectStage(state, static_cast<Stage>(i)); }
        ImGui::PopID();
    }
    ImGui::Spacing();
    const float bodyHeight = std::max(285.0f, ImGui::GetContentRegionAvail().y - 298.0f);
    const float leftWidth = width * 0.57f;
    ImGui::BeginChild("data", {leftWidth, bodyHeight}, ImGuiChildFlags_Borders);
    ImGui::PushFont(runtime.mono);
    const bool follow = runtime.lastTrace != &trace || runtime.lastStage != state.stage
        || runtime.lastTriangle != state.triangle || runtime.lastCorner != state.corner;
    runtime.lastTrace = &trace; runtime.lastStage = state.stage;
    runtime.lastTriangle = state.triangle; runtime.lastCorner = state.corner;
    switch (state.stage) {
    case Stage::source: sourcePanel(state, trace, follow); break;
    case Stage::corners: cornerPanel(state, trace, follow); break;
    case Stage::vertices: vertexPanel(state, trace, follow); break;
    case Stage::gpu: gpuPanel(state, trace, gpu, follow); break;
    }
    ImGui::PopFont(); ImGui::EndChild(); ImGui::SameLine();
    ImGui::BeginChild("geometry-and-lineage", {0, bodyHeight}, ImGuiChildFlags_Borders);
    ImGui::TextUnformatted("Triangle assembly"); ImGui::SameLine();
    ImGui::TextColored(muted, "Drag / zoom / x-ray labels");
    const ImVec2 canvas{ImGui::GetContentRegionAvail().x, std::max(95.0f, bodyHeight - 300)};
    resizeViewport(view, static_cast<uint16_t>(std::clamp(canvas.x, 1.0f, 2048.0f)), static_cast<uint16_t>(std::clamp(canvas.y, 1.0f, 2048.0f)));
    const auto canvasOrigin = ImGui::GetCursorScreenPos();
    ImGui::Image(static_cast<ImTextureID>(view.color.idx) + 1, canvas);
    if (ImGui::IsItemHovered()) {
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) { orbit(state, -io.MouseDelta.x * 0.009f, io.MouseDelta.y * 0.009f, 1); }
        if (io.MouseWheel != 0) { orbit(state, 0, 0, std::pow(1.12f, io.MouseWheel)); }
    }
    // X-ray provenance overlay: these labels stay visible for occluded corners.
    const auto matrices = viewMatrices(trace, state, canvas.x/canvas.y, woby::graphics::getCaps()->homogeneousDepth);
    auto* draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(canvasOrigin, {canvasOrigin.x+canvas.x, canvasOrigin.y+canvas.y}, true);
    std::array<ImVec2, 3> points;
    for (size_t c = 0; c < 3; ++c) {
        const auto vertex = trace.mesh.indices[state.triangle*3+c];
        const auto projected = projectVertex(matrices, trace.mesh.vertices[vertex]);
        points[c] = {canvasOrigin.x+projected[0]*canvas.x, canvasOrigin.y+projected[1]*canvas.y};
    }
    draw->AddTriangle(points[0], points[1], points[2], color(mint), 1.5f);
    const ImVec2 centroid{(points[0].x+points[1].x+points[2].x)/3, (points[0].y+points[1].y+points[2].y)/3};
    for (size_t c = 0; c < 3; ++c) {
        const auto vertex = trace.mesh.indices[state.triangle*3+c];
        draw->AddCircleFilled(points[c], c == state.corner ? 5.5f : 3.5f, color(c == state.corner ? amber : mint));
        char text[32]; std::snprintf(text, sizeof(text), "c%zu / v%u", c, vertex);
        const auto size = ImGui::CalcTextSize(text);
        const float x = points[c].x < centroid.x ? points[c].x - size.x - 9 : points[c].x + 9;
        const float y = points[c].y < centroid.y ? points[c].y - size.y - 6 : points[c].y + 6;
        textAt(draw, {std::clamp(x, canvasOrigin.x+3, canvasOrigin.x+canvas.x-size.x-3),
            std::clamp(y, canvasOrigin.y+3, canvasOrigin.y+canvas.y-size.y-3)}, c == state.corner ? amber : ink, text);
    }
    draw->PopClipRect();
    ImGui::SetNextItemWidth(160); int triangle = static_cast<int>(state.triangle);
    if (ImGui::SliderInt("Triangle", &triangle, 0, static_cast<int>(trace.triangles.size()) - 1)) { selectTriangle(state, trace, static_cast<size_t>(triangle)); }
    ImGui::SameLine();
    for (size_t c = 0; c < 3; ++c) {
        if (c) { ImGui::SameLine(); }
        char title[28]; std::snprintf(title, sizeof(title), "c%zu", c);
        if (ImGui::RadioButton(title, c == state.corner)) { selectCorner(state, c); }
    }
    ImGui::PushFont(runtime.mono); lineage(state, trace); ImGui::PopFont();
    ImGui::EndChild();
    ImGui::Spacing();
    ImGui::BeginChild("bytes", {0, 252}, ImGuiChildFlags_Borders);
    ImGui::PushFont(runtime.mono); byteInspector(state, trace, gpu); ImGui::PopFont();
    ImGui::EndChild();
    ImGui::TextColored(muted, "Production OBJ loader + exact native layouts  |  CPU double sidecar: %zu B  |  GPU payload excludes CPU metadata and allocator overhead",
        trace.mesh.precisePositions.size() * sizeof(woby::Coordinate));
    ImGui::End();
}
} // namespace mesh_lab

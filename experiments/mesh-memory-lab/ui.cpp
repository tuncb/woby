#include "ui.h"
#include "utf8_path.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace mesh_lab {
namespace {
constexpr ImVec4 ink{0.88f, 0.93f, 0.96f, 1};
constexpr ImVec4 muted{0.56f, 0.65f, 0.72f, 1};
constexpr ImVec4 mint{0.36f, 0.89f, 0.71f, 1};
constexpr ImVec4 blue{0.44f, 0.70f, 0.97f, 1};
constexpr ImVec4 amber{0.96f, 0.72f, 0.38f, 1};
constexpr ImVec4 purple{0.75f, 0.61f, 0.95f, 1};
constexpr const char* components[] = {"p.x", "p.y", "p.z", "n.x", "n.y", "n.z", "uv.u", "uv.v"};
ImU32 color(ImVec4 c) { return ImGui::ColorConvertFloat4ToU32(c); }
ImVec4 fieldColor(size_t c) { return c < 3 ? blue : (c < 6 ? purple : amber); }
void label(const char* text)
{
    ImGui::PushStyleColor(ImGuiCol_Text,muted); ImGui::TextWrapped("%s",text); ImGui::PopStyleColor();
}
void heading(const char* text) { ImGui::Spacing(); ImGui::TextColored(mint, "%s", text); ImGui::Spacing(); }
void markRow(bool active)
{
    if (active) { ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(38, 85, 75, 150)); }
}
void at(ImDrawList* draw, ImVec2 pos, ImVec4 c, const char* text) { draw->AddText(pos, color(c), text); }
void clipped(ImDrawList* draw, ImVec2 pos, float width, ImVec4 c, const char* text)
{
    draw->PushClipRect(pos, {pos.x+width, pos.y+24}, true); at(draw, pos, c, text); draw->PopClipRect();
}
void arrow(ImDrawList* draw, float x0, float x1, float y, ImVec4 c)
{
    draw->AddLine({x0,y}, {x1-4,y}, color(c), 1.5f);
    draw->AddTriangleFilled({x1,y}, {x1-5,y-4}, {x1-5,y+4}, color(c));
}
void miniRecord(ImDrawList* draw, ImVec2 p, float width, size_t id, bool active)
{
    const float cell = width / 8;
    for (size_t c = 0; c < 8; ++c) {
        auto shade = fieldColor(c); shade.w = active ? .85f : .25f;
        const float x = p.x + static_cast<float>(c)*cell;
        draw->AddRectFilled({x,p.y}, {x+cell-2,p.y+13}, color(shade), 1);
    }
    char name[20]; std::snprintf(name, sizeof(name), "v%zu", id);
    at(draw, {p.x, p.y-18}, active ? ink : muted, name);
}
void dataCard(UiState& state, const Trace& trace, const GpuCapture& gpu, const PipelineNode& node,
    ImVec2 p, float width, float height)
{
    auto* draw = ImGui::GetWindowDrawList();
    const bool selected = node.id == state.node;
    const auto accent = node.id == Node::gpu ? mint : blue;
    draw->AddRectFilled(p, {p.x+width,p.y+height}, selected ? IM_COL32(30, 52, 64, 255) : IM_COL32(23, 34, 44, 255), 6);
    draw->AddRect(p, {p.x+width,p.y+height}, selected ? color(accent) : IM_COL32(52, 65, 75, 255), 6, 0, selected ? 2.0f : 1.0f);
    at(draw, {p.x+12,p.y+10}, accent, node.id == Node::gpu ? "DATA / DEVICE" : (node.id == Node::source ? "DATA / SOURCE" : "DATA / CPU"));
    at(draw, {p.x+12,p.y+34}, ink, node.title);
    char bytes[64]; std::snprintf(bytes, sizeof(bytes), "%zu B payload", payloadBytes(node.id, trace));
    at(draw, {p.x+12,p.y+60}, muted, bytes);
    const auto v = selectedVertex(state, trace);
    const auto key = trace.vertexKeys[v];
    const float x = p.x+12, y = p.y+95, inner = width-24;
    char text[96];
    switch (node.id) {
    case Node::source:
        at(draw, {x,y}, blue, "v  1000000000...");
        at(draw, {x,y+26}, amber, "vt 0.2 0");
        at(draw, {x,y+52}, purple, "vn 0 -1 0");
        clipped(draw, {x,y+78}, inner, mint, "f 4/5/2 3/6/2 ...");
        at(draw, {x,y+119}, muted, "8 polygon corners"); break;
    case Node::attributes:
        for (int row = 0; row < 3; ++row) {
            const char* names[] = {"positions / f64 x3", "normals / f32 x3", "UVs / f32 x2"};
            const size_t counts[] = {trace.positions.size(), trace.normals.size(), trace.texcoords.size()};
            const ImVec4 shades[] = {blue, purple, amber};
            clipped(draw, {x,y+static_cast<float>(row)*43}, inner, shades[row], names[row]);
            const float cell = std::min(21.0f, inner/static_cast<float>(counts[row]));
            for (size_t i = 0; i < counts[row]; ++i) {
                auto shade = shades[row];
                const int selectedId = row == 0 ? key.position : (row == 1 ? key.normal : key.texcoord);
                shade.w = static_cast<int>(i) == selectedId ? .95f : .24f;
                draw->AddRectFilled({x+static_cast<float>(i)*cell,y+22+static_cast<float>(row)*43},
                    {x+static_cast<float>(i+1)*cell-3,y+34+static_cast<float>(row)*43}, color(shade), 1);
            }
        }
        break;
    case Node::corners:
        for (size_t t = 0; t < trace.triangles.size(); ++t) {
            const auto& corners = trace.triangles[t].corners;
            std::snprintf(text, sizeof(text), "T%zu  p[%d %d %d]", t, corners[0].position, corners[1].position, corners[2].position);
            clipped(draw, {x,y+static_cast<float>(t)*27}, inner, t == state.triangle ? mint : muted, text);
        }
        at(draw, {x,y+119}, muted, "12 tuples / 4 tris"); break;
    case Node::mesh:
        for (size_t row = 0; row < 3; ++row) {
            const size_t id = row == 1 ? static_cast<size_t>(v) : (row == 0 ? 0 : trace.mesh.vertices.size()-1);
            miniRecord(draw, {x,y+18+static_cast<float>(row)*35}, inner, id, row == 1);
        }
        at(draw, {x,y+119}, muted, "8 verts / 12 IDs"); break;
    case Node::gpu:
        at(draw, {x,y}, blue, "VB / 8 x 32 B");
        miniRecord(draw, {x,y+42}, inner, static_cast<size_t>(v), true);
        at(draw, {x,y+70}, amber, "IB / 12 x 4 B");
        std::snprintf(text, sizeof(text), "[%u %u %u]", trace.mesh.indices[state.triangle*3], trace.mesh.indices[state.triangle*3+1], trace.mesh.indices[state.triangle*3+2]);
        at(draw, {x,y+94}, ink, text);
        at(draw, {x,y+119}, gpu.matches ? mint : muted, gpu.complete ? (gpu.matches ? "Readback: equal" : "Readback: ERROR") : "Readback: pending"); break;
    default: break;
    }
    ImGui::SetCursorScreenPos(p);
    if (ImGui::InvisibleButton(node.title, {width,height}, ImGuiButtonFlags_EnableNav)) { selectNode(state, node.id); }
    if (ImGui::IsItemHovered()) { ImGui::SetTooltip("%s\nClick to inspect values, identities and memory.", node.caption); }
}
void train(UiState& state, const Trace& trace, const GpuCapture& gpu)
{
    const float width = ImGui::GetContentRegionAvail().x;
    const float gap = 12, operationWidth = width < 1250 ? 76.0f : 88.0f;
    const float dataWidth = (width-8*gap-4*operationWidth)/5;
    const auto origin = ImGui::GetCursorScreenPos(); auto* draw = ImGui::GetWindowDrawList();
    float x = origin.x;
    for (size_t i = 0; i < pipeline.size(); ++i) {
        const auto& node = pipeline[i]; const float w = node.transformer ? operationWidth : dataWidth;
        ImGui::PushID(static_cast<int>(i));
        if (node.transformer) {
            const ImVec2 p{x,origin.y+70}; const bool active = state.node == node.id;
            draw->AddRectFilled(p, {x+w,p.y+112}, active ? IM_COL32(72, 55, 33, 255) : IM_COL32(42, 37, 30, 255), 12);
            draw->AddRect(p, {x+w,p.y+112}, active ? color(amber) : IM_COL32(81, 65, 44, 255), 12, 0, active ? 2.0f : 1.0f);
            const char* names[] = {"", "Parse", "", "Rebase", "", "Intern", "", "Copy", ""};
            const char* second[] = {"", "", "", "+ split", "", "+ pack", "", "to GPU", ""};
            at(draw, {x+12,p.y+14}, amber, "f(x)");
            at(draw, {x+9,p.y+49}, ink, names[i]); at(draw, {x+9,p.y+72}, muted, second[i]);
            ImGui::SetCursorScreenPos(p);
            if (ImGui::InvisibleButton(node.title, {w,112}, ImGuiButtonFlags_EnableNav)) { selectNode(state,node.id); }
            if (ImGui::IsItemHovered()) { ImGui::SetTooltip("%s\nClick for inputs, outputs and transformation details.", node.caption); }
        } else { dataCard(state,trace,gpu,node,{x,origin.y},w,252); }
        if (i+1 < pipeline.size()) { arrow(draw,x+w+2,x+w+gap-2,origin.y+126,muted); }
        x += w+gap; ImGui::PopID();
    }
    ImGui::SetCursorScreenPos({origin.x,origin.y+262}); ImGui::Dummy({width,1});
}

void sourceTable(UiState& state, const Trace& trace, bool parsing)
{
    if (ImGui::BeginTable("source", parsing ? 4 : 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable, {0,-1})) {
        ImGui::TableSetupColumn("Line", ImGuiTableColumnFlags_WidthFixed, 42);
        ImGui::TableSetupColumn("+byte", ImGuiTableColumnFlags_WidthFixed, 62);
        ImGui::TableSetupColumn("Embedded OBJ text");
        if (parsing) { ImGui::TableSetupColumn("Parsed destination", ImGuiTableColumnFlags_WidthFixed, 178); }
        ImGui::TableSetupScrollFreeze(0,1); ImGui::TableHeadersRow();
        for (size_t i = 0; i < trace.lines.size(); ++i) {
            const auto& line = trace.lines[i]; ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow(); markRow(lineRelated(trace,i,state.triangle)); ImGui::TableNextColumn();
            char name[32]; std::snprintf(name,sizeof(name),"%zu",i+1);
            if (ImGui::Selectable(name,false,ImGuiSelectableFlags_SpanAllColumns)) { selectLine(state,trace,i); }
            ImGui::TableNextColumn(); ImGui::Text("%04zx",line.byteOffset);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(line.text.c_str());
            if (parsing) {
                ImGui::TableNextColumn();
                if (line.position >= 0) { ImGui::TextColored(blue,"positions[%d] / 24 B",line.position); }
                else if (line.texcoord >= 0) { ImGui::TextColored(amber,"texcoords[%d] / 8 B",line.texcoord); }
                else if (line.normal >= 0) { ImGui::TextColored(purple,"normals[%d] / 12 B",line.normal); }
                else if (line.face >= 0) { ImGui::TextColored(mint,"faces[%d] / 4 tuples",line.face); }
                else { label("metadata / comment"); }
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}
void positionsTable(UiState& state, const Trace& trace, bool localized)
{
    if (ImGui::BeginTable("positions",localized ? 5 : 4,ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("source ID"); ImGui::TableSetupColumn("x / float64");
        ImGui::TableSetupColumn("y"); ImGui::TableSetupColumn("z");
        if (localized) { ImGui::TableSetupColumn("local x = source - origin"); }
        ImGui::TableHeadersRow();
        const auto selected = trace.vertexKeys[selectedVertex(state,trace)].position;
        for (size_t i = 0; i < trace.positions.size(); ++i) {
            const auto& p = trace.positions[i]; ImGui::PushID(static_cast<int>(i)); ImGui::TableNextRow();
            markRow(static_cast<int>(i) == selected); ImGui::TableNextColumn(); char name[32]; std::snprintf(name,sizeof(name),"p%zu",i);
            if (ImGui::Selectable(name,false,ImGuiSelectableFlags_SpanAllColumns)) { selectPosition(state,trace,i); }
            ImGui::TableNextColumn(); ImGui::Text("%.17g",p[0]); ImGui::TableNextColumn(); ImGui::Text("%.5g",p[1]);
            ImGui::TableNextColumn(); ImGui::Text("%.5g",p[2]);
            if (localized) { ImGui::TableNextColumn(); ImGui::TextColored(blue,"%.17g",trace.localPositions[i][0]); }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}
void attributePools(UiState& state, const Trace& trace)
{
    label("Independent contiguous arrays; polygon corners index all three domains.");
    heading("positions / float64[6][3] / 144 B / stride 24"); positionsTable(state,trace,false);
    heading("normals / float32[2][3] / 24 B   +   texcoords / float32[8][2] / 64 B");
    if (ImGui::BeginTable("attributes",4,ImGuiTableFlags_RowBg)) {
        for (const auto* name : {"array / index","values","+byte","used by selected corner"}) { ImGui::TableSetupColumn(name); }
        ImGui::TableHeadersRow(); const auto key = trace.vertexKeys[selectedVertex(state,trace)];
        for (size_t i = 0; i < trace.normals.size()+trace.texcoords.size(); ++i) {
            const bool normal = i < trace.normals.size(); const size_t id = normal ? i : i-trace.normals.size();
            ImGui::PushID(static_cast<int>(i)); ImGui::TableNextRow();
            const bool active = static_cast<int>(id) == (normal ? key.normal : key.texcoord); markRow(active);
            ImGui::TableNextColumn(); char name[32]; std::snprintf(name,sizeof(name),"%s[%zu]",normal ? "normals" : "texcoords",id);
            if (ImGui::Selectable(name,false,ImGuiSelectableFlags_SpanAllColumns)) {
                for (size_t v = 0; v < trace.vertexKeys.size(); ++v) {
                    const auto& k = trace.vertexKeys[v];
                    if (static_cast<int>(id) == (normal ? k.normal : k.texcoord)) { selectVertex(state,trace,v); break; }
                }
            }
            ImGui::TableNextColumn();
            if (normal) { const auto& n = trace.normals[id]; ImGui::TextColored(purple,"(%.5g, %.5g, %.5g)",n[0],n[1],n[2]); }
            else { const auto& uv = trace.texcoords[id]; ImGui::TextColored(amber,"(%.5g, %.5g)",uv[0],uv[1]); }
            ImGui::TableNextColumn(); ImGui::Text("%zu",id*(normal ? 12 : 8));
            ImGui::TableNextColumn(); label(active ? "selected" : ""); ImGui::PopID();
        }
        ImGui::EndTable();
    }
    heading("Original polygon corners / 8 x 12 B / independent p,n,uv indices");
    if (ImGui::BeginTable("polygon-corners",4,ImGuiTableFlags_RowBg)) {
        for (const auto* name : {"face","corner","p / n / uv","corner payload +byte"}) { ImGui::TableSetupColumn(name); }
        ImGui::TableHeadersRow(); size_t offset = 0;
        for (size_t f = 0; f < trace.faces.size(); ++f) {
            for (size_t c = 0; c < trace.faces[f].corners.size(); ++c, offset += sizeof(Corner)) {
                const auto key = trace.faces[f].corners[c];
                ImGui::PushID(static_cast<int>(offset)); ImGui::TableNextRow();
                markRow(key == trace.vertexKeys[selectedVertex(state,trace)]);
                ImGui::TableNextColumn(); char name[24]; std::snprintf(name,sizeof(name),"F%zu",f);
                if (ImGui::Selectable(name,false,ImGuiSelectableFlags_SpanAllColumns)) {
                    const auto found = std::find(trace.vertexKeys.begin(),trace.vertexKeys.end(),key);
                    if (found != trace.vertexKeys.end()) { selectVertex(state,trace,static_cast<size_t>(found-trace.vertexKeys.begin())); }
                }
                ImGui::TableNextColumn(); ImGui::Text("%zu",c);
                ImGui::TableNextColumn(); ImGui::Text("(%d, %d, %d)",key.position,key.normal,key.texcoord);
                ImGui::TableNextColumn(); ImGui::Text("%zu",offset); ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
}
void cornerTable(UiState& state, const Trace& trace, bool packing)
{
    if (ImGui::BeginTable("corners",packing ? 6 : 5,ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,{0,-1})) {
        for (const auto* name : {"corner","face -> triangle","p / n / uv","render vertex","+byte"}) { ImGui::TableSetupColumn(name); }
        if (packing) { ImGui::TableSetupColumn("intern result"); }
        ImGui::TableSetupScrollFreeze(0,1); ImGui::TableHeadersRow();
        for (size_t i = 0; i < trace.mesh.indices.size(); ++i) {
            const size_t t = i/3, c = i%3; const auto& tri = trace.triangles[t]; const auto& key = tri.corners[c];
            ImGui::PushID(static_cast<int>(i)); ImGui::TableNextRow(); markRow(t == state.triangle);
            ImGui::TableNextColumn(); char name[32]; std::snprintf(name,sizeof(name),"C%zu",i);
            if (ImGui::Selectable(name,i == state.triangle*3+state.corner,ImGuiSelectableFlags_SpanAllColumns)) {
                selectTriangle(state,trace,t); selectCorner(state,c);
            }
            ImGui::TableNextColumn(); ImGui::Text("F%zu -> T%zu.%zu",tri.face,t,c);
            ImGui::TableNextColumn(); ImGui::Text("(%d, %d, %d)",key.position,key.normal,key.texcoord);
            ImGui::TableNextColumn(); ImGui::TextColored(mint,"v%u",trace.mesh.indices[i]);
            ImGui::TableNextColumn(); ImGui::Text("%s+%zu",packing ? "VB" : "corners",packing ? trace.mesh.indices[i]*size_t{32} : i*sizeof(Corner));
            if (packing) { ImGui::TableNextColumn(); ImGui::TextColored(trace.vertexEvents[i].created ? amber : muted,"%s",trace.vertexEvents[i].created ? "INSERT" : "REUSE"); }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

void meshTable(UiState& state, const Trace& trace)
{
    label("woby::Vertex = { float position[3]; float normal[3]; float texcoord[2]; }");
    ImGui::Text("stride 32 / align %zu / offsets: position 0, normal 12, texcoord 24",alignof(woby::Vertex));
    ImGui::Spacing();
    if (ImGui::BeginTable("vertices",5,ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,{0,282})) {
        for (const auto* name : {"vertex / +byte","source tuple p,n,uv","position / f32","normal / f32","UV / f32"}) { ImGui::TableSetupColumn(name); }
        ImGui::TableSetupScrollFreeze(0,1); ImGui::TableHeadersRow();
        const auto selected = selectedVertex(state,trace);
        for (size_t i = 0; i < trace.mesh.vertices.size(); ++i) {
            const auto& v = trace.mesh.vertices[i]; const auto& key = trace.vertexKeys[i];
            ImGui::PushID(static_cast<int>(i)); ImGui::TableNextRow();
            markRow(key.position == trace.vertexKeys[selected].position); ImGui::TableNextColumn();
            char name[40]; std::snprintf(name,sizeof(name),"v%zu / %03zu",i,i*32);
            if (ImGui::Selectable(name,i == selected,ImGuiSelectableFlags_SpanAllColumns)) { selectVertex(state,trace,i); }
            ImGui::TableNextColumn(); ImGui::Text("(%d, %d, %d)",key.position,key.normal,key.texcoord);
            ImGui::TableNextColumn(); ImGui::TextColored(blue,"%.5g, %.5g, %.5g",v.position[0],v.position[1],v.position[2]);
            ImGui::TableNextColumn(); ImGui::TextColored(purple,"%.5g, %.5g, %.5g",v.normal[0],v.normal[1],v.normal[2]);
            ImGui::TableNextColumn(); ImGui::TextColored(amber,"%.6g, %.6g",v.texcoord[0],v.texcoord[1]); ImGui::PopID();
        }
        ImGui::EndTable();
    }
    heading("Retained CPU arrays / not uploaded");
    ImGui::Text("precisePositions   double[8][3]   192 B    local coordinates per render vertex");
    ImGui::Text("sourceData.points  double[6][3]   144 B    original position identity, local frame");
    ImGui::Text("sourceData.indices uint32[12]      48 B    corner -> source position");
    label("688 B counts these arrays plus 256 B vertices and 48 B indices. Headers and capacity are excluded.");
    heading("Render indices versus original position indices / 4 B per element");
    if (ImGui::BeginTable("cpu-indices",4,ImGuiTableFlags_RowBg)) {
        for (const auto* name : {"triangle","+byte","mesh.indices / render ID","sourceData.indices / source ID"}) { ImGui::TableSetupColumn(name); }
        ImGui::TableHeadersRow();
        for (size_t t = 0; t < trace.triangles.size(); ++t) {
            ImGui::PushID(static_cast<int>(t)); ImGui::TableNextRow(); markRow(t == state.triangle);
            ImGui::TableNextColumn(); char name[20]; std::snprintf(name,sizeof(name),"T%zu",t);
            if (ImGui::Selectable(name,t == state.triangle,ImGuiSelectableFlags_SpanAllColumns)) { selectTriangle(state,trace,t); }
            ImGui::TableNextColumn(); ImGui::Text("%zu",t*12);
            ImGui::TableNextColumn(); const auto* ids = trace.mesh.indices.data()+t*3;
            ImGui::TextColored(mint,"[%u, %u, %u]",ids[0],ids[1],ids[2]);
            ImGui::TableNextColumn(); const auto* source = trace.mesh.sourceData->indices.data()+t*3;
            ImGui::TextColored(blue,"[%u, %u, %u]",source[0],source[1],source[2]); ImGui::PopID();
        }
        ImGui::EndTable();
    }
    heading("Double sidecars / same local frame, two identity domains");
    if (ImGui::BeginTable("sidecars",4,ImGuiTableFlags_RowBg)) {
        for (const auto* name : {"render vertex / +byte","precisePositions[v]","source position / +byte","sourceData.points[p]"}) { ImGui::TableSetupColumn(name); }
        ImGui::TableHeadersRow();
        for (size_t v = 0; v < trace.mesh.vertices.size(); ++v) {
            const auto p = static_cast<size_t>(trace.vertexKeys[v].position);
            const auto& a = trace.mesh.precisePositions[v]; const auto& b = trace.mesh.sourceData->points[p];
            ImGui::PushID(static_cast<int>(v)); ImGui::TableNextRow(); markRow(v == selectedVertex(state,trace));
            ImGui::TableNextColumn(); char name[32]; std::snprintf(name,sizeof(name),"v%zu / %zu",v,v*24);
            if (ImGui::Selectable(name,false,ImGuiSelectableFlags_SpanAllColumns)) { selectVertex(state,trace,v); }
            ImGui::TableNextColumn(); ImGui::Text("(%.17g, %.17g, %.17g)",a[0],a[1],a[2]);
            ImGui::TableNextColumn(); ImGui::Text("p%zu / %zu",p,p*24);
            ImGui::TableNextColumn(); ImGui::Text("(%.17g, %.17g, %.17g)",b[0],b[1],b[2]); ImGui::PopID();
        }
        ImGui::EndTable();
    }
}
void triangleIndices(UiState& state, const Trace& trace, const GpuCapture& gpu)
{
    if (ImGui::BeginTable("indices",5,ImGuiTableFlags_RowBg)) {
        for (const auto* name : {"primitive","IB +byte","uint32[3] / vertex IDs","VB offsets / bytes","GPU readback"}) { ImGui::TableSetupColumn(name); }
        ImGui::TableHeadersRow();
        for (size_t t = 0; t < trace.triangles.size(); ++t) {
            const auto a = trace.mesh.indices[t*3], b = trace.mesh.indices[t*3+1], c = trace.mesh.indices[t*3+2];
            ImGui::PushID(static_cast<int>(t)); ImGui::TableNextRow(); markRow(t == state.triangle); ImGui::TableNextColumn();
            char name[20]; std::snprintf(name,sizeof(name),"T%zu",t);
            if (ImGui::Selectable(name,t == state.triangle,ImGuiSelectableFlags_SpanAllColumns)) { selectTriangle(state,trace,t); }
            ImGui::TableNextColumn(); ImGui::Text("%zu",t*12);
            ImGui::TableNextColumn(); ImGui::TextColored(mint,"[%u, %u, %u]",a,b,c);
            ImGui::TableNextColumn(); ImGui::Text("[%u, %u, %u]",a*32,b*32,c*32);
            ImGui::TableNextColumn(); ImGui::TextColored(gpu.matches ? mint : muted,"%s",gpu.complete ? (gpu.matches ? "equal" : "MISMATCH") : "pending");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}
void allocation(const char* name, const char* type, size_t count, size_t stride, const char* ownership)
{
    ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextUnformatted(name);
    ImGui::TableNextColumn(); ImGui::TextUnformatted(type);
    ImGui::TableNextColumn(); ImGui::Text("%zu x %zu = %zu B",count,stride,count*stride);
    ImGui::TableNextColumn(); ImGui::TextUnformatted(ownership);
}
void uploadPanel(UiState& state, const Trace& trace, const GpuCapture& gpu)
{
    ImGui::TextColored(amber,"Copy 304 B of render data; retain all 688 B of CPU arrays.");
    label("Upload copies bytes. It does not move the vectors or upload their pointers, size or capacity.");
    heading("Ownership across the transfer");
    if (ImGui::BeginTable("ownership",4,ImGuiTableFlags_RowBg)) {
        for (const auto* name : {"allocation","element type","payload","lifetime"}) { ImGui::TableSetupColumn(name); }
        ImGui::TableHeadersRow();
        allocation("mesh.vertices","Vertex",8,32,"CPU capture");
        allocation("mesh.indices","uint32",12,4,"CPU capture");
        allocation("graphics::copy (VB)","byte",256,1,"renderer-owned upload");
        allocation("graphics::copy (IB)","byte",48,1,"renderer-owned upload");
        allocation("device VB","Vertex",8,32,"GPU resource");
        allocation("device IB","uint32",12,4,"GPU resource");
        ImGui::EndTable();
    }
    heading("Executed calls / completion");
    ImGui::TextUnformatted("createVertexBuffer(copy(vertices.data(), 256), {32}, COMPUTE_READ)\n"
        "createIndexBuffer(copy(indices.data(), 48), INDEX32)\n"
        "readBuffer(VB, ownedReadback); readBuffer(IB, ownedReadback);");
    ImGui::TextColored(gpu.matches ? mint : amber,"%s",gpu.complete ? (gpu.matches ? "Completion reached: all 304 returned bytes equal the upload." : "Readback mismatch.") : "Readback pending: destinations remain owned until completion.");
    ImGui::Spacing(); triangleIndices(state,trace,gpu);
}
void gpuPanel(UiState& state, const Trace& trace, const GpuCapture& gpu)
{
    ImGui::Text("VB handle %u / stride 32 B    IB handle %u / uint32",gpu.vertices.idx,gpu.indices.idx);
    label("Handles are opaque. Offsets are relative to each resource, not physical device addresses.");
    heading("Index fetch -> vertex fetch -> triangle assembly");
    triangleIndices(state,trace,gpu);
    const auto vertex = selectedVertex(state,trace); const auto index = state.triangle*3+state.corner;
    heading("Selected corner / exact shader addressing");
    ImGui::Text("T%zu.corner%zu:  indices[%zu] at IB+%zu = %u",state.triangle,state.corner,index,index*4,vertex);
    ImGui::Text("SV_VertexID = %u; root.stride = 8 float32 words = 32 bytes",vertex);
    ImGui::TextUnformatted("uint p = SV_VertexID * root.stride;\n"
        "position = float3(vertices[p+0], vertices[p+1], vertices[p+2]);\n"
        "normal   = float3(vertices[p+3], vertices[p+4], vertices[p+5]);\n"
        "uv       = float2(vertices[p+6], vertices[p+7]);");
    ImGui::Spacing();
    label("The viewport draws these buffers through the production vs_mesh / fs_mesh shaders.");
    label("The index stream is shown; no assumption is made about physical shader invocation order or cache hits.");
}
void splitPanel(UiState& state, const Trace& trace)
{
    ImGui::TextColored(amber,"1. Localize doubles before triangulation and float conversion");
    ImGui::Text("origin = (%.17g, %.5g, %.5g)",trace.mesh.origin[0],trace.mesh.origin[1],trace.mesh.origin[2]);
    label("Per axis: rebase to the bounds center when |center| >= 65536. local = original - origin.");
    positionsTable(state,trace,true);
    heading("2. RapidOBJ polygon triangulation / same winding, independent corner attributes");
    if (ImGui::BeginTable("faces",3,ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("original face / source corners"); ImGui::TableSetupColumn("output triangle"); ImGui::TableSetupColumn("position IDs");
        ImGui::TableHeadersRow();
        for (size_t t = 0; t < trace.triangles.size(); ++t) {
            const auto& triangle = trace.triangles[t]; const auto& f = trace.faces[triangle.face];
            ImGui::PushID(static_cast<int>(t)); ImGui::TableNextRow(); markRow(t == state.triangle);
            ImGui::TableNextColumn(); ImGui::Text("F%zu = [%d %d %d %d]",triangle.face,f.corners[0].position,f.corners[1].position,f.corners[2].position,f.corners[3].position);
            ImGui::TableNextColumn(); char title[20]; std::snprintf(title,sizeof(title),"T%zu",t);
            if (ImGui::Selectable(title,t == state.triangle,ImGuiSelectableFlags_SpanAllColumns)) { selectTriangle(state,trace,t); }
            ImGui::TableNextColumn(); ImGui::TextColored(mint,"[%d %d %d]",triangle.corners[0].position,triangle.corners[1].position,triangle.corners[2].position);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    label("Quad rule: choose diagonal 0-2 only when it is shorter than 1-3; ties use 1-3.");
    label("Positions are not duplicated here. The corner stream grows from 8 to 12 tuples (+48 B).");
}
void packPanel(UiState& state, const Trace& trace)
{
    ImGui::TextColored(amber,"12 corners -> 8 unique (position, normal, texcoord) tuples -> 8 vertices + 12 indices");
    label("Source p2 and p3 each have two render identities because their normal and UV indices differ.");
    ImGui::TextUnformatted("id = vertexIndex({corner.position, corner.normal, corner.texcoord});\n"
        "if (firstUse) emit Vertex{float(localPosition), authoredNormal, {u, 1-v}};\n"
        "indices.push_back(id); // first-use order; no index-buffer reordering");
    ImGui::Spacing();
    label("INSERT allocates a 32 B record; REUSE emits only a 4 B index. Double positions stay in CPU sidecars.");
    ImGui::Spacing(); cornerTable(state,trace,true);
}
void details(UiState& state, const Trace& trace, const GpuCapture& gpu)
{
    const auto& node = pipeline[static_cast<size_t>(state.node)];
    ImGui::TextColored(node.transformer ? amber : blue,"%s",node.transformer ? "TRANSFORMER" : "DATA");
    ImGui::SameLine(); ImGui::TextUnformatted(node.title); ImGui::SameLine(); label(node.caption);
    if (node.transformer) {
        const auto ends = transformationEndpoints(node.id);
        ImGui::TextColored(muted,"%s (%zu B) -> %s (%zu B)",pipeline[static_cast<size_t>(ends[0])].title,payloadBytes(ends[0],trace),
            pipeline[static_cast<size_t>(ends[1])].title,payloadBytes(ends[1],trace));
    } else { ImGui::TextColored(muted,"%zu B of array payload / click a record to follow its identity",payloadBytes(node.id,trace)); }
    ImGui::Separator(); ImGui::Spacing();
    switch (node.id) {
    case Node::source:
        label("Embedded ASCII OBJ. Line numbers and byte offsets refer to this exact string.");
        sourceTable(state,trace,false); break;
    case Node::parse:
        ImGui::TextUnformatted("decimal text -> float64 positions / float32 UVs and normals\n"
            "OBJ 1-based indices -> independent 0-based p, n, uv indices\n"
            "f records -> polygon corner tuples; absent attributes use -1");
        ImGui::Spacing(); sourceTable(state,trace,true); break;
    case Node::attributes: attributePools(state,trace); break;
    case Node::triangulate: splitPanel(state,trace); break;
    case Node::corners:
        ImGui::TextUnformatted("Corner = {int32 position, int32 texcoord, int32 normal}; sizeof = 12 B");
        label("Tuple notation below is p / n / uv. Each consecutive triple is one triangle.");
        label("232 B of localized attribute arrays + 144 B of corner records. Polygon provenance is retained separately.");
        ImGui::Spacing(); cornerTable(state,trace,false); break;
    case Node::pack: packPanel(state,trace); break;
    case Node::mesh: meshTable(state,trace); break;
    case Node::upload: uploadPanel(state,trace,gpu); break;
    case Node::gpu: gpuPanel(state,trace,gpu); break;
    case Node::count: break;
    }
}

void geometry(UiState& state, const Trace& trace, Viewport& view)
{
    const ImVec2 canvas{ImGui::GetContentRegionAvail().x, 132};
    resizeViewport(view, static_cast<uint16_t>(std::clamp(canvas.x, 1.0f, 2048.0f)), 132);
    const auto start = ImGui::GetCursorScreenPos();
    ImGui::Image(static_cast<ImTextureID>(view.color.idx)+1, canvas);
    if (ImGui::IsItemHovered()) {
        const auto& io = ImGui::GetIO();
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) { orbit(state,-io.MouseDelta.x*.009f,io.MouseDelta.y*.009f,1); }
        if (io.MouseWheel != 0) { orbit(state,0,0,std::pow(1.12f,io.MouseWheel)); }
    }
    const auto matrices = viewMatrices(trace,state,canvas.x/canvas.y,woby::graphics::getCaps()->homogeneousDepth);
    auto* draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(start,{start.x+canvas.x,start.y+canvas.y},true);
    std::array<ImVec2,3> points;
    for (size_t c = 0; c < 3; ++c) {
        const auto projected = projectVertex(matrices,trace.mesh.vertices[trace.mesh.indices[state.triangle*3+c]]);
        points[c] = {start.x+projected[0]*canvas.x,start.y+projected[1]*canvas.y};
    }
    draw->AddTriangle(points[0],points[1],points[2],color(mint),1.5f);
    const ImVec2 center{(points[0].x+points[1].x+points[2].x)/3,(points[0].y+points[1].y+points[2].y)/3};
    for (size_t c = 0; c < 3; ++c) {
        const auto vertex = trace.mesh.indices[state.triangle*3+c];
        draw->AddCircleFilled(points[c],c == state.corner ? 5.0f : 3.0f,color(c == state.corner ? amber : mint));
        char name[32]; std::snprintf(name,sizeof(name),"c%zu / v%u",c,vertex);
        const auto size = ImGui::CalcTextSize(name);
        const float x = points[c].x < center.x ? points[c].x-size.x-9 : points[c].x+9;
        const float y = points[c].y < center.y ? points[c].y-size.y-6 : points[c].y+6;
        at(draw,{std::clamp(x,start.x+3,start.x+canvas.x-size.x-3),std::clamp(y,start.y+3,start.y+canvas.y-size.y-3)},
            c == state.corner ? amber : ink,name);
    }
    at(draw,{start.x+8,start.y+8},muted,"indexed draw / drag to orbit");
    draw->PopClipRect();
}
void byteFields(UiState& state, const Trace& trace, const GpuCapture& gpu)
{
    const auto v = selectedVertex(state,trace);
    const auto start = vertexOffset(v,0);
    const float cell = (ImGui::GetContentRegionAvail().x-18)/4;
    ImGui::Text("Vertex v%u / VB+%zu / 32 B",v,start);
    for (size_t c = 0; c < 8; ++c) {
        if (c%4) { ImGui::SameLine(0,6); }
        ImGui::PushID(static_cast<int>(c));
        const auto p = ImGui::GetCursorScreenPos(); auto* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(p,{p.x+cell,p.y+48},state.component == c ? IM_COL32(38,64,74,255) : IM_COL32(24,35,44,255),3);
        draw->AddRectFilled(p,{p.x+cell,p.y+2},color(fieldColor(c)));
        float value = 0; std::memcpy(&value,gpu.vertexUpload.data()+start+c*4,4);
        char name[32], text[32]; std::snprintf(name,sizeof(name),"%s +%02zu",components[c],c*4);
        std::snprintf(text,sizeof(text),"%.7g",value);
        at(draw,{p.x+6,p.y+7},fieldColor(c),name); at(draw,{p.x+6,p.y+27},ink,text);
        if (ImGui::InvisibleButton(components[c],{cell,48},ImGuiButtonFlags_EnableNav)) { selectComponent(state,c); }
        ImGui::PopID();
    }
    const size_t byte = start+state.component*4;
    uint32_t bits = 0; std::memcpy(&bits,gpu.vertexUpload.data()+byte,4);
    const float value = std::bit_cast<float>(bits); const auto* b = gpu.vertexUpload.data()+byte;
    ImGui::TextColored(fieldColor(state.component),"%s / float32 %.9g / 0x%08X",components[state.component],value,bits);
    ImGui::Text("CPU bytes: %02X %02X %02X %02X  (%s)",b[0],b[1],b[2],b[3],
        std::endian::native == std::endian::little ? "little endian" : "big endian");
    if (gpu.complete) {
        const auto* device = gpu.vertexReadback.data()+byte;
        ImGui::TextColored(gpu.matches ? mint : amber,"GPU bytes: %02X %02X %02X %02X  @ VB+%zu",device[0],device[1],device[2],device[3],byte);
    } else { label("GPU readback pending"); }
    ImGui::Text("IEEE 754: sign %u / exp %u / frac %06X",bits>>31,(bits>>23)&255u,bits&0x7fffffu);
    const auto key = trace.vertexKeys[v];
    if (state.component < 3) {
        const size_t axis = state.component;
        const double original = trace.positions[static_cast<size_t>(key.position)][axis];
        const double local = trace.localPositions[static_cast<size_t>(key.position)][axis];
        ImGui::Text("source f64  %.17g",original);
        ImGui::Text("- origin    %.17g",trace.mesh.origin[axis]);
        ImGui::Text("= local f64 %.17g -> f32 %.9g",local,value);
        ImGui::TextColored(muted,"cast error %g / direct cast %.9g",static_cast<double>(value)-local,static_cast<float>(original));
    } else if (state.component < 6) {
        ImGui::Text("normal[%d].%c = %.9g (authored f32)",key.normal,"xyz"[state.component-3],value);
        label("Copied unchanged; this example needs no regeneration.");
    } else {
        const auto uv = trace.texcoords[static_cast<size_t>(key.texcoord)];
        ImGui::Text("uv[%d] = (%.9g, %.9g)",key.texcoord,uv[0],uv[1]);
        label("u = source.u; v = 1 - source.v");
    }
}
void focus(UiState& state, const Trace& trace, const GpuCapture& gpu, Viewport& view)
{
    ImGui::TextColored(mint,"FOLLOW A DATUM"); ImGui::SameLine(); label("selection links every stage");
    geometry(state,trace,view);
    int triangle = static_cast<int>(state.triangle); ImGui::SetNextItemWidth(115);
    if (ImGui::SliderInt("T",&triangle,0,static_cast<int>(trace.triangles.size())-1)) { selectTriangle(state,trace,static_cast<size_t>(triangle)); }
    for (size_t c = 0; c < 3; ++c) {
        ImGui::SameLine(); char name[12]; std::snprintf(name,sizeof(name),"c%zu",c);
        if (ImGui::RadioButton(name,state.corner == c)) { selectCorner(state,c); }
    }
    const auto v = selectedVertex(state,trace); const auto key = trace.vertexKeys[v];
    const auto face = trace.triangles[state.triangle].face;
    ImGui::Text("F%zu -> T%zu.c%zu -> (p%d,n%d,uv%d) -> v%u",face,state.triangle,state.corner,key.position,key.normal,key.texcoord,v);
    label("Same source position:");
    for (const auto alias : trace.positionVertices[static_cast<size_t>(key.position)]) {
        ImGui::SameLine(); char name[20]; std::snprintf(name,sizeof(name),"v%u",alias);
        if (ImGui::SmallButton(name)) { selectVertex(state,trace,alias); }
    }
    ImGui::Separator(); byteFields(state,trace,gpu);
}

} // namespace

void configureStyle(UiRuntime& runtime, const std::filesystem::path& assets)
{
    auto& io = ImGui::GetIO(); io.IniFilename = nullptr; io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    const auto regularPath = woby::pathToUtf8(assets / "fonts/Lato-Regular.ttf");
    const auto monoPath = woby::pathToUtf8(assets / "fonts/RobotoMonoNerdFont-Regular.ttf");
    runtime.regular = io.Fonts->AddFontFromFileTTF(regularPath.c_str(),17);
    runtime.title = io.Fonts->AddFontFromFileTTF(regularPath.c_str(),28);
    runtime.mono = io.Fonts->AddFontFromFileTTF(monoPath.c_str(),14);
    ImGui::StyleColorsDark(); auto& s = ImGui::GetStyle();
    s.WindowPadding = {18,14}; s.FramePadding = {8,5}; s.ItemSpacing = {10,7}; s.CellPadding = {7,6};
    s.WindowRounding = 0; s.ChildRounding = 6; s.FrameRounding = 3; s.PopupRounding = 5;
    s.WindowBorderSize = 0; s.ChildBorderSize = 1; s.ScrollbarSize = 12;
    s.Colors[ImGuiCol_Text] = ink; s.Colors[ImGuiCol_TextDisabled] = muted;
    s.Colors[ImGuiCol_WindowBg] = {0.055f,0.079f,0.10f,1};
    s.Colors[ImGuiCol_ChildBg] = {0.073f,0.10f,0.125f,1};
    s.Colors[ImGuiCol_Border] = {0.16f,0.21f,0.25f,1};
    s.Colors[ImGuiCol_Button] = {0.13f,0.19f,0.23f,1};
    s.Colors[ImGuiCol_ButtonHovered] = {0.19f,0.29f,0.33f,1};
    s.Colors[ImGuiCol_ButtonActive] = {0.23f,0.39f,0.37f,1};
    s.Colors[ImGuiCol_Header] = {0.14f,0.28f,0.27f,1};
    s.Colors[ImGuiCol_HeaderHovered] = {0.18f,0.33f,0.32f,1};
    s.Colors[ImGuiCol_HeaderActive] = {0.20f,0.37f,0.34f,1};
    s.Colors[ImGuiCol_TableHeaderBg] = {0.10f,0.15f,0.18f,1};
}
void drawUi(UiRuntime& runtime, UiState& state, const Trace& trace, const GpuCapture& gpu, Viewport& view)
{
    const auto& io = ImGui::GetIO();
    ImGui::SetNextWindowPos({0,0}); ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("Mesh memory lab",nullptr,ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PushFont(runtime.title); ImGui::TextUnformatted("Mesh memory lab"); ImGui::PopFont();
    ImGui::SameLine(); ImGui::TextColored(muted,"  DATA SHAPES + TRANSFORMATIONS");
    ImGui::TextColored(muted,"Built-in folded sheet    /    2 quads    /    6 source positions    /    UV + normal seam    /    coordinates near 1e9");
    ImGui::Spacing();
    ImGui::PushFont(runtime.mono);
    ImGui::BeginChild("train",{0,310},ImGuiChildFlags_Borders,ImGuiWindowFlags_NoScrollbar);
    ImGui::TextColored(blue,"DATA"); ImGui::SameLine(); ImGui::TextColored(amber,"  f(x) TRANSFORMER");
    ImGui::SameLine(); label("  Click any block to inspect its records or operation");
    train(state,trace,gpu); ImGui::EndChild();
    const float height = ImGui::GetContentRegionAvail().y-24;
    const float leftWidth = ImGui::GetContentRegionAvail().x*.66f;
    ImGui::BeginChild("inspector",{leftWidth,height},ImGuiChildFlags_Borders,ImGuiWindowFlags_HorizontalScrollbar);
    details(state,trace,gpu); ImGui::EndChild(); ImGui::SameLine();
    ImGui::BeginChild("selected-datum",{0,height},ImGuiChildFlags_Borders,ImGuiWindowFlags_HorizontalScrollbar);
    focus(state,trace,gpu,view); ImGui::EndChild();
    ImGui::PopFont();
    label("Production loader + shader buffers  /  Array payload excludes container headers and spare capacity  /  Source is compiled into the app");
    ImGui::End();
}
} // namespace mesh_lab

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
void workflowDiagram(UiState& state, const Workflow& workflow, const Trace* trace)
{
    const auto origin = ImGui::GetCursorScreenPos();
    auto* draw = ImGui::GetWindowDrawList();
    const auto point = [&](const WorkflowNode& node) {
        return ImVec2{origin.x + node.position[0], origin.y + node.position[1]};
    };
    for (const auto& edge : workflow.edges) {
        const auto a = point(workflow.nodes[edge.from]), b = point(workflow.nodes[edge.to]);
        ImVec2 from{a.x + workflowCardWidth*.5f, a.y + workflowCardHeight*.5f};
        ImVec2 to{b.x + workflowCardWidth*.5f, b.y + workflowCardHeight*.5f};
        const float dx = to.x-from.x, dy = to.y-from.y;
        if (std::abs(dx)/workflowCardWidth >= std::abs(dy)/workflowCardHeight) {
            from.x += std::copysign(workflowCardWidth*.5f, dx);
            to.x -= std::copysign(workflowCardWidth*.5f, dx);
        } else {
            from.y += std::copysign(workflowCardHeight*.5f, dy);
            to.y -= std::copysign(workflowCardHeight*.5f, dy);
        }
        const bool selected = state.workflowNode == edge.from || state.workflowNode == edge.to;
        const auto shade = selected ? mint : muted;
        const float length = std::hypot(to.x-from.x, to.y-from.y);
        if (length > 1) {
            const ImVec2 unit{(to.x-from.x)/length, (to.y-from.y)/length};
            draw->AddLine(from, to, color(shade), selected ? 2.0f : 1.0f);
            draw->AddTriangleFilled(to, {to.x-unit.x*8-unit.y*4,to.y-unit.y*8+unit.x*4},
                {to.x-unit.x*8+unit.y*4,to.y-unit.y*8-unit.x*4}, color(shade));
        }
        if (!edge.label.empty()) {
            const auto size = ImGui::CalcTextSize(edge.label.c_str());
            const ImVec2 p{(from.x+to.x-size.x)*.5f,(from.y+to.y)*.5f-18};
            draw->AddRectFilled({p.x-3,p.y-2},{p.x+size.x+3,p.y+size.y+2},IM_COL32(19,26,32,255));
            at(draw,p,shade,edge.label.c_str());
        }
    }
    for (size_t i = 0; i < workflow.nodes.size(); ++i) {
        const auto& node = workflow.nodes[i]; const auto p = point(node);
        const auto accent = node.transformer ? amber : blue;
        const bool selected = state.workflowNode == i;
        draw->AddRectFilled(p,{p.x+workflowCardWidth,p.y+workflowCardHeight},
            selected ? IM_COL32(30,52,64,255) : IM_COL32(23,34,44,255),6);
        draw->AddRect(p,{p.x+workflowCardWidth,p.y+workflowCardHeight},
            selected ? color(accent) : IM_COL32(52,65,75,255),6,0,selected ? 2.0f : 1.0f);
        const ImVec4 clip{p.x+8,p.y+4,p.x+workflowCardWidth-8,p.y+workflowCardHeight-4};
        const auto text = [&](float y, ImVec4 shade, const char* value, float wrap = 0) {
            draw->AddText(nullptr,0,{p.x+12,p.y+y},color(shade),value,nullptr,wrap,&clip);
        };
        text(10,accent,node.transformer ? "f(x) TRANSFORM" : "DATA");
        text(32,ink,node.title.c_str());
        text(55,muted,node.summary.c_str(),workflowCardWidth-24);
        if (trace && node.inspector && !node.transformer) {
            char bytes[48]; std::snprintf(bytes,sizeof(bytes),"%zu B payload",payloadBytes(*node.inspector,*trace));
            text(86,mint,bytes);
        }
        ImGui::PushID(static_cast<int>(i)); ImGui::SetCursorScreenPos(p);
        if (ImGui::InvisibleButton("node",{workflowCardWidth,workflowCardHeight},ImGuiButtonFlags_EnableNav)) {
            selectWorkflowNode(state,workflow,i);
        }
        if (ImGui::IsItemHovered()) { ImGui::SetTooltip("%s\n%s",node.title.c_str(),node.summary.c_str()); }
        ImGui::PopID();
    }
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy({workflow.extent[0]+4,workflow.extent[1]+4});
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
                else if (line.face >= 0) { ImGui::TextColored(mint,"faces[%d] / %zu tuples",line.face,trace.faces[static_cast<size_t>(line.face)].corners.size()); }
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
    heading("Positions / float64 x3 / stride 24");
    ImGui::Text("%zu positions / %zu B",trace.positions.size(),trace.positions.size()*sizeof(woby::Coordinate));
    positionsTable(state,trace,false);
    heading("Normals / float32 x3 + texcoords / float32 x2");
    ImGui::Text("%zu normals / %zu B    %zu UVs / %zu B",trace.normals.size(),trace.normals.size()*12,trace.texcoords.size(),trace.texcoords.size()*8);
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
    heading("Original polygon corners / independent p,n,uv indices");
    ImGui::Text("%zu corners x %zu B",trace.originalCorners,sizeof(Corner));
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
    ImGui::Text("precisePositions   %zu x 24 B    local coordinates per render vertex",trace.mesh.precisePositions.size());
    ImGui::Text("sourceData.points  %zu x 24 B    original position identity, local frame",trace.mesh.sourceData->points.size());
    ImGui::Text("sourceData.indices %zu x 4 B     corner -> source position",trace.mesh.sourceData->indices.size());
    ImGui::Text("%zu B total CPU payload; headers and capacity are excluded.",payloadBytes(Node::mesh,trace));
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
    ImGui::TextColored(amber,"Copy %zu B of render data; retain %zu B of CPU arrays.",payloadBytes(Node::gpu,trace),payloadBytes(Node::mesh,trace));
    label("Upload copies bytes. It does not move the vectors or upload their pointers, size or capacity.");
    heading("Ownership across the transfer");
    if (ImGui::BeginTable("ownership",4,ImGuiTableFlags_RowBg)) {
        for (const auto* name : {"allocation","element type","payload","lifetime"}) { ImGui::TableSetupColumn(name); }
        ImGui::TableHeadersRow();
        allocation("mesh.vertices","Vertex",trace.mesh.vertices.size(),32,"CPU capture");
        allocation("mesh.indices","uint32",trace.mesh.indices.size(),4,"CPU capture");
        allocation("graphics::copy (VB)","byte",gpu.vertexUpload.size(),1,"renderer-owned upload");
        allocation("graphics::copy (IB)","byte",gpu.indexUpload.size(),1,"renderer-owned upload");
        allocation("device VB","Vertex",trace.mesh.vertices.size(),32,"GPU resource");
        allocation("device IB","uint32",trace.mesh.indices.size(),4,"GPU resource");
        ImGui::EndTable();
    }
    heading("Executed calls / completion");
    ImGui::TextUnformatted("createVertexBuffer(copy(vertices.data(), vertexBytes), {32}, COMPUTE_READ)\n"
        "createIndexBuffer(copy(indices.data(), indexBytes), INDEX32)\n"
        "readBuffer(VB, ownedReadback); readBuffer(IB, ownedReadback);");
    ImGui::TextColored(gpu.matches ? mint : amber,"%s",gpu.complete ? (gpu.matches ? "Completion reached: all returned bytes equal the upload." : "Readback mismatch.") : "Readback pending: destinations remain owned until completion.");
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
            ImGui::TableNextColumn(); ImGui::Text("F%zu / %zu corners",triangle.face,f.corners.size());
            ImGui::TableNextColumn(); char title[20]; std::snprintf(title,sizeof(title),"T%zu",t);
            if (ImGui::Selectable(title,t == state.triangle,ImGuiSelectableFlags_SpanAllColumns)) { selectTriangle(state,trace,t); }
            ImGui::TableNextColumn(); ImGui::TextColored(mint,"[%d %d %d]",triangle.corners[0].position,triangle.corners[1].position,triangle.corners[2].position);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    label("Quad rule: choose diagonal 0-2 only when it is shorter than 1-3; ties use 1-3.");
    ImGui::Text("Positions stay shared: %zu polygon corners -> %zu triangle corners.",trace.originalCorners,trace.mesh.indices.size());
}
void packPanel(UiState& state, const Trace& trace)
{
    ImGui::TextColored(amber,"%zu corners -> %zu vertices + %zu indices",trace.mesh.indices.size(),trace.mesh.vertices.size(),trace.mesh.indices.size());
    ImGui::Text("%zu source positions have multiple render identities.",trace.splitPositions);
    ImGui::TextUnformatted("id = vertexIndex({corner.position, corner.normal, corner.texcoord});\n"
        "if (firstUse) emit Vertex{float(localPosition), resolvedNormal, resolvedUV};\n"
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
        ImGui::Text("%zu B of attributes and triangle corners. Polygon provenance is separate.",payloadBytes(Node::corners,trace));
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
        const bool authored = key.normal >= 0 && woby::validNormal(trace.normals[static_cast<size_t>(key.normal)]);
        ImGui::Text("normal.%c = %.9g (%s)","xyz"[state.component-3],value,authored ? "authored f32" : "generated");
        label(authored ? "Copied from the source normal." : "Generated by the production mesh loader.");
    } else {
        if (key.texcoord >= 0) {
            const auto uv = trace.texcoords[static_cast<size_t>(key.texcoord)];
            ImGui::Text("uv[%d] = (%.9g, %.9g)",key.texcoord,uv[0],uv[1]);
            label("u = source.u; v = 1 - source.v");
        } else { label("No source UV; the packed record contains the loader fallback."); }
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
void drawUi(UiRuntime& runtime, UiState& state, const WorkflowLibrary& library,
    const GpuCapture& gpu, Viewport& view, UiActions& actions)
{
    const auto& io = ImGui::GetIO();
    ImGui::SetNextWindowPos({0,0}); ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("Mesh memory lab",nullptr,ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PushFont(runtime.title); ImGui::TextUnformatted("Mesh memory lab"); ImGui::PopFont();
    ImGui::SameLine(); ImGui::TextColored(muted,"  WORKFLOW LIBRARY");
    ImGui::Spacing();
    ImGui::BeginChild("library",{212,0},ImGuiChildFlags_Borders);
    heading("WORKFLOWS");
    ImGui::BeginDisabled(runtime.loading);
    if (ImGui::Button("Reload folder")) { actions.reload = true; }
    ImGui::EndDisabled();
    if (runtime.loading) { label("Loading workflows..."); }
    ImGui::Spacing();
    for (size_t i = 0; i < library.entries.size(); ++i) {
        const auto& entry = library.entries[i];
        const auto name = entry.document ? entry.document->title : woby::pathToUtf8(entry.path.filename());
        ImGui::PushID(static_cast<int>(i));
        ImGui::BeginDisabled(runtime.loading || !entry.document);
        if (ImGui::Selectable(name.c_str(),i == state.workflow && entry.document.has_value(),0,{0,24})) { actions.workflow = i; }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("%s\n%s",woby::pathToUtf8(entry.path.filename()).c_str(),
                entry.document ? entry.document->description.c_str() : entry.error.c_str());
        }
        if (!entry.error.empty()) { ImGui::TextColored(amber,"Could not load"); }
        else { label(entry.trace ? "Live mesh + diagram" : "Diagram"); }
        ImGui::Spacing(); ImGui::PopID();
    }
    if (library.entries.empty()) { label("No .meshflow files in this folder. Add a workflow and reload."); }
    ImGui::Separator(); label("Folder");
    ImGui::TextWrapped("%s",woby::pathToUtf8(library.directory).c_str());
    ImGui::EndChild(); ImGui::SameLine();
    ImGui::BeginChild("workspace",{0,0});
    if (!runtime.message.empty()) { ImGui::TextWrapped("%s",runtime.message.c_str()); }
    if (state.workflow < library.entries.size() && library.entries[state.workflow].document) {
        const auto& entry = library.entries[state.workflow]; const auto& workflow = *entry.document;
        ImGui::TextColored(mint,"%s",workflow.title.c_str());
        ImGui::SameLine(); ImGui::BeginDisabled(runtime.loading);
        if (ImGui::SmallButton("Save copy")) { actions.saveCopy = true; }
        ImGui::EndDisabled();
        label(workflow.description.c_str());
        ImGui::PushFont(runtime.mono);
        ImGui::BeginChild("diagram",{0,330},ImGuiChildFlags_Borders,ImGuiWindowFlags_HorizontalScrollbar);
        workflowDiagram(state,workflow,entry.trace.get());
        ImGui::EndChild();
        const float height = ImGui::GetContentRegionAvail().y;
        const float leftWidth = entry.trace ? ImGui::GetContentRegionAvail().x*.63f : ImGui::GetContentRegionAvail().x*.66f;
        const auto& node = workflow.nodes[state.workflowNode];
        ImGui::BeginChild("inspector",{leftWidth,height},ImGuiChildFlags_Borders,ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::TextColored(node.transformer ? amber : blue,"%s",node.title.c_str());
        label(node.description.c_str()); ImGui::Spacing();
        if (entry.trace && node.inspector) { details(state,*entry.trace,gpu); }
        else {
            ImGui::Separator(); heading("CONNECTIONS");
            for (const auto& edge : workflow.edges) {
                if (edge.from != state.workflowNode && edge.to != state.workflowNode) { continue; }
                const size_t target = edge.from == state.workflowNode ? edge.to : edge.from;
                const auto text = std::string(edge.from == state.workflowNode ? "To: " : "From: ") + workflow.nodes[target].title;
                ImGui::PushID(static_cast<int>(&edge-workflow.edges.data()));
                if (ImGui::Selectable(text.c_str())) { selectWorkflowNode(state,workflow,target); }
                if (!edge.label.empty()) { label(edge.label.c_str()); }
                ImGui::PopID();
            }
        }
        ImGui::EndChild(); ImGui::SameLine();
        ImGui::BeginChild("selected-datum",{0,height},ImGuiChildFlags_Borders,ImGuiWindowFlags_HorizontalScrollbar);
        if (entry.trace) { focus(state,*entry.trace,gpu,view); }
        else {
            heading("WORKFLOW OUTLINE");
            label("Select a stage here or in the diagram.");
            for (size_t i = 0; i < workflow.nodes.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                if (ImGui::Selectable(workflow.nodes[i].title.c_str(),state.workflowNode == i)) { selectWorkflowNode(state,workflow,i); }
                ImGui::PopID();
            }
            ImGui::Spacing(); label("This workflow describes a process. It has no embedded mesh to inspect.");
        }
        ImGui::EndChild(); ImGui::PopFont();
    } else { label("Select a valid workflow from the library. File errors are shown beside their entries."); }
    ImGui::EndChild(); ImGui::End();
}
} // namespace mesh_lab

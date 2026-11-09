#include "tetrahedral_quality_view.h"
#include "comparison_legend.h"
#include "ui_operations.h"
#include "ui_icon_controls.h"
#include "comparison_report.h"
#include <imgui.h>
#include <algorithm>
#include <climits>
#include <cfloat>

namespace woby {
void drawTetrahedralQualityControls(TetrahedralQualitySettings& s) {
    std::array<const char*,tetrahedralQualityMetricCount> labels{};
    for (size_t i = 0; i < labels.size(); ++i) { labels[i] = tetrahedralQualityMetricName(static_cast<TetrahedralQualityMetric>(i)); }
    int metric = static_cast<int>(s.metric);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::Combo("##tetrahedral_metric",&metric,labels.data(),static_cast<int>(labels.size()))) { s.metric = static_cast<TetrahedralQualityMetric>(metric); }
    ImGui::TextWrapped("Shape and angles use the four corners. Validity and signed volume use all nodes.");
    drawVisibilityField("Selected cell only",s.selectedOnly);
    ImGui::Checkbox("Minimum##tetrahedral",&s.minimumEnabled); ImGui::SameLine();
    ImGui::BeginDisabled(!s.minimumEnabled); ImGui::SetNextItemWidth(-1); ImGui::InputFloat("##tetrahedral_minimum",&s.minimum,0,0,"%.5g"); ImGui::EndDisabled();
    ImGui::Checkbox("Maximum##tetrahedral",&s.maximumEnabled); ImGui::SameLine();
    ImGui::BeginDisabled(!s.maximumEnabled); ImGui::SetNextItemWidth(-1); ImGui::InputFloat("##tetrahedral_maximum",&s.maximum,0,0,"%.5g"); ImGui::EndDisabled();
}
void drawTetrahedralQualityResults(UiState& state, const ComparisonRuntime& runtime, SceneObjectId id) {
    const auto* comparison = findComparison(state,id); if (!comparison) { return; }
    const auto& surface = comparison->settings.quality.onOriginal ? runtime.results.value.original : runtime.results.value.repaired;
    if (!surface.tetrahedralQuality || surface.tetrahedralQuality->cells.empty()) { ImGui::TextWrapped("No tetrahedral cells in the selected sources."); return; }
    const auto& q = *surface.tetrahedralQuality; const auto& settings = comparison->settings.tetrahedral;
    const auto metric = static_cast<size_t>(settings.metric); const auto& stats = q.statistics[metric]; const auto& d = q.distributions[metric];
    ImGui::Text("%zu cells; %zu quadratic",q.cells.size(),q.quadraticCells);
    if (ImGui::BeginTable("tetrahedral_validity",2,ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        for (size_t i = 0; i < tetrahedralValidityCount; ++i) {
            ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextUnformatted(tetrahedralValidityName(static_cast<TetrahedralValidity>(i)));
            ImGui::TableNextColumn(); ImGui::Text("%zu",q.validityCounts[i]);
        }
        ImGui::EndTable();
    }
    ImGui::TextWrapped("Reversed: consistent negative node order. Folded: both Jacobian signs found. Unresolved: the bounds did not prove a sign. Collapse tolerance: 1e-12 after cell size normalization.");
    ImGui::TextWrapped("Magenta: invalid or collapsed. Gray: unresolved or unavailable. Quadratic faces use flat display triangles.");
    const auto palette = tetrahedralMetricHigherIsBetter(settings.metric) ? SurfaceQualityMetric::shape : SurfaceQualityMetric::longestEdge;
    const float legend = drawSurfaceQualityLegend(*ImGui::GetWindowDrawList(),ImGui::GetCursorScreenPos(),ImGui::GetContentRegionAvail().x,ImGui::GetFontSize(),palette,d);
    ImGui::Dummy({0,legend});
    if (stats.count) {
        ImGui::Text("Min %.5g | P5 %.5g | Median %.5g",stats.minimum,stats.percentile5,stats.median);
        ImGui::Text("P95 %.5g | Max %.5g",stats.percentile95,stats.maximum);
        std::array<float,surfaceQualityBinCount> bins{};
        for (size_t i = 0; i < bins.size(); ++i) { bins[i] = static_cast<float>(100.0*static_cast<double>(d.bins[0][i])/static_cast<double>(stats.count)); }
        ImGui::PlotHistogram("##tetrahedral_distribution",bins.data(),static_cast<int>(bins.size()),0,"Measured cells (%)",0,FLT_MAX,{0,ImGui::GetFontSize()*5});
    } else { ImGui::TextDisabled("No finite values for this metric."); }
    const auto limits = tetrahedralQualityOutsideLimits(q,settings);
    ImGui::Text("Below minimum: %zu | Above maximum: %zu",limits[0],limits[1]);
    ImGui::TextWrapped("Statistics include finite values from invalid cells. Validity is reported separately. Limits do not set validity or change colors.");
    const auto focus = comparison->tetrahedralFocus;
    const bool focused = focus && focus->signature == runtime.results.signature && focus->index < q.cells.size();
    if (focused) {
        const auto& cell = q.cells[focus->index];
        ImGui::Text("Selected source cell %llu: %s",static_cast<unsigned long long>(cell.sourceCell+1),tetrahedralValidityName(cell.validity));
        for (size_t i = 0; i < tetrahedralQualityMetricCount; ++i) {
            ImGui::Text("%s: %.7g",tetrahedralQualityMetricName(static_cast<TetrahedralQualityMetric>(i)),cell.values[i]);
        }
    }
    const auto list = [&](const char* label, const std::vector<uint32_t>& indices, bool reverse) {
        if (indices.empty()) { return; }
        ImGui::TextUnformatted(label); const auto height = ImGui::GetTextLineHeightWithSpacing();
        if (ImGui::BeginChild(label,{0,height*6},ImGuiChildFlags_Borders)) {
            ImGuiListClipper clipper; clipper.Begin(static_cast<int>(std::min(indices.size(),size_t{INT_MAX})),height);
            while (clipper.Step()) { for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                const auto index = indices[reverse ? indices.size()-1-static_cast<size_t>(row) : static_cast<size_t>(row)]; const auto& cell = q.cells[index];
                const auto part = findSceneObject(state,cell.partId);
                const std::string text = std::string(part ? part->name : "Source")+" / cell "+std::to_string(cell.sourceCell+1)
                    +" | "+measurementNumber(cell.values[metric])+" | "+tetrahedralValidityName(cell.validity)+"##"+std::to_string(index);
                if (ImGui::Selectable(text.c_str(),focused && focus->index == index)) { selectTetrahedralCell(state,surface.source,runtime.results.signature,index,id); }
            }}
        }
        ImGui::EndChild();
    };
    ImGui::TextWrapped("Select a row to frame its cell. Use Selected cell only to inspect an interior cell.");
    list("Validity findings",q.validityFindings,false);
    list("Cells by selected metric",q.sorted[metric],!tetrahedralMetricHigherIsBetter(settings.metric));
}
}

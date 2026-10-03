#include "uv_quality_view.h"
#include "ui_operations.h"
#include "comparison_scene.h"
#include "ui_icon_controls.h"

#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <limits>

namespace woby {
namespace {
void histogram(const UvQualityStatistics& s, ComparisonSettings& settings)
{
    ImGui::TextUnformatted("Distribution: blue = faces; orange = surface area");
    const auto origin = ImGui::GetCursorScreenPos();
    const float width = std::max(ImGui::GetContentRegionAvail().x,1.0f), height = ImGui::GetFontSize()*5;
    const float binWidth = width/static_cast<float>(uvHistogramBins);
    auto& draw = *ImGui::GetWindowDrawList();
    const auto fraction = [&](size_t i, bool area) {
        return area ? s.areas[i]/s.surfaceArea : static_cast<double>(s.counts[i])/static_cast<double>(s.count);
    };
    double peak = 0;
    for (size_t i=0;i<uvHistogramBins;++i) { peak = std::max({peak,fraction(i,false),fraction(i,true)}); }
    draw.AddRectFilled(origin,{origin.x+width,origin.y+height},IM_COL32(30,34,40,255));
    for (size_t i=0;i<uvHistogramBins;++i) {
        for (int side=0;side<2;++side) {
            const float x = origin.x+(static_cast<float>(i)+static_cast<float>(side)*.5f)*binWidth;
            const float h = static_cast<float>(fraction(i,side==1)/peak)*height;
            draw.AddRectFilled({x,origin.y+height-h},{x+binWidth*.45f,origin.y+height},
                side ? IM_COL32(255,166,64,255) : IM_COL32(77,166,255,255));
        }
    }
    ImGui::InvisibleButton("uv_histogram",{width,height});
    if (ImGui::IsItemHovered()) {
        const size_t bin = std::min(uvHistogramBins-1,static_cast<size_t>(std::max(0.0f,ImGui::GetIO().MousePos.x-origin.x)/binWidth));
        const double step = (s.histogramMaximum-s.histogramMinimum)/uvHistogramBins;
        const double low = s.histogramMinimum+static_cast<double>(bin)*step, high = low+step;
        ImGui::SetTooltip("[%.6g, %.6g%s\n%zu faces (%.3g%%); %.3g%% of surface area\nClick to highlight this range in cyan",
            low,high,bin+1==uvHistogramBins ? "]" : ")",s.counts[bin],fraction(bin,false)*100,fraction(bin,true)*100);
        if (ImGui::IsItemClicked()) {
            settings.uvRangeEnabled = true;
            settings.uvRangeMinimum = static_cast<float>(low);
            settings.uvRangeMaximum = static_cast<float>(high);
            if (bin+1 != uvHistogramBins) { settings.uvRangeMaximum = std::nextafter(settings.uvRangeMaximum,-std::numeric_limits<float>::infinity()); }
        }
    }
    ImGui::TextWrapped("%.5g to %.5g; vertical maximum %.3g%%",s.histogramMinimum,s.histogramMaximum,peak*100);
}
}

void drawUvQualityControls(UiState& state, ComparisonSettings& settings, const UvQuality* quality, SceneObjectId id)
{
    int metric = static_cast<int>(settings.uvMetric);
    const char* metrics[] = {"Angle distortion", "Signed area stretch", "UV orientation", "Stretch anisotropy", "Minimum local stretch", "UV overlaps"};
    if (ImGui::Combo("Metric",&metric,metrics,6)) {
        settings.uvMetric = static_cast<UvQualityMetric>(metric);
        constexpr float thresholds[] = {45,1,0,2,.01f,0};
        settings.uvThreshold = thresholds[metric]; settings.uvRangeEnabled = false;
        if (settings.uvMetric == UvQualityMetric::overlap) { settings.uvOverlapEnabled = true; }
    }
    int normalization = static_cast<int>(settings.uvNormalization);
    const char* normalizations[] = {"Per patch (relative)", "Absolute UV / surface"};
    if (ImGui::Combo("Area / stretch scale",&normalization,normalizations,2)) { settings.uvNormalization = static_cast<UvAreaNormalization>(normalization); }
    ImGui::TextWrapped("%s",uvQualityLegend(settings.uvMetric));
    ImGui::TextWrapped(settings.uvNormalization == UvAreaNormalization::perPatch
        ? "Area ratios use total UV / surface area as reference. Singular values use its square root. Anisotropy is unchanged."
        : "Area is UV units squared / surface units squared. Stretch is UV units / surface unit. Supplied UV domains are retained.");
    ImGui::TextWrapped("Magenta: collapsed UV. Gray: missing UVs or degenerate 3D triangles. Measurements describe the triangle mapping.");
    const bool numeric = settings.uvMetric != UvQualityMetric::orientation && settings.uvMetric != UvQualityMetric::overlap;
    if (numeric) {
        ImGui::Checkbox("Highlight threshold violations",&settings.uvThresholdEnabled);
        const char* label = settings.uvMetric == UvQualityMetric::minStretch ? "Stretch below"
            : settings.uvMetric == UvQualityMetric::area ? "Absolute log2 ratio above" : "Value above";
        ImGui::InputFloat(label,&settings.uvThreshold,0,0,"%.6g",ImGuiInputTextFlags_EnterReturnsTrue);
    }
    ImGui::InputFloat("Near-collapse stretch below",&settings.uvNearCollapse,0,0,"%.6g",ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::BeginDisabled(settings.uvMetric == UvQualityMetric::overlap);
    ImGui::Checkbox("Check UV overlaps",&settings.uvOverlapEnabled);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (drawRenderModeIconButton("uv_overlap_settings","\xef\x80\x93","UV overlap settings",RenderModeState::off,false)) {
        ImGui::OpenPopup("uv_overlap_settings_popup");
    }
    const auto buttonMax = ImGui::GetItemRectMax();
    ImGui::SetNextWindowPos({buttonMax.x,buttonMax.y+ImGui::GetStyle().ItemSpacing.y},ImGuiCond_Appearing,{1,0});
    ImGui::SetNextWindowSize({uiSize(360),0},ImGuiCond_Always);
    if (ImGui::BeginPopup("uv_overlap_settings_popup",ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        int scope = static_cast<int>(settings.uvOverlapScope);
        const char* scopes[] = {"Within each patch", "Selected patches share a domain"};
        if (ImGui::Combo("Overlap domain",&scope,scopes,2)) { settings.uvOverlapScope = static_cast<UvOverlapScope>(scope); }
        ImGui::TextWrapped("Checks original UVs, independent of display separation. Cross-patch overlap may be intentional. Shared edges and vertices are excluded.");
        ImGui::EndPopup();
    }
    if (settings.uvRangeEnabled) {
        ImGui::TextWrapped("Cyan range: %.6g to %.6g",settings.uvRangeMinimum,settings.uvRangeMaximum);
        if (ImGui::Button("Clear histogram selection")) { settings.uvRangeEnabled = false; }
    }
    if (!quality) { ImGui::TextDisabled("Computing UV measurements..."); return; }
    const auto& q = *quality;
    const auto& s = q.statistics;
    ImGui::TextWrapped("%zu collapsed UV; %zu mixed-orientation patches; %zu missing UV; %zu degenerate surface triangles",
        q.collapsed,q.mixedOrientationPatches,q.missing,q.degenerateSurface);
    ImGui::TextWrapped("%zu valid triangles near collapse (stretch < %.6g)",s.nearCollapseCount,q.settings.uvNearCollapse);
    if (numeric && s.count) {
        ImGui::TextWrapped("Minimum %.6g | Median %.6g | P95 %.6g | Maximum %.6g",s.minimum,s.median,s.percentile95,s.maximum);
        ImGui::TextWrapped("Threshold: %zu / %zu faces (%.3g%%); %.3g%% of valid surface area",s.thresholdCount,s.count,
            100*static_cast<double>(s.thresholdCount)/static_cast<double>(s.count),s.thresholdAreaPercent);
        ImGui::TextWrapped("Percentiles weight faces equally. Invalid triangles are excluded. Area statistics use physical surface area.");
        histogram(s,settings);
    }
    if (q.overlapChecked) {
        ImGui::TextWrapped("%zu overlap pairs: %zu within patches, %zu cross-patch; %zu affected triangles",
            q.overlaps.size(),q.overlaps.size()-q.crossPatchPairs,q.crossPatchPairs,q.overlappingTriangles);
        if (q.overlapTruncated) { ImGui::TextColored({1,.65f,.25f,1},"Partial result: work limit reached. Isolate fewer patches."); }
        if (ImGui::TreeNode("Overlap pairs")) {
            for (size_t i=0;i<std::min(q.overlaps.size(),size_t{100});++i) {
                const auto& pair = q.overlaps[i];
                const auto& a = q.triangles[pair.first]; const auto& b = q.triangles[pair.second];
                ImGui::PushID(static_cast<int>(i));
                ImGui::Text("%s: %zu / %zu",pair.crossPatch ? "Cross-patch" : "Within patch",a.triangle,b.triangle);
                ImGui::SameLine();
                if (ImGui::SmallButton("First")) { setUvProbe(state,id,a.partId,a.triangle,{1.0/3,1.0/3,1.0/3}); }
                ImGui::SameLine();
                if (ImGui::SmallButton("Second")) { setUvProbe(state,id,b.partId,b.triangle,{1.0/3,1.0/3,1.0/3}); }
                ImGui::PopID();
            }
            if (q.overlaps.size()>100) { ImGui::TextDisabled("First 100 pairs shown. Isolate patches to narrow the list."); }
            ImGui::TreePop();
        }
    }
    if (ImGui::TreeNode("UV findings")) {
        size_t shown = 0;
        for (const auto& t : q.triangles) {
            if (!t.collapsed && !t.mixedOrientation && !t.degenerateSurface
                && !(validUvTriangle(t) && t.minStretch < q.settings.uvNearCollapse) && !uvQualityHighlighted(t,settings)) { continue; }
            if (shown++ == 100) { ImGui::TextDisabled("First 100 shown. Isolate a patch to narrow the list."); break; }
            const auto object = findSceneObject(state,t.partId);
            const auto label = (object ? object->name : "Missing patch")+" / triangle "+std::to_string(t.triangle)
                +(t.collapsed ? " : collapsed UV" : t.degenerateSurface ? " : degenerate surface" : t.mixedOrientation ? " : mixed orientation" : " : threshold/range");
            ImGui::PushID(static_cast<int>(shown));
            if (ImGui::Selectable(label.c_str())) { setUvProbe(state,id,t.partId,t.triangle,{1.0/3,1.0/3,1.0/3}); }
            ImGui::PopID();
        }
        ImGui::TreePop();
    }
    const auto* comparison = findComparison(state,id);
    if (comparison && comparison->uvProbe && comparison->uvProbe->signature == comparisonGeometrySignature(state,id)) {
        const auto& probe = *comparison->uvProbe;
        const auto it = std::find_if(q.triangles.begin(),q.triangles.end(),[&](const auto& t) { return t.partId == probe.partId && t.triangle == probe.triangle; });
        if (it != q.triangles.end()) {
            UvPoint uv{};
            for (size_t i=0;i<3;++i) { for (size_t k=0;k<2;++k) { uv[k] += it->uv[i][k]*probe.barycentric[i]; } }
            ImGui::SeparatorText("Linked triangle probe");
            ImGui::Text("Part %llu / triangle %zu",static_cast<unsigned long long>(probe.partId),probe.triangle);
            if (!it->missing) { ImGui::Text("U %.9g | V %.9g",uv[0],uv[1]); }
            if (validUvTriangle(*it)) {
                ImGui::Text("Angle %.6g deg | Area log2 %.6g",it->angleDegrees,it->areaLog2);
                ImGui::Text("Anisotropy %.6g | Stretch %.6g to %.6g",it->anisotropy,it->minStretch,it->maxStretch);
            } else { ImGui::TextWrapped("Metric unavailable: missing UVs or degenerate mapping."); }
            if (ImGui::Button("Clear probe")) { clearUvProbe(state,id); }
        }
    }
}
} // namespace woby

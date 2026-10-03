#include "uv_quality.h"
#include "mesh_comparison.h"
#include "control_scene.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>
#include <cmath>
#include <numeric>

namespace {
woby::Mesh mappedTriangle(float u = 1, float v = 1)
{
    woby::Mesh mesh;
    mesh.vertices = {{{0,0,0},{},{0,0}},{{1,0,0},{},{u,0}},{{0,1,0},{},{0,v}}};
    mesh.indices = {0,1,2}; mesh.nodes = {{"patch",0,3}};
    mesh.nodes[0].hasTexcoords = true; mesh.nodes[0].sourceObjectId = 7;
    mesh.bounds = woby::calculateBounds(mesh.vertices);
    return mesh;
}
void appendUvTriangle(woby::Mesh& mesh, const std::array<woby::UvPoint,3>& uv, bool separate = false)
{
    const auto base = static_cast<uint32_t>(mesh.vertices.size());
    for (size_t i=0;i<3;++i) {
        auto vertex = mesh.vertices[i];
        vertex.texcoord = {static_cast<float>(uv[i][0]),static_cast<float>(uv[i][1])};
        mesh.vertices.push_back(vertex); mesh.indices.push_back(base+static_cast<uint32_t>(i));
    }
    if (separate) {
        mesh.nodes.push_back({"other patch",static_cast<uint32_t>(mesh.indices.size()-3),3});
        mesh.nodes.back().hasTexcoords = true; mesh.nodes.back().sourceObjectId = 8;
    } else { mesh.nodes.back().indexCount += 3; }
}
woby::UvQuality overlaps(const woby::Mesh& mesh, woby::UvOverlapScope scope = woby::UvOverlapScope::perPatch)
{
    woby::ComparisonSettings settings; settings.uvMetric = woby::UvQualityMetric::overlap; settings.uvOverlapScope = scope;
    auto quality = woby::analyzeUvQuality(mesh,settings);
    woby::inspectUvOverlaps(quality);
    return quality;
}
}

TEST_CASE("UV singular values follow surface to UV convention and patch normalization")
{
    auto mesh = mappedTriangle(4,.25f);
    auto q = woby::analyzeUvQuality(mesh,woby::UvAreaNormalization::absolute,woby::UvQualityMetric::anisotropy);
    CHECK(q.triangles[0].maxStretch == doctest::Approx(4));
    CHECK(q.triangles[0].minStretch == doctest::Approx(.25));
    CHECK(q.triangles[0].anisotropy == doctest::Approx(16));
    CHECK(q.triangles[0].areaLog2 == doctest::Approx(0));
    mesh = mappedTriangle(4,4);
    q = woby::analyzeUvQuality(mesh,woby::UvAreaNormalization::absolute,woby::UvQualityMetric::minStretch);
    CHECK(q.triangles[0].minStretch == doctest::Approx(4));
    CHECK(q.triangles[0].areaLog2 == doctest::Approx(4));
    q = woby::analyzeUvQuality(mesh,woby::UvAreaNormalization::perPatch,woby::UvQualityMetric::minStretch);
    CHECK(q.triangles[0].minStretch == doctest::Approx(1));
    CHECK(q.triangles[0].anisotropy == doctest::Approx(1));
    mesh.vertices[1].texcoord[0] = -4;
    q = woby::analyzeUvQuality(mesh,woby::UvAreaNormalization::absolute,woby::UvQualityMetric::minStretch);
    CHECK(q.triangles[0].orientation == -1);
    CHECK(q.triangles[0].minStretch == doctest::Approx(4));
    for (auto& vertex : mesh.vertices) {
        const auto p = vertex.position;
        vertex.position = {100+2*p[1],200,300-2*p[0]};
    }
    q = woby::analyzeUvQuality(mesh,woby::UvAreaNormalization::absolute,woby::UvQualityMetric::minStretch);
    CHECK(q.triangles[0].minStretch == doctest::Approx(2));
    q = woby::analyzeUvQuality(mesh,woby::UvAreaNormalization::perPatch,woby::UvQualityMetric::minStretch);
    CHECK(q.triangles[0].minStretch == doctest::Approx(1));
}

TEST_CASE("UV singular values handle shear near collapse and tiny scales")
{
    auto mesh = mappedTriangle(); mesh.vertices[2].texcoord = {1,1};
    auto q = woby::analyzeUvQuality(mesh,woby::UvAreaNormalization::absolute,woby::UvQualityMetric::anisotropy);
    CHECK(q.triangles[0].maxStretch == doctest::Approx((1+std::sqrt(5.0))/2));
    CHECK(q.triangles[0].minStretch == doctest::Approx((std::sqrt(5.0)-1)/2));
    mesh = mappedTriangle(1,1e-8f);
    q = woby::analyzeUvQuality(mesh,woby::UvAreaNormalization::absolute,woby::UvQualityMetric::minStretch);
    CHECK_FALSE(q.triangles[0].collapsed);
    CHECK(q.triangles[0].minStretch == doctest::Approx(1e-8).epsilon(1e-6));
    CHECK(q.statistics.nearCollapseCount == 1);
    CHECK(q.findings == std::vector<size_t>{0});
    CHECK(std::string(woby::uvFindingLabel(q.triangles[0], q.settings)) == "Near collapse");
    mesh = mappedTriangle(1e-20f,1e-20f);
    q = woby::analyzeUvQuality(mesh,woby::UvAreaNormalization::perPatch,woby::UvQualityMetric::minStretch);
    CHECK(q.triangles[0].minStretch == doctest::Approx(1));
    CHECK_FALSE(q.triangles[0].collapsed);
    mesh.vertices[2].texcoord = {0,0};
    q = woby::analyzeUvQuality(mesh,woby::UvAreaNormalization::absolute,woby::UvQualityMetric::minStretch);
    CHECK(q.collapsed == 1); CHECK(q.statistics.count == 0);
}

TEST_CASE("Signed UV area colors distinguish compression expansion and invalid triangles")
{
    auto mesh = mappedTriangle(.5f,.5f);
    mesh.uvQuality = std::make_shared<woby::UvQuality>(woby::analyzeUvQuality(mesh,woby::UvAreaNormalization::absolute,woby::UvQualityMetric::area));
    const auto compressed = woby::uvQualityVertices(mesh)[0].texcoord[0];
    mesh.vertices[1].texcoord = {2,0}; mesh.vertices[2].texcoord = {0,2};
    mesh.uvQuality = std::make_shared<woby::UvQuality>(woby::analyzeUvQuality(mesh,woby::UvAreaNormalization::absolute,woby::UvQualityMetric::area));
    const auto expanded = woby::uvQualityVertices(mesh)[0].texcoord[0];
    CHECK(compressed < .5f); CHECK(expanded > .5f);
    CHECK(compressed+expanded == doctest::Approx(1));
    const auto blue = woby::uvQualityColor(compressed,true), red = woby::uvQualityColor(expanded,true);
    CHECK(blue[2] > blue[0]); CHECK(red[0] > red[2]);
    CHECK(woby::uvQualityColor(.5,true) == std::array<float,4>{.92f,.92f,.92f,1});
    CHECK(woby::uvQualityColor(-1,true,true) == std::array<float,4>{1,0,1,1});
    CHECK(woby::uvQualityColor(-2,true,true) == std::array<float,4>{.56f,.61f,.67f,1});
}

TEST_CASE("UV statistics distinguish face percentiles from surface area percentages")
{
    auto mesh = mappedTriangle();
    appendUvTriangle(mesh,{{{0,0},{6,0},{0,3}}});
    mesh.vertices[4].position = {3,0,0}; mesh.vertices[5].position = {0,3,0};
    woby::ComparisonSettings settings; settings.uvMetric = woby::UvQualityMetric::anisotropy;
    settings.uvThreshold = 1.5f; settings.uvThresholdEnabled = true;
    auto q = woby::analyzeUvQuality(mesh,settings);
    CHECK(q.statistics.count == 2); CHECK(q.statistics.median == doctest::Approx(1.5));
    CHECK(q.statistics.percentile95 == doctest::Approx(1.95));
    CHECK(q.statistics.thresholdCount == 1); CHECK(q.statistics.thresholdAreaPercent == doctest::Approx(90));
    CHECK(q.statistics.highlightedCount == 1); CHECK(q.statistics.highlightedAreaPercent == doctest::Approx(90));
    CHECK(q.findings == std::vector<size_t>{1});
    CHECK(std::accumulate(q.statistics.counts.begin(),q.statistics.counts.end(),size_t{}) == 2);
    CHECK(std::accumulate(q.statistics.areas.begin(),q.statistics.areas.end(),0.0) == doctest::Approx(5));
    CHECK_FALSE(woby::uvQualityHighlighted(q.triangles[0],settings));
    CHECK(woby::uvQualityHighlighted(q.triangles[1],settings));
    settings.uvRangeEnabled = true; settings.uvRangeMinimum = 1; settings.uvRangeMaximum = 1.2f;
    CHECK(woby::uvQualityHighlighted(q.triangles[0],settings));
    CHECK_FALSE(woby::uvQualityHighlighted(q.triangles[1],settings));
    q = woby::analyzeUvQuality(mesh,settings);
    CHECK(q.statistics.highlightedCount == 1); CHECK(q.statistics.highlightedAreaPercent == doctest::Approx(10));
    CHECK(q.findings == std::vector<size_t>{0});
    settings.uvMetric = woby::UvQualityMetric::minStretch; settings.uvNormalization = woby::UvAreaNormalization::absolute;
    settings.uvThreshold = 1.1f; q = woby::analyzeUvQuality(mesh,settings);
    CHECK(q.statistics.thresholdCount == 2);
    mesh.nodes[0].hasTexcoords = false;
    q = woby::analyzeUvQuality(mesh,settings);
    CHECK(q.statistics.count == 0); CHECK(q.statistics.thresholdAreaPercent == 0);
    CHECK(q.statistics.highlightedCount == 0); CHECK(q.statistics.highlightedAreaPercent == 0);
}

TEST_CASE("UV overlap detection excludes boundary contact and detects containment and duplicates")
{
    auto mesh = mappedTriangle();
    appendUvTriangle(mesh,{{{1,0},{1,1},{0,1}}}); // Shared diagonal only.
    CHECK(overlaps(mesh).overlaps.empty());
    appendUvTriangle(mesh,{{{1,1},{2,1},{1,2}}}); // Shared vertex only.
    CHECK(overlaps(mesh).overlaps.empty());
    appendUvTriangle(mesh,{{{.1,.1},{.2,.1},{.1,.2}}});
    auto q = overlaps(mesh);
    REQUIRE(q.overlaps.size() == 1); CHECK(q.overlappingTriangles == 2);
    CHECK(q.overlaps[0].first == 0); CHECK(q.overlaps[0].second == 3);
    mesh = mappedTriangle();
    appendUvTriangle(mesh,{{{0,0},{0,1},{1,0}}}); // Mirrored coincident triangle.
    CHECK(overlaps(mesh).overlaps.size() == 1);
    mesh.vertices[5].texcoord = {0,0}; // Collapsed UV excluded.
    CHECK(overlaps(mesh).overlaps.empty());
}

TEST_CASE("UV overlap scope preserves independent domains and reports partial results")
{
    auto mesh = mappedTriangle();
    appendUvTriangle(mesh,{{{0,0},{1,0},{0,1}}},true);
    CHECK(overlaps(mesh).overlaps.empty());
    auto q = overlaps(mesh,woby::UvOverlapScope::selectedPatches);
    REQUIRE(q.overlaps.size() == 1); CHECK(q.crossPatchPairs == 1);
    CHECK(q.findings == std::vector<size_t>{0, 1});
    CHECK(q.triangles[0].crossPatchOverlap); CHECK_FALSE(q.triangles[0].overlapping);
    woby::inspectUvOverlaps(q,{}, {0,100});
    CHECK(q.overlapTruncated); CHECK(q.overlapChecked); CHECK(q.overlaps.empty());
    woby::inspectUvOverlaps(q,{}, {100,0}); CHECK(q.overlapTruncated);
    std::stop_source stop; stop.request_stop();
    CHECK_THROWS(woby::inspectUvOverlaps(q,stop.get_token()));
    q.settings.uvOverlapEnabled = false;
    woby::inspectUvOverlaps(q); CHECK_FALSE(q.overlapChecked); CHECK_FALSE(q.triangles[0].crossPatchOverlap);
    CHECK(q.findings.empty());
}

TEST_CASE("UV overlap worker publishes immutable results and reports statistics")
{
    auto mesh = mappedTriangle(); appendUvTriangle(mesh,{{{0,0},{1,0},{0,1}}});
    woby::ComparisonSettings settings; settings.uvMetric = woby::UvQualityMetric::overlap;
    mesh.uvQuality = std::make_shared<woby::UvQuality>(woby::analyzeUvQuality(mesh,settings));
    const auto result = woby::computeComparisonStages(mesh,{},woby::comparisonSource);
    CHECK_FALSE(mesh.uvQuality->overlapChecked);
    REQUIRE(result.original.source.uvQuality);
    CHECK(result.original.source.uvQuality->overlapChecked);
    const auto report = woby::controlComparisonResults(result,0,false)["uvQuality"];
    CHECK(report["convention"] == "surface_to_uv");
    CHECK(report["overlaps"]["pairs"].size() == 1);
    CHECK(report["overlaps"]["truncated"] == false);
    CHECK(report["statistics"]["count"] == 2);
}

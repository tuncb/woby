#include "uv_quality.h"
#include "mesh_comparison.h"
#include "control_scene.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>
#include <cmath>
#include <numeric>
#include <algorithm>
#include <limits>
#include <random>
#include <tuple>

namespace {
woby::Mesh mappedTriangle(float u = 1, float v = 1)
{
    woby::Mesh mesh;
    editMeshGeometry(mesh).vertices = {{{0,0,0},{},{0,0}},{{1,0,0},{},{u,0}},{{0,1,0},{},{0,v}}};
    editMeshGeometry(mesh).indices = {0,1,2}; editMeshGeometry(mesh).nodes = {{"patch",0,3}};
    editMeshGeometry(mesh).nodes[0].hasTexcoords = true; editMeshGeometry(mesh).nodes[0].sourceObjectId = 7;
    mesh.bounds = woby::calculateBounds(meshGeometry(mesh).vertices);
    return mesh;
}
void appendUvTriangle(woby::Mesh& mesh, const std::array<woby::UvPoint,3>& uv, bool separate = false)
{
    const auto base = static_cast<uint32_t>(meshGeometry(mesh).vertices.size());
    for (size_t i=0;i<3;++i) {
        auto vertex = meshGeometry(mesh).vertices[i];
        vertex.texcoord = {static_cast<float>(uv[i][0]),static_cast<float>(uv[i][1])};
        editMeshGeometry(mesh).vertices.push_back(vertex); editMeshGeometry(mesh).indices.push_back(base+static_cast<uint32_t>(i));
    }
    if (separate) {
        editMeshGeometry(mesh).nodes.push_back({"other patch",static_cast<uint32_t>(meshGeometry(mesh).indices.size()-3),3});
        editMeshGeometry(mesh).nodes.back().hasTexcoords = true; editMeshGeometry(mesh).nodes.back().sourceObjectId = 8;
    } else { editMeshGeometry(mesh).nodes.back().indexCount += 3; }
}
woby::UvQuality overlaps(const woby::Mesh& mesh, woby::UvOverlapScope scope = woby::UvOverlapScope::perPatch)
{
    woby::ComparisonSettings settings; settings.uvMetric = woby::UvQualityMetric::overlap; settings.uvOverlapScope = scope;
    auto quality = woby::analyzeUvQuality(mesh,settings);
    woby::inspectUvOverlaps(quality);
    return quality;
}
woby::UvTriangleQuality sweepTriangle(double x, double y, double side = 1, size_t patch = 0)
{
    woby::UvTriangleQuality t;
    t.uv = {{{x,y},{x+side,y},{x,y+side}}}; t.patch = patch; t.orientation = 1;
    t.surfaceArea = t.uvArea = .5; t.minStretch = t.maxStretch = 1;
    return t;
}
woby::UvQuality sweepQuality(std::vector<woby::UvTriangleQuality> triangles)
{
    auto measurements = std::make_shared<woby::UvMeasurements>();
    measurements->triangles = std::move(triangles);
    measurements->patchAreaLog2.resize(4); measurements->patchStretchScale.resize(4,1);
    woby::UvQuality q; q.measurements = std::move(measurements); q.settings.uvOverlapEnabled = true;
    return q;
}
}

TEST_CASE("UV sweep completes disjoint rows and columns with no exact candidates")
{
    for (bool transpose : {false,true}) {
        std::vector<woby::UvTriangleQuality> triangles;
        for (size_t i=0; i<2001; ++i) { triangles.push_back(sweepTriangle(0,static_cast<double>(i)*2)); }
        std::mt19937 rng(12); std::shuffle(triangles.begin(),triangles.end(),rng);
        if (transpose) { for (auto& t : triangles) {
            for (auto& p : t.uv) { std::swap(p[0],p[1]); } t.orientation = -1;
        } }
        auto q = sweepQuality(std::move(triangles));
        woby::inspectUvOverlaps(q,{}, {0,0});
        const auto& r = woby::uvOverlapResult(q);
        CHECK_FALSE(r.truncated); CHECK(r.candidates == 0); CHECK(r.pairs.empty());
        CHECK(r.truncation == woby::UvOverlapTruncation::none);
        CHECK(r.searchWork <= r.searchWorkLimit);
    }
}

TEST_CASE("UV sweep agrees with independent all pairs geometry and deterministic event order")
{
    std::mt19937 rng(823);
    for (int trial=0; trial<6; ++trial) {
        std::vector<woby::UvTriangleQuality> triangles;
        for (size_t i=0; i<160; ++i) {
            auto t = sweepTriangle(static_cast<double>(rng()%50)-25,static_cast<double>(rng()%30)-15,
                static_cast<double>(rng()%15+1),rng()%4);
            t.missing = i%37 == 0; t.collapsed = i%41 == 0; t.degenerateSurface = i%43 == 0;
            triangles.push_back(t);
        }
        for (auto scope : {woby::UvOverlapScope::perPatch,woby::UvOverlapScope::selectedPatches}) {
            auto q = sweepQuality(triangles); q.settings.uvOverlapScope = scope;
            const auto uKey = [&](size_t i) {
                return std::tuple{scope == woby::UvOverlapScope::perPatch ? triangles[i].patch : size_t{},triangles[i].uv[0][0],i};
            };
            std::vector<std::pair<size_t,size_t>> expected;
            // For these right triangles, positive area exists iff the lowest shared
            // x+y lies strictly below both hypotenuses. This is independent of the SAT.
            for (size_t i=0; i<triangles.size(); ++i) { for (size_t j=i+1; j<triangles.size(); ++j) {
                const auto& a = triangles[i]; const auto& b = triangles[j];
                if (!woby::validUvTriangle(a) || !woby::validUvTriangle(b)
                    || (scope == woby::UvOverlapScope::perPatch && a.patch != b.patch)) { continue; }
                const double lower = std::max(a.uv[0][0],b.uv[0][0])+std::max(a.uv[0][1],b.uv[0][1]);
                if (lower < std::min(a.uv[1][0]+a.uv[0][1],b.uv[1][0]+b.uv[0][1])) {
                    expected.emplace_back(uKey(i)<uKey(j) ? i : j,uKey(i)<uKey(j) ? j : i);
                }
            } }
            std::sort(expected.begin(),expected.end(),[&](const auto& a, const auto& b) {
                return std::tuple{uKey(a.second),triangles[a.first].uv[0][1],uKey(a.first)}
                    < std::tuple{uKey(b.second),triangles[b.first].uv[0][1],uKey(b.first)};
            });
            for (size_t cap : {size_t{0},size_t{7},expected.size()}) {
                woby::inspectUvOverlaps(q,{}, {100000,cap});
                const auto& r = woby::uvOverlapResult(q);
                REQUIRE(r.pairs.size() == std::min(cap,expected.size()));
                CHECK(r.truncated == (cap < expected.size()));
                CHECK(r.truncation == (r.truncated ? woby::UvOverlapTruncation::pairs : woby::UvOverlapTruncation::none));
                std::vector<uint8_t> flags(triangles.size()); size_t cross = 0;
                for (size_t i=0; i<r.pairs.size(); ++i) {
                    const auto& pair = r.pairs[i];
                    CHECK(pair.first == expected[i].first); CHECK(pair.second == expected[i].second);
                    const bool crossPatch = triangles[pair.first].patch != triangles[pair.second].patch;
                    CHECK(pair.crossPatch == crossPatch); if (crossPatch) { ++cross; }
                    const uint8_t flag = crossPatch ? uint8_t{2} : uint8_t{1};
                    flags[pair.first] |= flag; flags[pair.second] |= flag;
                }
                CHECK(r.flags == flags); CHECK(r.crossPatchPairs == cross);
                CHECK(r.overlappingTriangles == static_cast<size_t>(std::count_if(flags.begin(),flags.end(),[](auto f) { return f != 0; })));
                CHECK(r.searchWork <= r.searchWorkLimit);
            }
        }
    }
}

TEST_CASE("UV sweep distinguishes limits and preserves deterministic partial prefixes")
{
    auto q = sweepQuality(std::vector<woby::UvTriangleQuality>(6,sweepTriangle(0,0)));
    for (size_t cap : {size_t{0},size_t{1},size_t{4},size_t{15},size_t{16}}) {
        woby::inspectUvOverlaps(q,{}, {cap,100});
        CHECK(woby::uvOverlapResult(q).candidates == std::min(cap,size_t{15}));
        CHECK(woby::uvOverlapResult(q).pairs.size() == std::min(cap,size_t{15}));
        CHECK(woby::uvOverlapResult(q).truncation == (cap < 15 ? woby::UvOverlapTruncation::candidates : woby::UvOverlapTruncation::none));
    }
    woby::inspectUvOverlaps(q,{}, {100,4});
    const auto retained = q.overlap;
    REQUIRE(retained->pairs.size() == 4);
    CHECK(retained->flags == std::vector<uint8_t>{1,1,1,1,0,0});
    CHECK(retained->pairs[0].first == 0); CHECK(retained->pairs[0].second == 1);
    CHECK(retained->pairs[1].first == 0); CHECK(retained->pairs[1].second == 2);
    CHECK(retained->pairs[2].first == 1); CHECK(retained->pairs[2].second == 2);
    CHECK(retained->pairs[3].first == 0); CHECK(retained->pairs[3].second == 3);
    CHECK(retained->truncation == woby::UvOverlapTruncation::pairs);
    auto settings = q.settings; settings.uvThreshold = 7;
    woby::updateUvQualitySettings(q,settings); CHECK(q.overlap == retained);

    for (size_t budget : {size_t{0},size_t{1},size_t{5},size_t{10}}) {
        woby::inspectUvOverlaps(q,{}, {100,100,budget});
        const auto bounded = q.overlap;
        CHECK(bounded->truncated); CHECK(bounded->truncation == woby::UvOverlapTruncation::searchWork);
        CHECK(bounded->searchWork == budget); CHECK(bounded->searchWorkLimit == budget);
        woby::inspectUvOverlaps(q,{}, {100,100,budget});
        REQUIRE(q.overlap->pairs.size() == bounded->pairs.size());
        for (size_t i=0; i<bounded->pairs.size(); ++i) {
            CHECK(q.overlap->pairs[i].first == bounded->pairs[i].first);
            CHECK(q.overlap->pairs[i].second == bounded->pairs[i].second);
        }
        CHECK(q.overlap->flags == bounded->flags);
    }
    woby::inspectUvOverlaps(q,{}, {std::numeric_limits<size_t>::max(),100});
    CHECK_FALSE(q.overlap->truncated); CHECK(q.overlap->searchWorkLimit == std::numeric_limits<size_t>::max());
    auto empty = sweepQuality({}); woby::inspectUvOverlaps(empty,{}, {0,0,0});
    CHECK_FALSE(empty.overlap->truncated);
}

TEST_CASE("UV singular values follow surface to UV convention and patch normalization")
{
    auto mesh = mappedTriangle(4,.25f);
    auto q = woby::analyzeUvQuality(mesh,woby::UvAreaNormalization::absolute,woby::UvQualityMetric::anisotropy);
    CHECK(woby::uvTriangleQuality(q, 0).maxStretch == doctest::Approx(4));
    CHECK(woby::uvTriangleQuality(q, 0).minStretch == doctest::Approx(.25));
    CHECK(woby::uvTriangleQuality(q, 0).anisotropy == doctest::Approx(16));
    CHECK(woby::uvTriangleQuality(q, 0).areaLog2 == doctest::Approx(0));
    mesh = mappedTriangle(4,4);
    q = woby::analyzeUvQuality(mesh,woby::UvAreaNormalization::absolute,woby::UvQualityMetric::minStretch);
    CHECK(woby::uvTriangleQuality(q, 0).minStretch == doctest::Approx(4));
    CHECK(woby::uvTriangleQuality(q, 0).areaLog2 == doctest::Approx(4));
    q = woby::analyzeUvQuality(mesh,woby::UvAreaNormalization::perPatch,woby::UvQualityMetric::minStretch);
    CHECK(woby::uvTriangleQuality(q, 0).minStretch == doctest::Approx(1));
    CHECK(woby::uvTriangleQuality(q, 0).anisotropy == doctest::Approx(1));
    editMeshGeometry(mesh).vertices[1].texcoord[0] = -4;
    q = woby::analyzeUvQuality(mesh,woby::UvAreaNormalization::absolute,woby::UvQualityMetric::minStretch);
    CHECK(woby::uvTriangleQuality(q, 0).orientation == -1);
    CHECK(woby::uvTriangleQuality(q, 0).minStretch == doctest::Approx(4));
    for (auto& vertex : editMeshGeometry(mesh).vertices) {
        const auto p = vertex.position;
        vertex.position = {100+2*p[1],200,300-2*p[0]};
    }
    q = woby::analyzeUvQuality(mesh,woby::UvAreaNormalization::absolute,woby::UvQualityMetric::minStretch);
    CHECK(woby::uvTriangleQuality(q, 0).minStretch == doctest::Approx(2));
    q = woby::analyzeUvQuality(mesh,woby::UvAreaNormalization::perPatch,woby::UvQualityMetric::minStretch);
    CHECK(woby::uvTriangleQuality(q, 0).minStretch == doctest::Approx(1));
}

TEST_CASE("UV singular values handle shear near collapse and tiny scales")
{
    auto mesh = mappedTriangle(); editMeshGeometry(mesh).vertices[2].texcoord = {1,1};
    auto q = woby::analyzeUvQuality(mesh,woby::UvAreaNormalization::absolute,woby::UvQualityMetric::anisotropy);
    CHECK(woby::uvTriangleQuality(q, 0).maxStretch == doctest::Approx((1+std::sqrt(5.0))/2));
    CHECK(woby::uvTriangleQuality(q, 0).minStretch == doctest::Approx((std::sqrt(5.0)-1)/2));
    mesh = mappedTriangle(1,1e-8f);
    q = woby::analyzeUvQuality(mesh,woby::UvAreaNormalization::absolute,woby::UvQualityMetric::minStretch);
    CHECK_FALSE(woby::uvTriangleQuality(q, 0).collapsed);
    CHECK(woby::uvTriangleQuality(q, 0).minStretch == doctest::Approx(1e-8).epsilon(1e-6));
    CHECK(q.statistics.nearCollapseCount == 1);
    CHECK(q.findings == std::vector<size_t>{0});
    CHECK(std::string(woby::uvFindingLabel(woby::uvTriangleQuality(q, 0), q.settings)) == "Near collapse");
    mesh = mappedTriangle(1e-20f,1e-20f);
    q = woby::analyzeUvQuality(mesh,woby::UvAreaNormalization::perPatch,woby::UvQualityMetric::minStretch);
    CHECK(woby::uvTriangleQuality(q, 0).minStretch == doctest::Approx(1));
    CHECK_FALSE(woby::uvTriangleQuality(q, 0).collapsed);
    editMeshGeometry(mesh).vertices[2].texcoord = {0,0};
    q = woby::analyzeUvQuality(mesh,woby::UvAreaNormalization::absolute,woby::UvQualityMetric::minStretch);
    CHECK(woby::uvMeasurements(q).collapsed == 1); CHECK(q.statistics.count == 0);
}

TEST_CASE("Signed UV area colors distinguish compression expansion and invalid triangles")
{
    auto mesh = mappedTriangle(.5f,.5f);
    mesh.uvQuality = std::make_shared<woby::UvQuality>(woby::analyzeUvQuality(mesh,woby::UvAreaNormalization::absolute,woby::UvQualityMetric::area));
    const auto compressed = woby::uvQualityVertices(mesh)[0].texcoord[0];
    editMeshGeometry(mesh).vertices[1].texcoord = {2,0}; editMeshGeometry(mesh).vertices[2].texcoord = {0,2};
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
    editMeshGeometry(mesh).vertices[4].position = {3,0,0}; editMeshGeometry(mesh).vertices[5].position = {0,3,0};
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
    CHECK_FALSE(woby::uvQualityHighlighted(woby::uvTriangleQuality(q, 0),settings));
    CHECK(woby::uvQualityHighlighted(woby::uvTriangleQuality(q, 1),settings));
    settings.uvRangeEnabled = true; settings.uvRangeMinimum = 1; settings.uvRangeMaximum = 1.2f;
    CHECK(woby::uvQualityHighlighted(woby::uvTriangleQuality(q, 0),settings));
    CHECK_FALSE(woby::uvQualityHighlighted(woby::uvTriangleQuality(q, 1),settings));
    q = woby::analyzeUvQuality(mesh,settings);
    CHECK(q.statistics.highlightedCount == 1); CHECK(q.statistics.highlightedAreaPercent == doctest::Approx(10));
    CHECK(q.findings == std::vector<size_t>{0});
    settings.uvMetric = woby::UvQualityMetric::minStretch; settings.uvNormalization = woby::UvAreaNormalization::absolute;
    settings.uvThreshold = 1.1f; q = woby::analyzeUvQuality(mesh,settings);
    CHECK(q.statistics.thresholdCount == 2);
    editMeshGeometry(mesh).nodes[0].hasTexcoords = false;
    q = woby::analyzeUvQuality(mesh,settings);
    CHECK(q.statistics.count == 0); CHECK(q.statistics.thresholdAreaPercent == 0);
    CHECK(q.statistics.highlightedCount == 0); CHECK(q.statistics.highlightedAreaPercent == 0);
}

TEST_CASE("UV overlap detection excludes boundary contact and detects containment and duplicates")
{
    auto mesh = mappedTriangle();
    appendUvTriangle(mesh,{{{1,0},{1,1},{0,1}}}); // Shared diagonal only.
    CHECK(woby::uvOverlapResult(overlaps(mesh)).pairs.empty());
    appendUvTriangle(mesh,{{{1,1},{2,1},{1,2}}}); // Shared vertex only.
    CHECK(woby::uvOverlapResult(overlaps(mesh)).pairs.empty());
    appendUvTriangle(mesh,{{{.1,.1},{.2,.1},{.1,.2}}});
    auto q = overlaps(mesh);
    REQUIRE(woby::uvOverlapResult(q).pairs.size() == 1); CHECK(woby::uvOverlapResult(q).overlappingTriangles == 2);
    CHECK(woby::uvOverlapResult(q).pairs[0].first == 0); CHECK(woby::uvOverlapResult(q).pairs[0].second == 3);
    mesh = mappedTriangle();
    appendUvTriangle(mesh,{{{0,0},{0,1},{1,0}}}); // Mirrored coincident triangle.
    CHECK(woby::uvOverlapResult(overlaps(mesh)).pairs.size() == 1);
    editMeshGeometry(mesh).vertices[5].texcoord = {0,0}; // Collapsed UV excluded.
    CHECK(woby::uvOverlapResult(overlaps(mesh)).pairs.empty());
}

TEST_CASE("UV overlap scope preserves independent domains and reports partial results")
{
    auto mesh = mappedTriangle();
    appendUvTriangle(mesh,{{{0,0},{1,0},{0,1}}},true);
    CHECK(woby::uvOverlapResult(overlaps(mesh)).pairs.empty());
    auto q = overlaps(mesh,woby::UvOverlapScope::selectedPatches);
    REQUIRE(woby::uvOverlapResult(q).pairs.size() == 1); CHECK(woby::uvOverlapResult(q).crossPatchPairs == 1);
    CHECK(q.findings == std::vector<size_t>{0, 1});
    CHECK(woby::uvTriangleQuality(q, 0).crossPatchOverlap); CHECK_FALSE(woby::uvTriangleQuality(q, 0).overlapping);
    woby::inspectUvOverlaps(q,{}, {0,100});
    CHECK(woby::uvOverlapResult(q).truncated); CHECK(bool(q.overlap)); CHECK(woby::uvOverlapResult(q).pairs.empty());
    woby::inspectUvOverlaps(q,{}, {100,0}); CHECK(woby::uvOverlapResult(q).truncated);
    std::stop_source stop; stop.request_stop();
    CHECK_THROWS(woby::inspectUvOverlaps(q,stop.get_token()));
    q.settings.uvOverlapEnabled = false;
    woby::inspectUvOverlaps(q); CHECK_FALSE(bool(q.overlap)); CHECK_FALSE(woby::uvTriangleQuality(q, 0).crossPatchOverlap);
    CHECK(q.findings.empty());
}

TEST_CASE("UV overlap worker publishes immutable results and reports statistics")
{
    auto mesh = mappedTriangle(); appendUvTriangle(mesh,{{{0,0},{1,0},{0,1}}});
    woby::ComparisonSettings settings; settings.uvMetric = woby::UvQualityMetric::overlap;
    mesh.uvQuality = std::make_shared<woby::UvQuality>(woby::analyzeUvQuality(mesh,settings));
    const auto result = woby::computeComparisonStages(mesh,{},woby::comparisonSource);
    CHECK_FALSE(bool(mesh.uvQuality->overlap));
    REQUIRE(result.original.source.uvQuality);
    CHECK(bool(result.original.source.uvQuality->overlap));
    const auto report = woby::controlComparisonResults(result,0,false)["uvQuality"];
    CHECK(report["convention"] == "surface_to_uv");
    CHECK(report["overlaps"]["pairs"].size() == 1);
    CHECK(report["overlaps"]["truncated"] == false);
    CHECK(report["overlaps"]["truncationReason"] == "none");
    CHECK(report["overlaps"]["pairOrder"] == "uv_sweep_v1");
    CHECK(report["overlaps"]["searchWorkCount"].get<size_t>() <= report["overlaps"]["searchWorkLimit"].get<size_t>());
    CHECK(report["statistics"]["count"] == 2);
}

TEST_CASE("UV cached settings preserve all six metrics distributions and normalization round trips")
{
    auto mesh = mappedTriangle(4, 2);
    appendUvTriangle(mesh, {{{0,0},{2,0},{1,1}}});
    appendUvTriangle(mesh, {{{0,0},{0,1},{1,0}}}, true);
    appendUvTriangle(mesh, {{{0,0},{0,0},{0,0}}});
    woby::ComparisonSettings settings;
    auto cached = woby::analyzeUvQuality(mesh, settings);
    for (const auto normalization : {woby::UvAreaNormalization::absolute, woby::UvAreaNormalization::perPatch,
            woby::UvAreaNormalization::absolute, woby::UvAreaNormalization::perPatch}) {
        for (const auto metric : {woby::UvQualityMetric::angle, woby::UvQualityMetric::area,
                woby::UvQualityMetric::orientation, woby::UvQualityMetric::anisotropy,
                woby::UvQualityMetric::minStretch, woby::UvQualityMetric::overlap}) {
            settings.uvMetric = metric;
            settings.uvNormalization = normalization;
            settings.uvOverlapEnabled = true;
            settings.uvOverlapScope = woby::UvOverlapScope::selectedPatches;
            settings.uvThresholdEnabled = true;
            settings.uvThreshold = .7f;
            settings.uvNearCollapse = .8f;
            settings.uvRangeEnabled = true;
            settings.uvRangeMinimum = .1f;
            settings.uvRangeMaximum = 2;
            woby::updateUvQualitySettings(cached, settings);
            auto fresh = woby::analyzeUvQuality(mesh, settings);
            woby::inspectUvOverlaps(fresh);
            CHECK(cached.findings == fresh.findings);
            CHECK(woby::uvOverlapResult(cached).candidates == woby::uvOverlapResult(fresh).candidates);
            CHECK(woby::uvOverlapResult(cached).pairs.size() == woby::uvOverlapResult(fresh).pairs.size());
            CHECK(cached.statistics.counts == fresh.statistics.counts);
            CHECK(cached.statistics.areas == fresh.statistics.areas);
            CHECK(cached.statistics.median == fresh.statistics.median);
            CHECK(cached.statistics.percentile95 == fresh.statistics.percentile95);
            CHECK(cached.statistics.thresholdCount == fresh.statistics.thresholdCount);
            CHECK(cached.statistics.nearCollapseCount == fresh.statistics.nearCollapseCount);
            CHECK(cached.statistics.highlightedAreaPercent == fresh.statistics.highlightedAreaPercent);
            for (size_t i = 0; i < woby::uvMeasurements(cached).triangles.size(); ++i) {
                const auto& a = woby::uvTriangleQuality(cached, i); const auto& b = woby::uvTriangleQuality(fresh, i);
                CHECK(a.partId == b.partId); CHECK(a.triangle == b.triangle); CHECK(a.patch == b.patch);
                CHECK(a.uv == b.uv); CHECK(a.orientation == b.orientation);
                CHECK(a.areaLog2 == b.areaLog2); CHECK(a.angleDegrees == b.angleDegrees);
                CHECK(a.minStretch == b.minStretch); CHECK(a.maxStretch == b.maxStretch);
                CHECK(a.anisotropy == b.anisotropy);
                CHECK(a.overlapping == b.overlapping); CHECK(a.crossPatchOverlap == b.crossPatchOverlap);
            }
        }
    }
    for (size_t i = 0; i < cached.distributions.size(); ++i) {
        const bool used = i < 6 || i == 6 + static_cast<size_t>(woby::UvQualityMetric::area)
            || i == 6 + static_cast<size_t>(woby::UvQualityMetric::minStretch);
        CHECK(cached.distributions[i].has_value() == used);
    }
    std::stop_source stop; stop.request_stop();
    CHECK_THROWS(woby::updateUvQualitySettings(cached, settings, stop.get_token()));
}

TEST_CASE("UV presentation edits reuse bounded overlap results and invalidate only dependent distributions")
{
    auto mesh = mappedTriangle();
    appendUvTriangle(mesh, {{{0,0},{1,0},{0,1}}}, true);
    auto quality = overlaps(mesh, woby::UvOverlapScope::selectedPatches);
    woby::inspectUvOverlaps(quality, {}, {100, 0});
    REQUIRE(woby::uvOverlapResult(quality).truncated);
    const auto candidates = woby::uvOverlapResult(quality).candidates;
    auto settings = quality.settings;
    settings.uvThresholdEnabled = true; settings.uvThreshold = .1f;
    settings.uvRangeEnabled = true; settings.uvRangeMinimum = 0; settings.uvRangeMaximum = 1;
    settings.uvNormalization = woby::UvAreaNormalization::absolute;
    woby::updateUvQualitySettings(quality, settings);
    CHECK(woby::uvOverlapResult(quality).truncated); CHECK(woby::uvOverlapResult(quality).pairs.empty());
    CHECK(woby::uvOverlapResult(quality).candidates == candidates);
    CHECK(quality.statistics.highlightedCount == 2);
    CHECK(quality.distributions[static_cast<size_t>(woby::UvQualityMetric::overlap)].has_value());
    settings.uvOverlapScope = woby::UvOverlapScope::perPatch;
    woby::updateUvQualitySettings(quality, settings);
    CHECK_FALSE(woby::uvOverlapResult(quality).truncated); CHECK(woby::uvOverlapResult(quality).pairs.empty());
    settings.uvOverlapScope = woby::UvOverlapScope::selectedPatches;
    woby::updateUvQualitySettings(quality, settings);
    CHECK(woby::uvOverlapResult(quality).crossPatchPairs == 1);
    CHECK(woby::uvTriangleQuality(quality, 0).crossPatchOverlap);
    settings.uvMetric = woby::UvQualityMetric::angle; settings.uvOverlapEnabled = false;
    woby::updateUvQualitySettings(quality, settings);
    CHECK_FALSE(bool(quality.overlap)); CHECK_FALSE(woby::uvTriangleQuality(quality, 0).crossPatchOverlap);
    CHECK_FALSE(quality.distributions[static_cast<size_t>(woby::UvQualityMetric::overlap)].has_value());
}

TEST_CASE("UV face records preserve corner normals measurement offsets and node order")
{
    auto mesh = mappedTriangle();
    appendUvTriangle(mesh, {{{0,0},{2,0},{0,2}}}, true);
    auto& geometry = editMeshGeometry(mesh);
    for (size_t i = 0; i < geometry.vertices.size(); ++i) {
        geometry.vertices[i].normal = {static_cast<float>(i),static_cast<float>(i+1),static_cast<float>(i+2)};
    }
    geometry.nodes[0].uvQualityOffset = 2;
    geometry.nodes[1].uvQualityOffset = 0;
    std::reverse(geometry.nodes.begin(), geometry.nodes.end());
    woby::UvQuality quality;
    auto measurements = std::make_shared<woby::UvMeasurements>();
    measurements->patchAreaLog2 = {2}; measurements->patchStretchScale = {2};
    measurements->triangles.resize(3);
    measurements->triangles[0].angleDegrees = 45;
    measurements->triangles[0].anisotropy = 8;
    measurements->triangles[0].areaLog2 = 1;
    measurements->triangles[0].minStretch = .01;
    measurements->triangles[0].orientation = -1;

    measurements->triangles[2].missing = true;
    quality.measurements = measurements;
    auto overlap = std::make_shared<woby::UvOverlapResult>();
    overlap->measurements = measurements; overlap->flags = {1,0,0}; quality.overlap = overlap;
    for (auto metric : {woby::UvQualityMetric::angle,woby::UvQualityMetric::area,woby::UvQualityMetric::anisotropy,
        woby::UvQualityMetric::minStretch,woby::UvQualityMetric::orientation,woby::UvQualityMetric::overlap}) {
        quality.metric = metric; quality.settings.uvMetric = metric;
        const auto reference = woby::uvQualityVertices(mesh,quality);
        const auto faces = woby::uvQualityFaces(mesh,quality);
        REQUIRE(faces.size() == 2);
        for (size_t node = 0; node < 2; ++node) for (size_t corner = 0; corner < 3; ++corner) {
            const auto face = geometry.nodes[node].indexOffset/3;
            CHECK(faces[face].value == reference[node*3+corner].texcoord);
            CHECK(faces[face].normals[corner] == reference[node*3+corner].normal);
        }
    }
    std::stop_source stop; stop.request_stop();
    CHECK_THROWS((void)woby::uvQualityFaces(mesh,quality,stop.get_token()));
    geometry.nodes[0].indexOffset = 1;
    CHECK_THROWS((void)woby::uvQualityFaces(mesh,quality));
}

TEST_CASE("UV presentation copies share immutable measurements and derive normalization without drift")
{
    const auto original = woby::analyzeUvQuality(mappedTriangle(4,4), woby::UvAreaNormalization::perPatch,
        woby::UvQualityMetric::area);
    auto edited = original;
    const auto measurements = original.measurements;
    REQUIRE(measurements);
    CHECK(edited.measurements == measurements);
    CHECK(measurements->triangles[0].areaLog2 == 4);
    CHECK(measurements->triangles[0].minStretch == 4);
    auto settings = edited.settings;
    settings.uvThresholdEnabled = true;
    settings.uvThreshold = 4;
    for (int round = 0; round < 8; ++round) {
        settings.uvNormalization = woby::UvAreaNormalization::absolute;
        woby::updateUvQualitySettings(edited, settings);
        CHECK(edited.measurements == measurements);
        CHECK(woby::uvTriangleQuality(edited, 0).areaLog2 == 4);
        CHECK(edited.statistics.thresholdCount == 0); // Strict threshold.
        settings.uvRangeEnabled = true; settings.uvRangeMinimum = settings.uvRangeMaximum = 4;
        woby::updateUvQualitySettings(edited, settings);
        CHECK(edited.statistics.highlightedCount == 1); // Inclusive range.
        settings.uvNormalization = woby::UvAreaNormalization::perPatch;
        woby::updateUvQualitySettings(edited, settings);
        CHECK(woby::uvTriangleQuality(edited, 0).areaLog2 == 0);
        CHECK(woby::uvTriangleQuality(edited, 0).minStretch == 1);
        CHECK(edited.statistics.highlightedCount == 0);
        settings.uvRangeEnabled = false;
    }
    CHECK(edited.distributions[1].has_value());
    CHECK(edited.distributions[7].has_value());
    CHECK(original.findings.empty());
    CHECK_FALSE(original.settings.uvThresholdEnabled);
    CHECK(measurements->triangles[0].minStretch == 4);
}

TEST_CASE("UV overlap ownership preserves bounded results and disabled findings do not leak")
{
    auto mesh = mappedTriangle();
    appendUvTriangle(mesh, {{{0,0},{1,0},{0,1}}}, true);
    auto quality = overlaps(mesh, woby::UvOverlapScope::selectedPatches);
    const auto overlap = quality.overlap;
    const auto measurements = quality.measurements;
    REQUIRE(overlap);
    CHECK(overlap->measurements == measurements);
    CHECK(overlap->flags == std::vector<uint8_t>{2,2});
    CHECK_FALSE(measurements->triangles[0].crossPatchOverlap);
    auto settings = quality.settings;
    settings.uvMetric = woby::UvQualityMetric::angle; settings.uvOverlapEnabled = false;
    woby::updateUvQualitySettings(quality, settings);
    CHECK_FALSE(quality.overlap);
    CHECK(quality.overlapCache == overlap);
    CHECK(quality.findings.empty());
    CHECK_FALSE(woby::uvTriangleQuality(quality, 0).crossPatchOverlap);
    settings.uvOverlapEnabled = true;
    woby::updateUvQualitySettings(quality, settings);
    CHECK(quality.overlap == overlap);
    CHECK(quality.findings == std::vector<size_t>{0,1});
    woby::inspectUvOverlaps(quality, {}, {0,0});
    const auto bounded = quality.overlap;
    REQUIRE(bounded);
    CHECK(bounded != overlap);
    CHECK(bounded->truncated);
    CHECK(bounded->pairs.empty());
    settings.uvNearCollapse = .5f;
    woby::updateUvQualitySettings(quality, settings);
    CHECK(quality.overlap == bounded);
    // A cached result from another measurement generation is never adopted.
    auto changed = woby::analyzeUvQuality(mesh, settings);
    changed.overlapCache = overlap;
    woby::updateUvQualitySettings(changed, settings);
    REQUIRE(changed.overlap);
    CHECK(changed.overlap != overlap);
    CHECK(changed.overlap->measurements == changed.measurements);
}

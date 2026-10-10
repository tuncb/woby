#include "prototype.h"
#include "obj_mesh.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <random>
#include <string_view>
#include <tuple>

namespace {
using namespace woby;
using Json = nlohmann::json;
void require(bool value, const char* message) { if (!value) { throw std::runtime_error(message); } }
UvTriangleQuality triangle(double x, double y, double size = 1, size_t patch = 0)
{
    UvTriangleQuality t;
    t.uv = {{{x,y},{x+size,y},{x,y+size}}}; t.orientation = 1; t.patch = patch;
    t.surfaceArea = t.uvArea = .5; t.minStretch = t.maxStretch = 1;
    return t;
}
UvQuality quality(std::vector<UvTriangleQuality> triangles)
{
    auto raw = std::make_shared<UvMeasurements>(); raw->triangles = std::move(triangles);
    size_t patches = 1;
    for (size_t i=0; i<raw->triangles.size(); ++i) {
        patches = std::max(patches,raw->triangles[i].patch+1); raw->triangles[i].triangle = i+1;
    }
    raw->patchAreaLog2.resize(patches); raw->patchStretchScale.resize(patches,1);
    UvQuality q; q.measurements = std::move(raw); q.settings.uvOverlapEnabled = true;
    updateUvQualityStatistics(q); return q;
}
void same(const UvQuality& a, const UvQuality& b)
{
    const auto& x = uvOverlapResult(a); const auto& y = uvOverlapResult(b);
    require(x.pairs.size() == y.pairs.size(),"Pair counts differ");
    for (size_t i=0; i<x.pairs.size(); ++i) {
        require(x.pairs[i].first == y.pairs[i].first && x.pairs[i].second == y.pairs[i].second
            && x.pairs[i].crossPatch == y.pairs[i].crossPatch,"Ordered pairs differ");
    }
    require(x.flags == y.flags && x.crossPatchPairs == y.crossPatchPairs
        && x.overlappingTriangles == y.overlappingTriangles && x.truncated == y.truncated,"Overlap results differ");
    require(a.findings == b.findings,"Findings differ");
}
void compare(const UvQuality& q, UvOverlapLimits limits = {10000000,1000000})
{
    auto a = q, b = q; UvBroadPhaseProbe probe;
    inspectUvOverlapsReference(a,{},limits); inspectUvOverlapsTree(b,probe,limits);
    same(a,b);
    b = q; inspectUvOverlapsTree(b,probe,limits,{},std::numeric_limits<size_t>::max(),true); same(a,b);
    if (!uvOverlapResult(a).truncated) {
        b = q; inspectUvOverlapsSweep(b,probe,limits,{},std::numeric_limits<size_t>::max(),true);
        auto x = std::make_shared<UvOverlapResult>(uvOverlapResult(a));
        auto y = std::make_shared<UvOverlapResult>(uvOverlapResult(b));
        const auto order = [](const auto& first, const auto& second) {
            return std::tie(first.first,first.second,first.crossPatch) < std::tie(second.first,second.second,second.crossPatch);
        };
        std::sort(x->pairs.begin(),x->pairs.end(),order); std::sort(y->pairs.begin(),y->pairs.end(),order);
        a.overlap = x; b.overlap = y; same(a,b);
        auto production = q; inspectUvOverlaps(production,{},limits);
        auto actual = std::make_shared<UvOverlapResult>(uvOverlapResult(production));
        std::sort(actual->pairs.begin(),actual->pairs.end(),order); production.overlap = actual; same(a,production);
    }
}
void sweepContracts()
{
    std::vector<UvTriangleQuality> values;
    std::mt19937 rng(3451);
    // Mixed lifetimes exercise deletion with two children and recycled node slots.
    for (size_t i=0; i<300; ++i) {
        values.push_back(triangle(static_cast<double>(rng()%90),static_cast<double>(rng()%30),
            static_cast<double>(rng()%20+1),rng()%3));
    }
    auto q = quality(values); compare(q);
    q.settings.uvOverlapScope = UvOverlapScope::selectedPatches; compare(q);
    auto reference = q; inspectUvOverlapsReference(reference,{}, {10000000,1000000});
    auto expected = uvOverlapResult(reference).pairs;
    const auto lower = [&](size_t i, size_t axis) {
        return std::min({values[i].uv[0][axis],values[i].uv[1][axis],values[i].uv[2][axis]});
    };
    const auto order = [&](const auto& a, const auto& b) {
        return std::tuple{lower(a.second,0),a.second,lower(a.first,1),lower(a.first,0),a.first}
            < std::tuple{lower(b.second,0),b.second,lower(b.first,1),lower(b.first,0),b.first};
    };
    std::sort(expected.begin(),expected.end(),order);
    for (size_t cap : {size_t{0},size_t{1},size_t{13},expected.size(),expected.size()+1}) {
        auto result = q; UvBroadPhaseProbe p;
        inspectUvOverlapsSweep(result,p,{10000000,cap},{},std::numeric_limits<size_t>::max(),true);
        const auto& overlap = uvOverlapResult(result);
        require(overlap.pairs.size() == std::min(cap,expected.size()),"Sweep pair cap differs");
        require(overlap.truncated == (cap<expected.size()),"Sweep pair truncation differs");
        for (size_t i=0; i<overlap.pairs.size(); ++i) {
            require(overlap.pairs[i].first == expected[i].first && overlap.pairs[i].second == expected[i].second,
                "Sweep prefix differs from independent event order");
        }
        std::vector<uint8_t> flags(values.size());
        for (const auto& pair : overlap.pairs) {
            const uint8_t flag = pair.crossPatch ? uint8_t{2} : uint8_t{1};
            flags[pair.first] |= flag; flags[pair.second] |= flag;
        }
        require(overlap.flags == flags,"Sweep flags exceed retained prefix");
        auto repeat = q; inspectUvOverlaps(repeat,{}, {10000000,cap}); same(result,repeat);
    }
    q = quality(std::vector<UvTriangleQuality>(40,triangle(0,0)));
    for (size_t cap : {size_t{0},size_t{1},size_t{13},size_t{780},size_t{781}}) {
        auto result = q; UvBroadPhaseProbe p; inspectUvOverlapsSweep(result,p,{cap,1000});
        require(uvOverlapResult(result).candidates == std::min(cap,size_t{780}),"Sweep candidate cap differs");
        require(uvOverlapResult(result).truncated == (cap<780),"Sweep candidate truncation differs");
    }
    for (size_t work : {size_t{0},size_t{1},size_t{10},size_t{100}}) {
        auto result = q; UvBroadPhaseProbe p; inspectUvOverlapsSweep(result,p,{}, {},work);
        require(p.workTruncated && uvOverlapResult(result).truncated && p.nodeTests+p.maintenanceTests == work,
            "Sweep work budget exceeded");
    }
    UvBroadPhaseProbe p;
    q.settings.uvOverlapEnabled = false; inspectUvOverlapsSweep(q,p); require(!q.overlap,"Disabled sweep published");
    q.settings.uvOverlapEnabled = true;
    std::stop_source stop; stop.request_stop(); bool threw = false;
    try { inspectUvOverlapsSweep(q,p,{},stop.get_token()); } catch (const std::runtime_error&) { threw = true; }
    require(threw && !q.overlap,"Canceled sweep published");
}
void selfTest()
{
    sweepContracts();
    compare(quality({})); compare(quality({triangle(0,0)}));
    std::mt19937 rng(457);
    for (size_t trial=0; trial<30; ++trial) {
        std::vector<UvTriangleQuality> values;
        for (size_t i=0; i<100; ++i) {
            auto t = triangle(static_cast<double>(rng()%40)-20,static_cast<double>(rng()%40)-20,
                static_cast<double>(rng()%6+1),rng()%4);
            if (i%7 == 0) { std::swap(t.uv[1],t.uv[2]); t.orientation = -1; }
            values.push_back(t);
        }
        auto q = quality(std::move(values)); compare(q);
        q.settings.uvOverlapScope = UvOverlapScope::selectedPatches; compare(q);
        compare(q,{10000000,3});
    }
    for (double scale : {1e-100,1.0,1e100}) {
        auto a = triangle(0,0,scale); auto b = a;
        b.uv = {{{scale,scale},{0,scale},{scale,0}}}; // Shared diagonal only.
        auto c = triangle(scale/4,scale/4,scale/4); // Containment.
        auto d = triangle(scale,0,scale); // Shared vertex only.
        auto invalid = a; invalid.missing = true;
        auto q = quality({a,b,c,d,invalid}); compare(q);
        UvBroadPhaseProbe p; inspectUvOverlapsTree(q,p);
        require(uvOverlapResult(q).pairs.size() == 1,"Contact or containment classification changed");
    }
    auto dense = quality(std::vector<UvTriangleQuality>(40,triangle(0,0)));
    compare(dense,{0,100}); compare(dense,{13,100}); compare(dense,{100,0}); compare(dense,{10000,13});
    auto a = dense, b = dense; UvBroadPhaseProbe p;
    inspectUvOverlapsTree(a,p); inspectUvOverlapsTree(b,p); same(a,b);
    inspectUvOverlapsTree(b,p,{}, {},0);
    require(p.workTruncated && uvOverlapResult(b).truncated && uvOverlapResult(b).pairs.empty(),"Work limit failed");
    b.settings.uvOverlapEnabled = false; inspectUvOverlapsTree(b,p);
    require(!b.overlap,"Disabled overlap published results");
    b = dense; std::stop_source stop; stop.request_stop(); bool threw = false;
    try { inspectUvOverlapsTree(b,p,{},stop.get_token()); } catch (const std::runtime_error&) { threw = true; }
    require(threw && !b.overlap,"Canceled tree published results");
    for (bool transpose : {false,true}) {
        std::vector<UvTriangleQuality> rows;
        for (size_t i=0; i<2001; ++i) { rows.push_back(triangle(0,static_cast<double>(i)*2)); }
        if (transpose) { for (auto& t : rows) { for (auto& v : t.uv) { std::swap(v[0],v[1]); } t.orientation = -1; } }
        b = quality(std::move(rows)); inspectUvOverlapsTree(b,p);
        require(!uvOverlapResult(b).truncated && uvOverlapResult(b).candidates == 0,"Disjoint rows failed");
        inspectUvOverlapsSweep(b,p,{}, {},std::numeric_limits<size_t>::max(),true);
        require(!uvOverlapResult(b).truncated && uvOverlapResult(b).candidates == 0,"Sweep disjoint rows failed");
    }
}
Json run(const UvQuality& source, int algorithm, UvOverlapLimits limits, size_t workLimit)
{
    auto q = source; UvBroadPhaseProbe probe;
    const auto start = std::chrono::steady_clock::now();
    if (algorithm == 4) { inspectUvOverlaps(q,{},limits); }
    else if (algorithm == 3) { inspectUvOverlapsSweep(q,probe,limits,{},workLimit); }
    else if (algorithm) { inspectUvOverlapsTree(q,probe,limits,{},workLimit,algorithm == 2); }
    else { inspectUvOverlapsReference(q,{},limits); }
    const double ms = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    const auto& r = uvOverlapResult(q);
    Json result{{"ms",ms},{"candidates",r.candidates},{"pairs",r.pairs.size()},
        {"affected",r.overlappingTriangles},{"cross_patch_pairs",r.crossPatchPairs},{"truncated",r.truncated}};
    if (algorithm == 4) {
        result["search_work"] = r.searchWork; result["search_work_limit"] = r.searchWorkLimit;
        result["truncation_reason"] = uvOverlapTruncationKey(r.truncation);
    } else if (algorithm) {
        result["build_ms"] = probe.buildMs; result["query_ms"] = probe.queryMs;
        result["node_tests"] = probe.nodeTests; result["leaf_tests"] = probe.leafTests;
        result["scratch_bytes"] = probe.scratchBytes; result["work_truncated"] = probe.workTruncated;
        if (algorithm == 3) { result["maintenance_tests"] = probe.maintenanceTests; result["peak_active"] = probe.peakActive; }
    }
    return result;
}
Json measure(const std::string& name, const UvQuality& q, int samples, UvOverlapLimits limits = {})
{
    std::cerr << name << '\n';
    // Experimental linear allowance for tree navigation, separate from exact candidates.
    const size_t workLimit = uvMeasurements(q).triangles.size()*64+limits.candidates*4;
    Json result{{"name",name},{"triangles",uvMeasurements(q).triangles.size()},
        {"valid",q.statistics.count},{"scope",q.settings.uvOverlapScope == UvOverlapScope::perPatch ? "per_patch" : "selected_patches"},
        {"candidate_limit",limits.candidates},{"pair_limit",limits.pairs},{"tree_work_limit",workLimit},
        {"samples",Json::array()}};
    for (int i=0; i<samples; ++i) {
        Json sample;
        const std::array<const char*,5> names{"scan","tree","lazy_tree","sweep","production"};
        for (int offset=0; offset<5; ++offset) {
            const int algorithm = (i+offset)%5;
            sample[names[static_cast<size_t>(algorithm)]] = run(q,algorithm,limits,workLimit);
        }
        result["samples"].push_back(std::move(sample));
    }
    return result;
}
Json synthetic(int samples)
{
    Json out = Json::array();
    for (size_t count : {size_t{2001},size_t{10000}}) {
        std::vector<UvTriangleQuality> rows;
        for (size_t i=0; i<count; ++i) { rows.push_back(triangle(0,static_cast<double>(i)*2)); }
        std::mt19937 rng(941); std::shuffle(rows.begin(),rows.end(),rng);
        out.push_back(measure("shuffled_rows_"+std::to_string(count),quality(rows),samples));
        for (auto& t : rows) { for (auto& p : t.uv) { std::swap(p[0],p[1]); } t.orientation = -1; }
        out.push_back(measure("shuffled_columns_"+std::to_string(count),quality(std::move(rows)),samples));
    }
    std::vector<UvTriangleQuality> grid;
    for (size_t i=0; i<10000; ++i) { grid.push_back(triangle(static_cast<double>(i%100)*2,static_cast<double>(i/100)*2)); }
    out.push_back(measure("disjoint_grid_10000",quality(std::move(grid)),samples));
    out.push_back(measure("coincident_1000",quality(std::vector<UvTriangleQuality>(1000,triangle(0,0))),samples));
    std::vector<UvTriangleQuality> diagonal;
    for (size_t i=0; i<512; ++i) {
        auto t = triangle(0,0); const double y = static_cast<double>(i)*.01;
        t.uv = {{{0,y},{100,100+y},{0,y+.001}}}; diagonal.push_back(t);
    }
    out.push_back(measure("disjoint_diagonal_512",quality(std::move(diagonal)),samples));
    return out;
}
}
int main(int argc, char** argv)
{
    try {
        selfTest();
        if (argc == 2 && std::string_view(argv[1]) == "--self-test") { std::cout << "UV broad-phase contracts passed\n"; return 0; }
        require(argc == 3,"Usage: woby_uv_overlap_probe model.obj|--synthetic samples");
        const int samples = std::stoi(argv[2]); require(samples > 0 && samples <= 5,"Invalid sample count");
        Json result;
        if (std::string_view(argv[1]) == "--synthetic") { result = synthetic(samples); }
        else {
            std::cerr << "Loading " << argv[1] << '\n';
            auto q = analyzeUvQuality(loadObjMesh(argv[1]),UvAreaNormalization::perPatch,UvQualityMetric::angle);
            q.settings.uvOverlapEnabled = true;
            result = Json::array();
            result.push_back(measure("model_per_patch",q,samples));
            q.settings.uvOverlapScope = UvOverlapScope::selectedPatches;
            result.push_back(measure("model_selected_patches",q,samples));
        }
        std::cout << result.dump(2) << '\n';
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}

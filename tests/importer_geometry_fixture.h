#pragma once
#include "woby/importer.h"
#include <array>
#include <cmath>

// Self-contained ABI 4 sample, shared by direct validation tests and the DLL.
struct ImportGeometryData {
    WobyImportVertex vertices[4] = {{{0,0,0},{},{}},{{1,0,0},{},{}},{{0,1,0},{},{}},{{0,0,0},{},{}}};
    uint32_t triangles[3] = {0,1,2}, segments[2] = {1,2}, dots[2] = {0,3};
    uint64_t ids[4] = {0,UINT64_MAX,2,0};
    WobyImportGroup pointGroup{"landmarks",0,2,WOBY_IMPORT_GROUP_HAS_COLOR | WOBY_IMPORT_GROUP_INITIALLY_HIDDEN,{1,0,0,.5f}};
    WobyImportLines lines{sizeof(WobyImportLines),segments,2,nullptr,0};
    WobyImportPoints points{sizeof(WobyImportPoints),dots,2,&pointGroup,1};
    WobyImportPointIds pointIds{sizeof(WobyImportPointIds),ids,4};
    WobyImportControlPoint arc[3] = {{{1,0,0},1,{},{}},{{1,1,0},std::sqrt(.5),{},{}},{{0,1,0},1,{},{}}};
    WobyImportControlPoint surface[4] = {
        {{0,0,1},1,{0,0},{0,0,-2}}, {{1,0,1},1,{1,0},{0,0,-2}},
        {{0,1,1},1,{0,1},{0,0,-2}}, {{1,1,1},1,{1,1},{0,0,-2}}};
    WobyImportControlPoint boundary[5] = {
        {{.25,.25,0},1,{},{}},{{.75,.25,0},1,{},{}},{{.75,.75,0},1,{},{}},
        {{.25,.75,0},1,{},{}},{{.25,.25,0},1,{},{}}};
    double quadratic[6] = {0,0,0,1,1,1}, linear[4] = {0,0,1,1}, polygon[7] = {0,0,1,2,3,4,4};
    WobyImportSpline trim{};
    WobyImportTrimSegment trimSegment{0,{4,0}};
    WobyImportTrimLoop hole{&trimSegment,1};
    WobyImportTrimRegion region{{nullptr,0},&hole,1};
    WobyImportFreeformPatch patches[2]{};
    WobyImportFreeform freeform{sizeof(WobyImportFreeform),patches,2,&trim,1};
    WobyImportHierarchyNode nodes[6] = {
        {"Assembly",WOBY_IMPORT_NO_PARENT,WOBY_IMPORT_NO_GROUP},
        {"Triangles",0,0},{"Segments",0,1},{"Points",0,2},{"Arc",0,3},{"Panel",0,4}};
    WobyImportHierarchy hierarchy{sizeof(WobyImportHierarchy),nodes,6};
    WobyImportResult result{};
    ImportGeometryData() {
        patches[0].group = {"arc",0,0,WOBY_IMPORT_GROUP_HAS_COLOR,{0,1,0,1}};
        patches[0].spline = {sizeof(WobyImportSpline),WOBY_IMPORT_SPLINE_CURVE,0,2,0,3,1,arc,3,quadratic,6,nullptr,0,{0,1},{0,0}};
        patches[1].group = {"panel",0,0,0,{}};
        patches[1].spline = {sizeof(WobyImportSpline),WOBY_IMPORT_SPLINE_SURFACE,WOBY_IMPORT_HAS_NORMALS | WOBY_IMPORT_HAS_TEXCOORDS,
            1,1,2,2,surface,4,linear,4,linear,4,{0,1},{0,1}};
        trim = {sizeof(WobyImportSpline),WOBY_IMPORT_SPLINE_CURVE,0,1,0,5,1,boundary,5,polygon,7,nullptr,0,{0,4},{0,0}};
        patches[1].regions = &region; patches[1].region_count = 1;
        result.struct_size = sizeof(result); result.vertices = vertices; result.vertex_count = 4;
        result.indices = triangles; result.index_count = 3;
    }
};

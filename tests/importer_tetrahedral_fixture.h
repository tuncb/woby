#pragma once
#include "woby/importer.h"

struct ImportTetrahedralData {
    WobyImportVertex vertices[14] = {
        {{0,0,0},{},{}},{{1,0,0},{},{}},{{0,1,0},{},{}},{{0,0,1},{},{}},
        {{.5,0,0},{},{}},{{.5,.5,0},{},{}},{{0,.5,0},{},{}},
        {{0,0,.5},{},{}},{{.5,0,.5},{},{}},{{0,.5,.5},{},{}},
        {{0,0,-1},{},{}},{{0,0,-.5},{},{}},{{0,.5,-.5},{},{}},{{.5,0,-.5},{},{}}};
    WobyImportTetrahedron cells[2] = {{10,{0,1,2,3,4,5,6,7,8,9}},{10,{0,2,1,10,6,5,4,11,12,13}}};
    WobyImportGroup groups[2] = {{"upper",0,1,WOBY_IMPORT_GROUP_HAS_COLOR,{1,0,0,.5f}},
        {"lower",1,1,WOBY_IMPORT_GROUP_INITIALLY_HIDDEN,{}}};
    WobyImportTetrahedra tetrahedra{sizeof(WobyImportTetrahedra),cells,2,groups,2};
    WobyImportHierarchyNode nodes[3] = {{"Assembly",WOBY_IMPORT_NO_PARENT,WOBY_IMPORT_NO_GROUP},{"Upper cell",0,0},{"Lower cell",0,1}};
    WobyImportHierarchy hierarchy{sizeof(WobyImportHierarchy),nodes,3};
    WobyImportResult result{sizeof(WobyImportResult),0,vertices,14,nullptr,0,nullptr,0,nullptr,nullptr};
};

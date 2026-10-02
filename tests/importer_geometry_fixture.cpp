#include "importer_geometry_fixture.h"
#include <cstring>

namespace {
int outstanding = 0;
struct Data {
    ImportGeometryData geometry;
    bool pointsOnly = false, freeformOnly = false, absent = false;
};
uint32_t WOBY_IMPORT_CALL importFile(const WobyImportRequest* request, WobyImportResult* result)
{
    if (outstanding) { result->error = "Previous geometry result was not released exactly once."; return WOBY_IMPORT_ERROR; }
    try {
        auto* data = new Data;
        ++outstanding;
        auto& g = data->geometry;
        *result = g.result; result->user_data = data;
        data->pointsOnly = std::strstr(request->path,"pointsonly") != nullptr;
        data->freeformOnly = std::strstr(request->path,"freeformonly") != nullptr;
        data->absent = std::strstr(request->path,"absent") != nullptr;
        if (data->pointsOnly || data->freeformOnly) { result->indices = nullptr; result->index_count = 0; }
        if (data->freeformOnly) { result->vertices = nullptr; result->vertex_count = 0; }
        if (std::strstr(request->path,"invalid_point")) { g.dots[0] = 99; }
        if (std::strstr(request->path,"invalid_spline")) { g.arc[0].weight = 0; }
        if (std::strstr(request->path,"invalid_trim")) { g.trimSegment.curve_index = 1; }
        if (std::strstr(request->path,"failure")) { result->error = "Geometry fixture failure."; return WOBY_IMPORT_ERROR; }
        request->report_progress(request->context,.5f);
        return request->is_canceled(request->context) ? WOBY_IMPORT_CANCELED : WOBY_IMPORT_OK;
    } catch (...) { result->error = "Geometry fixture allocation failed."; return WOBY_IMPORT_ERROR; }
}
void WOBY_IMPORT_CALL releaseResult(WobyImportResult* result)
{
    if (result->user_data) { delete static_cast<Data*>(result->user_data); result->user_data = nullptr; --outstanding; }
}
const WobyImportHierarchy* WOBY_IMPORT_CALL getHierarchy(const WobyImportResult* result) {
    const auto& d = *static_cast<const Data*>(result->user_data);
    return d.pointsOnly || d.freeformOnly || d.absent ? nullptr : &d.geometry.hierarchy;
}
const WobyImportLines* WOBY_IMPORT_CALL getLines(const WobyImportResult* result) {
    const auto& d = *static_cast<const Data*>(result->user_data);
    return d.pointsOnly || d.freeformOnly || d.absent ? nullptr : &d.geometry.lines;
}
const WobyImportPointIds* WOBY_IMPORT_CALL getPointIds(const WobyImportResult* result) {
    const auto& d = *static_cast<const Data*>(result->user_data);
    return d.freeformOnly || d.absent ? nullptr : &d.geometry.pointIds;
}
const WobyImportPoints* WOBY_IMPORT_CALL getPoints(const WobyImportResult* result) {
    const auto& d = *static_cast<const Data*>(result->user_data);
    return d.freeformOnly || d.absent ? nullptr : &d.geometry.points;
}
const WobyImportFreeform* WOBY_IMPORT_CALL getFreeform(const WobyImportResult* result) {
    const auto& d = *static_cast<const Data*>(result->user_data);
    return d.pointsOnly || d.absent ? nullptr : &d.geometry.freeform;
}
const WobyImporterApiWithGeometry api{
    {{{{sizeof(WobyImporterApiWithGeometry),WOBY_IMPORTER_ABI_VERSION,
        "org.woby.test.geometry","Geometry fixture importer","1","wgeom",importFile,releaseResult},getHierarchy},getLines},getPointIds},
    getPoints,getFreeform};
}
extern "C" WOBY_IMPORT_EXPORT const WobyImporterApi* WOBY_IMPORT_CALL woby_get_importer_api(uint32_t version)
{
    return version == WOBY_IMPORTER_ABI_VERSION ? &api.base.base.base.base : nullptr;
}

#include "woby/importer.h"

#include <cstring>
#include <cstddef>
#include <new>
#include <string>
#include <type_traits>

namespace {

static_assert(std::is_standard_layout_v<WobyImporterApiWithHierarchy>);
static_assert(offsetof(WobyImporterApiWithHierarchy, base) == 0);

const WobyImportVertex vertices[] = {
    {{0, 0, 0}, {}, {0, 0}}, {{1, 0, 0}, {}, {1, 0}},
    {{0.5, 1, 0}, {}, {0.5f, 1}}, {{0.5, 0.4, 1}, {}, {0.5f, 0.4f}},
    {{2, 0, 0}, {}, {0, 0}}, {{3, 0, 0}, {}, {1, 0}},
    {{2.5, 1, 0}, {}, {0.5f, 1}}, {{2.5, 0.4, 1}, {}, {0.5f, 0.4f}},
};
const uint32_t indices[] = {0, 2, 1, 0, 1, 3, 1, 2, 3, 2, 0, 3,
    4, 6, 5, 4, 5, 7, 5, 6, 7, 6, 4, 7};

struct ResultData {
    std::string assembly = "Assembly";
    WobyImportGroup groups[4] = {
        {"solid-a/patch-1", 0, 6, WOBY_IMPORT_GROUP_HAS_COLOR, {0.9f, 0.3f, 0.2f, 1}},
        {"solid-a/patch-2", 6, 6, WOBY_IMPORT_GROUP_INITIALLY_HIDDEN, {}},
        {"solid-b/patch-1", 12, 6, WOBY_IMPORT_GROUP_HAS_COLOR, {0.2f, 0.5f, 0.9f, 1}},
        {"solid-b/patch-2", 18, 6, 0, {}},
    };
    WobyImportHierarchyNode nodes[7] = {
        {assembly.c_str(), WOBY_IMPORT_NO_PARENT, WOBY_IMPORT_NO_GROUP},
        {"Solid A", 0, WOBY_IMPORT_NO_GROUP}, {"Patch 1", 1, 0}, {"Patch 2", 1, 1},
        {"Solid B", 0, WOBY_IMPORT_NO_GROUP}, {"Patch 1", 4, 2}, {"Patch 2", 4, 3},
    };
    WobyImportHierarchy hierarchy{sizeof(WobyImportHierarchy), nodes, 7};
    bool flat = false;
};

int outstanding = 0;

uint32_t WOBY_IMPORT_CALL readFixture(const WobyImportRequest* request, WobyImportResult* result)
{
    if (outstanding != 0) {
        result->error = "Previous hierarchy result was not released exactly once.";
        return WOBY_IMPORT_ERROR;
    }
    try {
        auto* data = new ResultData;
        ++outstanding;
        result->user_data = data;
        result->vertices = vertices;
        result->vertex_count = 8;
        result->indices = indices;
        result->index_count = 24;
        result->groups = data->groups;
        result->group_count = 4;
        result->flags = WOBY_IMPORT_HAS_TEXCOORDS;
        data->flat = std::strstr(request->path, "flat") != nullptr;
        if (std::strstr(request->path, "invalid")) { data->nodes[0].parent_index = 0; }
        if (std::strstr(request->path, "hidden")) {
            for (auto& group : data->groups) { group.flags |= WOBY_IMPORT_GROUP_INITIALLY_HIDDEN; }
        }
        if (std::strstr(request->path, "failure")) {
            result->error = "Hierarchy fixture parse failure.";
            return WOBY_IMPORT_ERROR;
        }
        request->report_progress(request->context, 0.5f);
        return request->is_canceled(request->context) ? WOBY_IMPORT_CANCELED : WOBY_IMPORT_OK;
    } catch (...) {
        result->error = "Hierarchy fixture allocation failed.";
        return WOBY_IMPORT_ERROR;
    }
}

const WobyImportHierarchy* WOBY_IMPORT_CALL hierarchy(const WobyImportResult* result)
{
    const auto& data = *static_cast<const ResultData*>(result->user_data);
    return data.flat ? nullptr : &data.hierarchy;
}

void WOBY_IMPORT_CALL releaseFixture(WobyImportResult* result)
{
    if (result->user_data) {
        delete static_cast<ResultData*>(result->user_data);
        --outstanding;
        result->user_data = nullptr;
    }
}

const WobyImporterApiWithHierarchy api = {
    {sizeof(WobyImporterApiWithHierarchy), WOBY_IMPORTER_ABI_VERSION,
        "org.woby.test.hierarchy", "Hierarchy fixture importer", "1", "whier", readFixture, releaseFixture},
    hierarchy,
};

} // namespace

extern "C" WOBY_IMPORT_EXPORT const WobyImporterApi* WOBY_IMPORT_CALL woby_get_importer_api(uint32_t version)
{
    return version == WOBY_IMPORTER_ABI_VERSION ? &api.base : nullptr;
}

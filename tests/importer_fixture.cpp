#include "woby/importer.h"

#include <cstring>
#include <limits>

namespace {

WobyImportVertex vertices[] = {
    {{0, 0, 0}, {0, 0, 1}, {0, 0}},
    {{1, 0, 0}, {0, 0, 1}, {1, 0}},
    {{0, 1, 0}, {0, 0, 1}, {0, 1}},
    {{9, 9, 9}, {0, 0, 1}, {0, 0}}, // Deliberately unused.
};
uint32_t indices[] = {0, 1, 2};
WobyImportGroup group = {"triangle", 0u, 3u, 0u, {}};
int outstanding = 0;

uint32_t WOBY_IMPORT_CALL readFixture(const WobyImportRequest* request, WobyImportResult* result)
{
    if (outstanding != 0) {
        result->error = "Previous result was not released.";
        return WOBY_IMPORT_ERROR;
    }
    ++outstanding;
    result->user_data = &outstanding;
    result->vertices = vertices;
    result->vertex_count = 4u;
    result->indices = indices;
    result->index_count = 3u;
    result->groups = &group;
    result->group_count = 1u;
    indices[2] = 2u;
    vertices[0].position[0] = 0.0f;
    group.index_offset = 0u;
    group.name = "triangle";
    group.flags = 0u;
    for (auto& component : group.color) { component = 0.0f; }
    if (std::strstr(request->path, "colored")) {
        group.flags |= WOBY_IMPORT_GROUP_HAS_COLOR;
        group.color[0] = 0.2f;
        group.color[1] = 0.4f;
        group.color[2] = 0.6f;
        group.color[3] = 0.25f;
    }
    if (std::strstr(request->path, "hidden")) { group.flags |= WOBY_IMPORT_GROUP_INITIALLY_HIDDEN; }
    if (std::strstr(request->path, "invalid_flags")) { group.flags = 4u; }
    if (std::strstr(request->path, "invalid_color")) {
        group.flags |= WOBY_IMPORT_GROUP_HAS_COLOR;
        group.color[3] = std::numeric_limits<float>::quiet_NaN();
    }
    if (std::strstr(request->path, "invalid_index")) { indices[2] = 100u; }
    if (std::strstr(request->path, "nan_position")) { vertices[0].position[0] = std::numeric_limits<float>::quiet_NaN(); }
    if (std::strstr(request->path, "invalid_group")) { group.index_offset = 3u; }
    if (std::strstr(request->path, "changed_group")) { group.name = "changed"; }
    if (std::strstr(request->path, "null_buffer")) { result->vertices = nullptr; }
    if (std::strstr(request->path, "huge_buffer")) { result->vertex_count = UINT32_MAX; }
    if (std::strstr(request->path, "short_result")) { result->struct_size = 0u; }
    if (std::strstr(request->path, "failure")) {
        result->error = "Fixture parse failure.";
        return WOBY_IMPORT_ERROR;
    }
    request->report_progress(request->context, 0.5f);
    if (request->is_canceled(request->context)) { return WOBY_IMPORT_CANCELED; }
    request->report_progress(request->context, 1.0f);
    return WOBY_IMPORT_OK;
}

void WOBY_IMPORT_CALL releaseFixture(WobyImportResult* result)
{
    if (result->user_data) { --outstanding; }
    // Successful imports must own their metadata before this callback returns.
    group.flags = 0u;
    for (auto& component : group.color) { component = 0.0f; }
}

const WobyImporterApi api = {
    sizeof(WobyImporterApi),
#ifdef WOBY_TEST_BAD_ABI
    2u, // Previous ABI must be rejected by the ABI 3 host.
#else
    WOBY_IMPORTER_ABI_VERSION,
#endif
#ifdef WOBY_TEST_CONFLICT
    "org.woby.conflict", "Conflicting importer", "1", "wtest",
#elif defined(WOBY_TEST_RESERVED)
    "org.woby.reserved", "Reserved importer", "1", "OBJ",
#else
    "org.woby.test", "Fixture importer", "1", "wtest;wt2",
#endif
    readFixture, releaseFixture,
};

} // namespace

extern "C" WOBY_IMPORT_EXPORT const WobyImporterApi* WOBY_IMPORT_CALL woby_get_importer_api(uint32_t)
{
    return &api;
}

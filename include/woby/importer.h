#ifndef WOBY_IMPORTER_H
#define WOBY_IMPORTER_H

#include <stdint.h>

#if defined(_WIN32)
#define WOBY_IMPORT_CALL __cdecl
#define WOBY_IMPORT_EXPORT __declspec(dllexport)
#else
#define WOBY_IMPORT_CALL
#define WOBY_IMPORT_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define WOBY_IMPORTER_ABI_VERSION 4u
#define WOBY_IMPORT_OK 0u
#define WOBY_IMPORT_ERROR 1u
#define WOBY_IMPORT_CANCELED 2u
#define WOBY_IMPORT_HAS_NORMALS 1u
#define WOBY_IMPORT_HAS_TEXCOORDS 2u
#define WOBY_IMPORT_GROUP_HAS_COLOR 1u
#define WOBY_IMPORT_GROUP_INITIALLY_HIDDEN 2u
#define WOBY_IMPORT_NO_PARENT UINT32_MAX
#define WOBY_IMPORT_NO_GROUP UINT32_MAX
#define WOBY_IMPORT_MAX_HIERARCHY_DEPTH 128u

/* All strings are null-terminated UTF-8. No exceptions may cross this ABI.
 * The plugin owns all returned memory until release_result. See doc/importers.md. */
typedef struct WobyImportVertex {
    double position[3]; // Original coordinates; Woby chooses the working origin.
    float normal[3];
    float texcoord[2];
} WobyImportVertex;

typedef struct WobyImportGroup {
    const char* name;
    uint32_t index_offset;
    uint32_t index_count;
    /* Zero flags uses Woby's palette and initially shows this group. */
    uint32_t flags;
    /* Optional RGBA in [0, 1], used only with WOBY_IMPORT_GROUP_HAS_COLOR.
     * Alpha initializes group opacity. These are defaults; saved scenes override them. */
    float color[4];
} WobyImportGroup;

typedef struct WobyImportRequest {
    uint32_t struct_size;
    const char* path;
    void* context;
    uint32_t (WOBY_IMPORT_CALL *is_canceled)(void* context);
    void (WOBY_IMPORT_CALL *report_progress)(void* context, float fraction);
} WobyImportRequest;

typedef struct WobyImportResult {
    uint32_t struct_size;
    uint32_t flags;
    const WobyImportVertex* vertices;
    uint32_t vertex_count;
    const uint32_t* indices;
    uint32_t index_count;
    const WobyImportGroup* groups;
    uint32_t group_count;
    const char* error;
    void* user_data;
} WobyImportResult;

typedef struct WobyImporterApi {
    uint32_t struct_size;
    uint32_t abi_version;
    const char* id;
    const char* name;
    const char* version;
    /* Semicolon-separated extensions without dots, e.g. "ply;off". */
    const char* extensions;
    uint32_t (WOBY_IMPORT_CALL *import_file)(const WobyImportRequest*, WobyImportResult*);
    /* Called exactly once after every import_file call, including failure/cancel. */
    void (WOBY_IMPORT_CALL *release_result)(WobyImportResult*);
} WobyImporterApi;

/* Optional size-gated extension. Base API and result layouts remain unchanged.
 * Nodes are ordered parent before child. Only containers (NO_GROUP) can parent
 * other nodes; every mesh group must occur exactly once. Names are display labels
 * and may repeat; WobyImportGroup.name remains the stable, unique saved identity. */
typedef struct WobyImportHierarchyNode {
    const char* name;
    uint32_t parent_index;
    uint32_t group_index;
} WobyImportHierarchyNode;

typedef struct WobyImportHierarchy {
    uint32_t struct_size;
    const WobyImportHierarchyNode* nodes;
    uint32_t node_count;
} WobyImportHierarchy;

typedef struct WobyImporterApiWithHierarchy {
    /* Set base.struct_size to sizeof(WobyImporterApiWithHierarchy). Return &base
     * from woby_get_importer_api. The host checks struct_size before reading it. */
    WobyImporterApi base;
    /* Optional; called after a successful import_file. NULL means a flat import.
     * All metadata is borrowed until release_result. Do not retain the request
     * or perform further import work here. No exceptions may cross this ABI. */
    const WobyImportHierarchy* (WOBY_IMPORT_CALL *get_hierarchy)(const WobyImportResult*);
} WobyImporterApiWithHierarchy;

/* Optional segment pairs referencing the result's shared vertex table. Groups
 * partition indices in pairs and use the triangle group's color/visibility flags.
 * A nonempty buffer with zero groups creates one group named "Lines".
 * NULL from get_lines means absent. */
typedef struct WobyImportLines {
    uint32_t struct_size;
    const uint32_t* indices;
    uint32_t index_count;
    const WobyImportGroup* groups;
    uint32_t group_count;
} WobyImportLines;

typedef struct WobyImporterApiWithLines {
    /* Set base.base.struct_size to sizeof(WobyImporterApiWithLines) and return
     * &base.base. Base API and hierarchy tables remain unchanged. Hierarchy
     * group indexes address triangle groups first, then line groups (including
     * generated defaults). A line-only result has no triangle groups. */
    WobyImporterApiWithHierarchy base;
    /* Borrowed metadata prepared during import_file; lifetime until release_result. */
    const WobyImportLines* (WOBY_IMPORT_CALL *get_lines)(const WobyImportResult*);
} WobyImporterApiWithLines;

/* Optional source identity for duplicate-point inspection only. Supply one ID
 * per result vertex, including unused and line-only vertices. IDs are opaque,
 * file-local uint64 values (including 0 and UINT64_MAX). Vertices sharing an ID
 * must have exactly equal original positions; signed zeros compare equal.
 * Distinct IDs at equal positions remain duplicates. Normals and UVs may differ.
 * These IDs never weld topology or change duplicate-triangle inspection. */
typedef struct WobyImportPointIds {
    uint32_t struct_size;
    const uint64_t* ids;
    uint32_t vertex_count;
} WobyImportPointIds;

typedef struct WobyImporterApiWithPointIds {
    /* Set base.base.base.struct_size to sizeof(WobyImporterApiWithPointIds) and
     * return &base.base.base. Unused hierarchy/line callbacks may be NULL. */
    WobyImporterApiWithLines base;
    /* Optional; NULL callback/result preserves per-vertex duplicate counting.
     * Metadata is prepared in import_file and borrowed until release_result. */
    const WobyImportPointIds* (WOBY_IMPORT_CALL *get_point_ids)(const WobyImportResult*);
} WobyImporterApiWithPointIds;

/* Explicit points (including point clouds) reference the shared vertex table.
 * Groups partition indices individually. Zero groups generates "Points".
 * These are drawable points, independent of the source-identity get_point_ids. */
typedef struct WobyImportPoints {
    uint32_t struct_size;
    const uint32_t* indices;
    uint32_t index_count;
    const WobyImportGroup* groups;
    uint32_t group_count;
} WobyImportPoints;

#define WOBY_IMPORT_SPLINE_CURVE 0u
#define WOBY_IMPORT_SPLINE_SURFACE 1u

/* Euclidean original-file coordinates, NOT multiplied by weight. Use weight=1
 * for nonrational geometry. UVs use OBJ's convention (host flips V for display).
 * normal/texcoord are used only when the corresponding spline flag is set. */
typedef struct WobyImportControlPoint {
    double position[3];
    double weight;
    double texcoord[2];
    double normal[3];
} WobyImportControlPoint;

/* Canonical B-spline representation, also covering Bezier and NURBS.
 * Bezier: count=degree+1, knots=[a repeated degree+1, b repeated degree+1].
 * Tensor-product controls are U-fastest: controls[v * count_u + u].
 * All degrees are 1..8. Curves use degree_v=0, count_v=1, knot_count_v=0.
 * Weights must be positive and finite; flags are HAS_NORMALS/HAS_TEXCOORDS.
 * A knot vector has count+degree+1 entries, finite and nondecreasing.
 * Domains are increasing subranges of [knots[degree], knots[count]]. */
typedef struct WobyImportSpline {
    uint32_t struct_size;
    uint32_t kind;
    uint32_t flags;
    uint32_t degree_u, degree_v;
    uint32_t count_u, count_v;
    const WobyImportControlPoint* controls;
    uint32_t control_count;
    const double* knots_u;
    uint32_t knot_count_u;
    const double* knots_v;
    uint32_t knot_count_v;
    double domain_u[2], domain_v[2];
} WobyImportSpline;

/* Trim curves are defined separately in WobyImportFreeform.trim_curves using
 * position=(u,v,0), kind=CURVE, flags=0 and at most 4096 controls per curve.
 * They do not create drawable groups.
 * Intervals can run forwards or backwards inside the trim curve's domain. */
typedef struct WobyImportTrimSegment {
    uint32_t curve_index;
    double interval[2];
} WobyImportTrimSegment;

typedef struct WobyImportTrimLoop {
    const WobyImportTrimSegment* segments;
    uint32_t segment_count;
} WobyImportTrimLoop;

typedef struct WobyImportTrimRegion {
    /* Empty outer means the surface's parameter rectangle. Holes must be nonempty.
     * Loops must be closed and simple; boundaries must not touch or intersect.
     * Winding is irrelevant. Multiple disjoint regions are supported. */
    WobyImportTrimLoop outer;
    const WobyImportTrimLoop* holes;
    uint32_t hole_count;
} WobyImportTrimRegion;

typedef struct WobyImportFreeformPatch {
    /* One stable named group per patch, with the usual color/visibility flags.
     * index_offset and index_count must both be zero: the host tessellates it. */
    WobyImportGroup group;
    WobyImportSpline spline;
    const WobyImportTrimRegion* regions;
    uint32_t region_count; /* Zero means untrimmed. Curves must use zero. */
} WobyImportFreeformPatch;

typedef struct WobyImportFreeform {
    uint32_t struct_size;
    const WobyImportFreeformPatch* patches;
    uint32_t patch_count;
    const WobyImportSpline* trim_curves;
    uint32_t trim_curve_count;
} WobyImportFreeform;

typedef struct WobyImporterApiWithGeometry {
    /* Set base.base.base.base.struct_size to sizeof(WobyImporterApiWithGeometry)
     * and return &base.base.base.base. All optional callbacks can be NULL.
     * Hierarchy order: triangle groups, line groups, point groups, then patches
     * in array order (curves and surfaces interleaved). Includes default groups.
     * A freeform-only result may leave the base vertex/index buffers empty. */
    WobyImporterApiWithPointIds base;
    /* Borrowed data prepared in import_file, valid until release_result. */
    const WobyImportPoints* (WOBY_IMPORT_CALL *get_points)(const WobyImportResult*);
    const WobyImportFreeform* (WOBY_IMPORT_CALL *get_freeform)(const WobyImportResult*);
} WobyImporterApiWithGeometry;

typedef const WobyImporterApi* (WOBY_IMPORT_CALL *WobyGetImporterApi)(uint32_t host_abi_version);

/* Each plugin exports this exact symbol; return NULL for an unsupported ABI. */
#ifdef WOBY_IMPORTER_BUILD
WOBY_IMPORT_EXPORT
#endif
const WobyImporterApi* WOBY_IMPORT_CALL woby_get_importer_api(uint32_t host_abi_version);

#ifdef __cplusplus
}
#endif
#endif

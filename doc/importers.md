# File format importers

woby can load native importer libraries supplied by the user. An importer reads
a model file and returns triangles, lines, points, and spline curves/surfaces
(including trimming), with named groups and optional hierarchy,
color and visibility defaults. woby owns the copied geometry, scene settings, GPU resources,
and rendering. The DLL must implement the woby API;
an arbitrary third-party format library needs a small adapter.

## Portable importer packages

Create an `importers` folder beside the woby executable. Put each importer in its
own immediate subfolder with an `importer.json` manifest:

```text
woby.exe
importers/
  off/
    importer.json
    woby_off_importer.dll
    dependency.dll
    data/
```

The manifest is UTF-8 JSON:

```json
{
  "schema": 1,
  "library": "woby_off_importer.dll"
}
```

`schema` must be the integer `1`. `library` names the entry library relative to
the manifest's folder, using forward slashes (for example `bin/off.dll`). Absolute
paths, parent traversal, and links resolving outside that package are rejected.
The library must exist; manifests are limited to 64 KiB. The DLL API supplies the
importer's ID, name, version, and extensions, so the manifest does not duplicate them.
On Linux or macOS, use the corresponding `.so` or `.dylib` filename.

woby scans these package folders on every viewer launch, before loading models or
scenes, regardless of the working directory. Only the declared library is loaded;
companion libraries and data files are left alone. Windows resolves DLL dependencies
beside the entry DLL; other platforms need their usual loader search paths/rpaths.
Loose libraries, folders without manifests, and deeper nested packages are not
automatically scanned. A missing `importers` folder is allowed and is not created
automatically. No `--remember` registration is needed, and packages keep working
when the whole installation moves.

Loading order is explicit CLI options, remembered registrations, then portable
packages sorted by folder path. Earlier registrations win ID/extension conflicts.
Invalid manifests or libraries produce startup diagnostics and do not stop other
packages from loading. Restart woby after adding or replacing a package. To remove
an automatic importer, close woby and remove its package folder; `importers forget`
only removes remembered registrations.

The portable `importers/` directory belongs to the user. Automatic updates and
rollback preserve its manifests, libraries, dependencies, and data. Release
packaging and the updater reject manifests claiming that directory, and recovery
rejects journals that would modify it. Bundled application files must live elsewhere.

## Explicit loading and remembered registrations

Both command-line options may appear any number of times:

```powershell
woby.exe --plugin C:\importers\off.dll --plugin D:\tools\other.dll
woby.exe --plugin-folder C:\importers --plugin-folder D:\company-importers
woby.exe --plugin-folder C:\importers --plugin D:\tools\other.dll --file C:\models\part.off
```

Options are processed in their command-line order, before any model or scene is
loaded. Each plugin folder is scanned non-recursively, in sorted path order, for
`.dll` files on Windows, `.so` files on Linux, and `.dylib`/`.so` files on macOS.
Use a dedicated importer folder; dependencies can live beside the importer but
will be reported as missing the importer export if included in a folder scan.
These explicit scans retain their existing loose-library behavior; use portable
packages above to select an entry library without scanning its dependencies.
Loading the same physical file more than once has no effect. Duplicate importer
IDs and extension conflicts are reported, with the earlier registration retained.
OBJ and the `.woby` scene extension are reserved.

Startup options apply only to that launch. Use `wobyctl importers add PATH --remember`
to load a library and save its absolute path for future launches. Registrations are
stored in `importers.txt` in SDL's per-user preference
directory for organization/application `woby/woby`. CLI registrations are loaded
before saved registrations. Invalid registrations produce diagnostics on stderr,
in enabled logs and through `wobyctl importers list`; other plugins can still load.
Use `wobyctl importers forget PATH` to remove a saved registration. Already loaded
plugins stay loaded until exit. Restart woby after replacing a DLL.

Registered formats are available through the model dialog, `--file`, recursive
`--folder` / `--folder-tree`, drag and drop, and scene reopening. File dialog filter
strings remain alive until the asynchronous dialog callback completes.

## Saved scenes

Scenes continue to reference the original model files. A plugin-imported file also
records the importer's stable ID:

```toml
version = 4

[[files]]
path = "models/part.off"
importer_id = "org.woby.example.off"
# Normal file/group settings and scene hierarchy follow.
```

The DLL path and binary are not embedded in a scene. Opening a scene never loads
a DLL by a path from that scene. The matching importer must already be registered
on that machine. Missing importers fail scene loading before the existing scene
is replaced. Scenes save as version 21 and include independent analysis
objects in `[[analyses]]` records referencing saved file/group indexes.
The optional `analysis_task` field records the inspector category (`mesh_checks`,
`mesh_quality`, `surface_comparison`, or `uv_inspection`). Older scenes infer the
category from their existing analysis type, display mode, and inputs.
Mesh checks use one Sources list. Selecting this task or loading an explicitly
saved `mesh_checks` analysis consolidates A and B into A, preserving disabled
parts and saved-view source states. A repeated part is enabled if either input
enabled it. Legacy two-input analyses keep separate inputs in Surface comparison
or Mesh quality until Mesh checks is selected.

Saved group settings are indexed. Importers must preserve unique group names and
their order for the same file across versions. Reopening rejects a changed group
count or name/order instead of applying settings to different groups. Importer
software versions are displayed but not pinned in scenes; compatible updates may
continue using the same ID. Importer IDs must remain stable when the DLL is moved.

Imported containers and leaves are saved using the existing `[[nodes]]` scene
records. Saved trees take precedence over the importer's current default hierarchy,
including scenes saved with a flat tree before the importer supported hierarchy.
Per-child visibility, color and other settings remain independent saved values.

## Authoring a DLL

The public, C-compatible header is [`include/woby/importer.h`](../include/woby/importer.h).
Define `WOBY_IMPORTER_BUILD` when building the plugin and export the exact C symbol
`woby_get_importer_api`. It takes the host ABI version and returns a static
`WobyImporterApi`, or null for unsupported versions. The current ABI version is 4.
Rebuild existing plugins against the current header. ABI 1, 2, and 3 are rejected;
there is no fallback negotiation. ABI 4 adds optional drawable points and freeform
geometry. Existing triangle/line plugins only need to rebuild and advertise ABI 4;
their table layouts and behavior are unchanged. Optional original-point identity
metadata still controls duplicate-point counting independently of drawable points.
The DLL must match the host's architecture (the Windows preset builds x64).
Use the platform's default struct alignment, 64-bit IEEE doubles for positions,
32-bit IEEE floats for normals/UVs, and the header's
calling convention. No C++ containers, exceptions, FILE handles, SDL, ImGui, or
bgfx objects cross the interface.

The API advertises a nonempty ID, display name, software version, and a
semicolon-separated list of alphanumeric extensions without dots. Extensions are
case-insensitive. IDs and versions are at most 255 bytes; other individual strings
are at most 4095 bytes. All strings and request paths use null-terminated UTF-8.
The request path is absolute. Plugins may read companion files relative to it.

`import_file` receives a zero-initialized result with `struct_size` already set.
It must leave that size intact, fill the result, and return OK, ERROR, or CANCELED.
On errors it may provide a diagnostic string. The host invokes `release_result`
exactly once after every call, including errors, cancellation, and rejected output.
The release function must tolerate partially filled results and must not throw.
The plugin allocates and frees its own buffers; the host copies successful output
before release. Never retain request pointers or callbacks after the call returns.
Do not perform substantial work in `DllMain`.

Output consists of interleaved vertices, zero-based 32-bit triangle indices, and
optional groups. Each vertex contains a position, normal, and texture coordinate;
each triangle uses three indices into that single vertex table. Plugins must
triangulate polygons and combine any separate position/normal/UV index tables
before returning. Vertices must be split where corners at the same position need
different normals or UVs. Unlike the built-in OBJ loader, the importer host does
not perform these conversions.

Flags indicate whether normals and texture coordinates are present.
Missing normals are generated; missing texture coordinates become zero. Positions
must be finite and have absolute components no greater than 1e9. Supplied normals
must be finite and nonzero; woby normalizes them. UVs must be finite.
All indices must reference valid vertices. The combined input vertex/index/point-ID buffers
are limited to 4 GiB; internal copies and GPU buffers consume additional memory.

Groups must partition the entire index buffer consecutively, with nonempty ranges
aligned to triangles, and unique nonempty names. The limits are 100,000 groups and
1 MiB of total group-name bytes. Zero groups produces one group named `Mesh`.

Zero-initialize each group's `flags` and `color` when no appearance defaults are
needed. `WOBY_IMPORT_GROUP_HAS_COLOR` supplies an RGBA color in `color[4]`, using
the same RGB values as woby's group color control. Every component must be finite
and in `[0, 1]`; invalid colors and unknown group flags reject the import.
Without that flag, color storage is ignored and the normal palette is used.
Alpha initializes the separate group opacity control; the UI color's alpha is
set to 1. A color with alpha 0 is transparent, but is still logically visible.

`WOBY_IMPORT_GROUP_INITIALLY_HIDDEN` starts the group hidden without discarding
its geometry. Users can reveal it with the existing visibility control. Without
that flag, the group starts visible. Both flags can be combined:

```c
WobyImportGroup groups[] = {
    {"Primary", 0, 3, WOBY_IMPORT_GROUP_HAS_COLOR, {0.2f, 0.6f, 0.9f, 1.0f}},
    {"Auxiliary", 3, 3, WOBY_IMPORT_GROUP_HAS_COLOR | WOBY_IMPORT_GROUP_INITIALLY_HIDDEN,
        {0.8f, 0.4f, 0.2f, 0.5f}}
};
```

These values initialize new imports only. Saved scene color, opacity, and
visibility override the importer defaults, including when those defaults change.
Palette assignment for other groups retains its normal group-index ordering.

The host retains the returned vertex table, including unused and duplicate
vertices; it does not compact or deduplicate it. Vertex, group, and triangle order
are preserved. Unused vertices also contribute to the calculated mesh bounds.
Plugins must bake source transforms and coordinate/unit conversions into their
output. Return positions in double precision without recentering them. Woby
chooses a local origin before constructing float GPU vertices and retains double
positions for analysis. Source bounds and exported finding coordinates restore
the original coordinates. This does not recover detail already lost by an SDK
or a source format that stores positions as floats.

Parametric CAD surfaces, full material definitions, textures, and animation are
not part of this API. Optional hierarchy organizes triangle and line groups;
it does not add instancing or source transforms.

Imports through the interactive loading pipeline run on its CPU worker; startup
imports use the existing synchronous startup pipeline. Calls into each importer
are serialized. Both callbacks must be invoked only on the importing thread and
only during `import_file`. Report progress as a fraction from 0 to 1 and poll
`is_canceled` during lengthy work. Cancellation is cooperative; there is no forced
timeout. All plugin-owned threads must finish before the import returns. GPU work
stays on woby's main thread. Host callback exceptions are captured and rethrown
after the DLL returns rather than unwinding across the ABI.

Native importers execute inside woby's process with its permissions. Buffer checks
validate a cooperative plugin's output; they cannot make arbitrary pointers safe
or isolate DLL crashes. Use trusted libraries. Crash isolation would require a
separate importer process.

## Optional original-point IDs

Importers may split one source point into several vertices to provide different
normals or UVs, or to keep adjacent closed volumes topologically separate. Return
`WobyImportPointIds` through `WobyImporterApiWithPointIds.get_point_ids` to identify
these intentional copies. This changes only `duplicate_points`: at each exactly
equal position it counts distinct original IDs minus one, rather than vertex
records minus one. Different IDs at the same position remain genuine duplicates,
even when they belong to different groups.

Set `base.base.base.struct_size = sizeof(WobyImporterApiWithPointIds)` and return
`&api.base.base.base`. The intervening hierarchy and line callbacks can be null.
A base table, null callback, or null returned metadata preserves existing behavior.
Prepare the metadata during `import_file`; Woby copies it before `release_result`,
which still runs exactly once after failure, cancellation, or rejected metadata.

```c
/* Two coincident triangles may have opposite normals in six render vertices. */
static const uint64_t ids[] = {10, 20, 30, 10, 20, 30};
static const WobyImportPointIds point_ids = {sizeof(WobyImportPointIds), ids, 6};
static const WobyImportPointIds* WOBY_IMPORT_CALL get_point_ids(const WobyImportResult* result)
{
    (void)result;
    return &point_ids;
}
static const WobyImporterApiWithPointIds api = {
    {{{sizeof(WobyImporterApiWithPointIds), WOBY_IMPORTER_ABI_VERSION,
       "org.example.solids", "Solid importer", "1", "solids",
       import_file, release_result}, 0}, 0},
    get_point_ids
};
WOBY_IMPORT_EXPORT const WobyImporterApi* WOBY_IMPORT_CALL woby_get_importer_api(uint32_t version)
{
    return version == WOBY_IMPORTER_ABI_VERSION ? &api.base.base.base : 0;
}
```

- Supply exactly one ID per result vertex, including unused and line-only vertices.
  A non-null metadata object must have a valid `struct_size`, non-null `ids`, and
  `vertex_count` equal to the result's count. Its bytes count toward the 4 GiB limit.
- IDs are opaque unsigned 64-bit values scoped to this imported file. Zero and
  `UINT64_MAX` are valid; there is no sentinel or dense-numbering requirement.
- Vertices with the same ID must have exactly equal original double coordinates;
  signed zeros compare equal. Woby checks this before shifting the working origin,
  so even coordinate differences lost during that shift reject the import.
- Duplicate findings list the first selected vertex index for each identity, in
  the existing one-based report format. Importer IDs are not substituted for report
  indices. All selected transformed copies remain included in the display geometry.
  Whole-file analysis still includes unused points; part-only analysis does not.
- IDs do not weld render vertices, change `duplicate_tris`, or alter topology.
  Keep separate indices for adjacent closed volumes. Exact-position topology
  remains an explicit geometric weld and can join their contact surfaces.

Scenes reload this metadata from the importer; it is not embedded in `.woby` files.

## Optional model hierarchy

An importer can return an assembly tree such as `Assembly / Solid A / Patch 1`.
This is an optional, size-gated API table extension: `WobyImporterApi`,
`WobyImportResult`, and the vertex/group layouts are unchanged. ABI 4 libraries
using only the base API table load as flat files. All API tables must advertise
ABI 4, including plugins that use neither hierarchy nor point IDs.

Return the `base` member of a static `WobyImporterApiWithHierarchy` from
`woby_get_importer_api`. Set **`base.struct_size` to the size of the extended
table**, and supply `get_hierarchy`. Woby calls it after a successful `import_file`
and before `release_result`. A null callback or a null returned pointer uses the
flat behavior for that import. Prepare the metadata during `import_file`, usually
alongside geometry in `result->user_data`; the callback only retrieves it. All
node strings and arrays remain owned by the plugin until `release_result`.

For example, given stable groups named `solid-a/patch-1`, `solid-a/patch-2`, and
`solid-b/patch-1`, the hierarchy can use shorter, repeated display labels:

```c
static const WobyImportHierarchyNode nodes[] = {
    {"Solid A", WOBY_IMPORT_NO_PARENT, WOBY_IMPORT_NO_GROUP},
    {"Patch 1", 0, 0},
    {"Patch 2", 0, 1},
    {"Solid B", WOBY_IMPORT_NO_PARENT, WOBY_IMPORT_NO_GROUP},
    {"Patch 1", 3, 2}
};
static const WobyImportHierarchy tree = {
    sizeof(WobyImportHierarchy), nodes, 5
};
static const WobyImportHierarchy* WOBY_IMPORT_CALL get_hierarchy(const WobyImportResult* result)
{
    (void)result;
    return &tree;
}
static const WobyImporterApiWithHierarchy api = {
    {sizeof(WobyImporterApiWithHierarchy), WOBY_IMPORTER_ABI_VERSION,
     "org.example.surfaces", "Surface importer", "1", "surfaces",
     import_file, release_result},
    get_hierarchy
};
WOBY_IMPORT_EXPORT const WobyImporterApi* WOBY_IMPORT_CALL woby_get_importer_api(uint32_t version)
{
    return version == WOBY_IMPORTER_ABI_VERSION ? &api.base : 0;
}
```

The hierarchy must satisfy these rules:

- Parents precede their children in the array; `WOBY_IMPORT_NO_PARENT` places a
  node directly under the model file. Multiple roots are allowed. Sibling order
  follows array order, independently of triangle-group order.
- Containers use `WOBY_IMPORT_NO_GROUP`. A group node references the original
  zero-based group index and cannot have children. Every group occurs exactly
  once. For a result with no explicit groups, index `0` refers to the generated
  `Mesh` group. Empty named containers are allowed and contain no geometry.
- Labels are nonempty UTF-8 strings of at most 4095 bytes; duplicates are allowed.
  The original group names remain unique, stable save-file identities.
- At most 100,000 nodes, 1 MiB total label bytes, and 128 levels (including leaves,
  excluding Woby's file wrapper) are accepted. A non-null tree must contain nodes.
  Invalid trees reject the import and still call `release_result` exactly once.

Containers appear as folders under the model in Woby's tree. Selecting a parent
lets users change its descendants' color or visibility; subsequent child edits
remain independent. New parent visibility reflects whether any descendant is
visible. Parent transforms use the existing scene controls. Geometry must still
be supplied in one file coordinate system with source transforms baked in.
`ctl scene tree` exposes nested children and each occurrence's `parentId`;
`ctl object` includes `parentId` in its `occurrences` array. Root parents are null.

## Optional line segments

`WobyImporterApiWithLines` extends the hierarchy table with `get_lines`. Set
`base.base.struct_size = sizeof(WobyImporterApiWithLines)` and return
`&api.base.base` from the entry point. The base and hierarchy layouts stay unchanged;
triangle-only and hierarchy tables remain supported when built for ABI 4.

Prepare `WobyImportLines` during `import_file`. Its `indices` are pairs of
zero-based indices into **the same vertex table as the triangles**. Each pair
is an independent segment: a polyline `(a,b,c)` becomes `(a,b,b,c)`. There are
no implicit closing edges. Degenerate or zero projected-length segments are
accepted but do not draw. Importers can sample curves into segments themselves
or return exact spline controls through the freeform extension below. The plugin
ABI exposes all geometry types currently supported by the built-in OBJ reader.

```c
static const uint32_t segments[] = {0, 1, 1, 2};
static const WobyImportGroup curves[] = {
    {"solid-a/boundary", 0, 4, WOBY_IMPORT_GROUP_HAS_COLOR, {0, 1, 0, 1}}
};
static const WobyImportLines lines = {sizeof(WobyImportLines), segments, 4, curves, 1};
static const WobyImportLines* WOBY_IMPORT_CALL get_lines(const WobyImportResult* result)
{
    (void)result;
    return &lines;
}
static const WobyImporterApiWithLines api = {
    {{sizeof(WobyImporterApiWithLines), WOBY_IMPORTER_ABI_VERSION,
      "org.example.curves", "Curve importer", "1", "curves",
      import_file, release_result},
     get_hierarchy}, /* May be null when no hierarchy is needed. */
    get_lines
};
WOBY_IMPORT_EXPORT const WobyImporterApi* WOBY_IMPORT_CALL woby_get_importer_api(uint32_t version)
{
    return version == WOBY_IMPORTER_ABI_VERSION ? &api.base.base : 0;
}
```

A null callback, null returned pointer, or an empty line buffer with no groups
means there are no lines. The host calls this callback after a successful import,
copies its data before releasing the result, and never takes ownership of plugin
allocations. Release line metadata and geometry from `release_result`, including
on failed validation or cancellation.

Line groups use `WobyImportGroup` with the same color and initial visibility flags.
They must cover their line index buffer consecutively, with nonempty, pair-aligned
ranges. Names must be unique **across all primitive groups and freeform patches**. Combined
limits remain 100,000 groups, 1 MiB of group-name bytes, and 4 GiB of input vertex
and index buffers. All index values must reference valid vertices. The normal/UV
flags still apply to the entire shared vertex table, including line vertices;
leave those flags unset if the corresponding data is not supplied.

Hierarchy `group_index` addresses the concatenation of triangle groups followed
by line groups, point groups, and freeform patches. With no explicit groups, a nonempty triangle buffer contributes
one `Mesh` group and a nonempty line buffer contributes one `Lines` group. An
empty triangle buffer contributes no group. For a line-only model, set the base
result's `indices = NULL`, `index_count = 0`, `groups = NULL`, and `group_count = 0`;
hierarchy index zero then addresses the first line group. At least one primitive
buffer or freeform patch must be present. Every generated or explicit group needs exactly one
hierarchy leaf when hierarchy metadata is supplied.

Each line group appears in the scene tree and supports color, opacity, visibility,
transforms, selection, framing, and optional vertex markers. Line width is measured
in drawable pixels (1-12, default 2). Lines are depth-tested by default; **Draw lines
on top** disables depth testing. Parent edits apply to eligible descendant groups,
and later child edits remain independent. Surface analyses and UV views accept
triangle groups only; importing curves does not add them to triangle topology.

Line appearance is persisted in scene format 18, views, and undo history. Previous
scene versions remain readable. The saved primitive kind prevents an updated
importer from silently applying triangle settings to a same-named line group.
`ctl object` and `ctl scene tree` expose each group's `primitive`; object details
also include `lineSegmentCount`, and settings include `lineWidth` and `lineDepthTest`.

```powershell
woby ctl --instance review render set GROUP_ID --line-width 4 --line-depth-test false
```

## Points and freeform geometry (ABI 4)

`WobyImporterApiWithGeometry` extends `WobyImporterApiWithPointIds` with
`get_points` and `get_freeform`. Set `base.base.base.base.struct_size` to
`sizeof(WobyImporterApiWithGeometry)` and return `&api.base.base.base.base`.
All optional callbacks can be null, and a null callback result means absent for
that file. These callbacks only retrieve data prepared by `import_file`; the host
copies everything, including trimming curves, before `release_result`.

```c
static const WobyImporterApiWithGeometry api = {
    {{{{sizeof(WobyImporterApiWithGeometry), WOBY_IMPORTER_ABI_VERSION,
        "org.example.geometry", "Geometry importer", "1", "geom",
        import_file, release_result}, get_hierarchy}, get_lines}, get_point_ids},
    get_points, get_freeform
};
/* Return version == WOBY_IMPORTER_ABI_VERSION ? &api.base.base.base.base : 0. */
```

`WobyImportPoints` contains individual indices into the base result's vertex
table. Point clouds use one index per desired vertex; there are no implicit
points from unused vertices. Point groups partition this buffer consecutively,
with nonempty ranges. An indexed point buffer without groups creates `Points`.
A point-only result leaves the base triangle indices/groups empty. This callback
is distinct from `get_point_ids`, which supplies duplicate-inspection identities.
Point groups use the existing point size, color, opacity, visibility, selection,
transform, and scene persistence controls.

`WobyImportFreeform` supplies an array of named patches and a separate array of
UV trimming curves. Each patch creates one group. Hierarchy `group_index` order
is **triangle groups, line groups, point groups, then patches in array order**;
curves and surfaces remain interleaved as supplied. Default groups count too.
Names must be unique across the whole file; a collision with `Mesh`, `Lines`, or
`Points` is rejected if that default group is generated. Patch groups use the
usual appearance flags, with `index_offset = index_count = 0` because the host
creates their indices. A freeform-only file can leave all base buffers empty.
At least one drawable primitive or patch is required.

A `WobyImportSpline` is a curve or tensor-product surface in canonical B-spline
form. It carries double-precision controls, positive rational weights, degrees,
knot vectors, and parameter domains. This single representation supports:

- **Bézier:** degree `p`, `p+1` controls, knots `[a repeated p+1, b repeated p+1]`.
  For a piecewise Bézier spline, interior break knots repeat `p` times and
  adjacent spans share endpoints. Apply the same rule independently in U and V.
- **B-spline:** the authored knot vector, with every control weight set to 1.
- **NURBS / rational Bézier:** the same representation with positive weights.
  Controls contain Euclidean xyz; do not multiply coordinates by the weights.

Controls are U-fastest (`controls[v * count_u + u]`). Each knot vector contains
`count + degree + 1` entries. Degrees are 1–8, knots must be finite and
nondecreasing, and domains are increasing subranges of the active knot range
`[knots[degree], knots[count]]`. Interior knot multiplicity cannot exceed the
degree; discontinuous splines are rejected. Curves use `degree_v=0`, `count_v=1`,
and `knot_count_v=0`. Optional normals and texture coordinates must be present on
every control when their flags are set. Spline UVs use the OBJ convention: Woby
flips V for display. This differs from the base vertex table's display-ready UVs.
XYZ components must be finite and within ±1e9, as for ordinary imported vertices.

For example, a quadratic rational quarter circle uses controls `(1,0,0)`,
`(1,1,0)`, `(0,1,0)`, weights `1, sqrt(0.5), 1`, degree 2, knots
`[0,0,0,1,1,1]`, and domain `[0,1]`. Use all weights 1 for a quadratic Bézier curve.

A patch with no regions is untrimmed. For trimmed surfaces, each region specifies
an outer loop and holes; an empty outer loop means the surface parameter
rectangle. Loops concatenate `WobyImportTrimSegment` intervals referencing the
separate trim-curve array. Trim curves use `(u,v,0)` controls, `kind=CURVE`, and
`flags=0`; they create no visible groups. Intervals can run forward or backward
inside the curve's domain. Loops must close and be simple; holes must be nonempty
and inside their outer region. Touching/crossing boundaries, nested holes, and
overlapping regions are rejected. Winding does not matter. Multiple disjoint
regions and islands inside holes use the same rules as OBJ imports.

Woby retains immutable spline data and uses the existing CPU/GPU tessellation
path. CPU geometry supplies bounds, picking, inspection, and the GPU fallback;
trimmed connectivity is constrained in parameter space. Normals and UVs follow
the spline evaluator. Curves become line groups, surfaces become triangle groups,
and control nets and trimming curves are not drawn. Scene files reference the
source model and importer ID, so reopening regenerates geometry and restores
appearance. The scene format does not need a new version for this extension.

Limits include 100,000 combined drawable groups, 1 MiB of group-name bytes,
4 GiB of base vertex/index/point-ID buffers, 2,000,000 total spline controls
(including trimming controls), 100,000 trim curves with at most 4,096 controls
each, and 100,000 total region,
hole, and segment records. Tessellation is capped at 2,000,000 generated vertices
per file and the existing trimming work/boundary limits. Invalid outputs reject
the import with a diagnostic and still release the result exactly once. Host
validation and tessellation support cancellation. If source point IDs are
supplied, generated vertices receive distinct host IDs that cannot collide with
any original ID; the original IDs are retained unchanged.

[`tests/importer_geometry_fixture.h`](../tests/importer_geometry_fixture.h) and
[its DLL adapter](../tests/importer_geometry_fixture.cpp) provide a compiled
example combining triangles, lines, points, a rational curve, a trimmed surface,
appearance, hierarchy, and source identities. The OFF example below demonstrates
a minimal triangle-only plugin built for ABI 4.

## Example: triangular OFF files

[`examples/off_importer`](../examples/off_importer) contains an independent example
that reads the basic ASCII OFF format with triangular faces (no comments, colors,
or polygon tessellation). The normal test build also builds this DLL:

```powershell
cmake --preset vs2026-vcpkg
cmake --build --preset vs2026-vcpkg
.\build\vs2026-vcpkg\bin\Debug\woby.exe --plugin .\build\vs2026-vcpkg\example-plugins\Debug\woby_off_importer.dll --file .\examples\off_importer\triangle.off
```

It can also be built independently without woby's dependencies:

```powershell
cmake -S examples/off_importer -B build/off-importer -G "Visual Studio 18 2026" -A x64
cmake --build build/off-importer --config Debug
```

Both builds generate `importer.json` beside the example library. Copy that manifest
and the library into `<woby executable folder>/importers/off/`, then launch woby
normally to load OFF files automatically.

The SDK avoids cross-DLL CRT ownership problems described by
[Microsoft](https://learn.microsoft.com/en-us/cpp/c-runtime-library/potential-errors-passing-crt-objects-across-dll-boundaries).
Windows loading uses an absolute path with `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR` and
`LOAD_LIBRARY_SEARCH_DEFAULT_DIRS` as described in
[LoadLibraryExW](https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-loadlibraryexw).
The platform adapter uses `dlopen` with local symbols on Linux and macOS.

# File format importers

woby can load native importer libraries supplied by the user. An importer reads
a model file and returns triangles and named groups with optional hierarchy,
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
OBJ, STL, and the `.woby` scene extension are reserved.

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
is replaced. Scenes save as version 17 and include independent analysis
objects in `[[analyses]]` records referencing saved file/group indexes.

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
`WobyImporterApi`, or null for unsupported versions. The current ABI version is 2.
The group layout was extended within ABI 2 before external adoption. Rebuild any
existing plugins against the current header; previous ABI 2 group layouts are not
compatible and cannot be distinguished by the version number. ABI 1 is rejected.
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
All indices must reference valid vertices. The combined input vertex/index buffers
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

CAD surfaces, full material definitions, textures, and animation are not part of
this mesh-only API. Optional hierarchy describes the organization of these same
triangle groups; it does not add instancing or source transforms.

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

## Optional model hierarchy

An importer can return an assembly tree such as `Assembly / Solid A / Patch 1`.
This is an optional, size-gated extension to ABI 2: `WobyImporterApi`,
`WobyImportResult`, and the vertex/group layouts are unchanged. Existing ABI 2
libraries continue to load as flat files without recompilation. Older ABI 2 hosts
can load an extended library and use its flat groups, ignoring hierarchy.

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

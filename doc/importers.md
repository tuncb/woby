# File format importers

woby can load native importer libraries supplied by the user. An importer reads
a model file and returns triangles and named groups. woby owns the copied geometry,
scene settings, GPU resources, and rendering. The DLL must implement the woby API;
an arbitrary third-party format library needs a small adapter.

## Loading plugins

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
is replaced. Scenes save as version 5, which includes independent comparison
objects referencing saved file/group indexes. Version 2–4 scenes remain readable;
legacy per-part A/B membership migrates into one comparison object. Older woby
versions reject version 5 explicitly.

Saved group settings are indexed. Importers must preserve unique group names and
their order for the same file across versions. Reopening rejects a changed group
count or name/order instead of applying settings to different groups. Importer
software versions are displayed but not pinned in scenes; compatible updates may
continue using the same ID. Importer IDs must remain stable when the DLL is moved.

## Authoring a DLL

The public, C-compatible header is [`include/woby/importer.h`](../include/woby/importer.h).
Define `WOBY_IMPORTER_BUILD` when building the plugin and export the exact C symbol
`woby_get_importer_api`. It takes the host ABI version and returns a static
`WobyImporterApi`, or null for unsupported versions. The current ABI version is 1.
The DLL must match the host's architecture (the Windows preset builds x64).
Use the platform's default struct alignment, 32-bit IEEE floats, and the header's
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

Output consists of interleaved vertices, 32-bit triangle indices, and optional
groups. Flags indicate whether normals and texture coordinates are present.
Missing normals are generated; missing texture coordinates become zero. Positions
must be finite and have absolute components no greater than 1e9. Supplied normals
must be finite and nonzero; woby normalizes them. UVs must be finite.
All indices must reference valid vertices. The combined input vertex/index buffers
are limited to 256 MiB; internal copies and GPU expansion consume additional memory.

Groups must partition the entire index buffer consecutively, with nonempty ranges
aligned to triangles, and unique nonempty names. The limits are 100,000 groups and
1 MiB of total group-name bytes. Zero groups produces one group named `Mesh`.
Unused vertices are removed. Group and triangle order are preserved.
Plugins must bake source transforms and coordinate/unit conversions into their
output. Hierarchies, CAD surfaces, materials, textures, and animation are not part
of this mesh-only API.

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

The SDK avoids cross-DLL CRT ownership problems described by
[Microsoft](https://learn.microsoft.com/en-us/cpp/c-runtime-library/potential-errors-passing-crt-objects-across-dll-boundaries).
Windows loading uses an absolute path with `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR` and
`LOAD_LIBRARY_SEARCH_DEFAULT_DIRS` as described in
[LoadLibraryExW](https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-loadlibraryexw).
The platform adapter uses `dlopen` with local symbols on Linux and macOS.

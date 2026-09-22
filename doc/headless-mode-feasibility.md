# Headless mode feasibility

Historical investigation preceding implementation. For the implemented mode,
current backend support, and agent instructions, see [headless.md](headless.md).

Investigated on 2026-09-22 against Woby commit `e6034e6`, Windows, and the installed
bgfx package `1.129.8940-496#1`.

## Conclusion

A headless mode with camera controls and PNG screenshots is feasible. The camera,
command API, scene state, and offscreen capture already exist. The main work is
extracting the application processing loop from the desktop UI in `main.cpp`.
This is a moderate runtime refactor, not a replacement renderer.

An isolated experiment using the installed bgfx library, Woby's camera functions,
and Woby's compiled shaders successfully rendered and read back pixels with no
SDL initialization or native window. Direct3D 11 worked on this Windows machine.
The complete Woby application does not yet offer a headless launch mode; this
experiment establishes the rendering foundation, not full application support.

Recommend a persistent `woby --headless --instance review` process controlled by
the existing `woby ctl` commands or local JSON-RPC API. Start with Windows/D3D11,
then validate additional platforms independently. A new MCP server or scripting
language is not required for the initial AI workflow.

## Existing capabilities to reuse

| Requirement | Existing implementation |
| --- | --- |
| Model loading and scene editing | CLI/RPC model, folder, scene, visibility, transform, and analysis commands. |
| Camera navigation | Absolute placement, look-at, fit object/scene, presets, orbit, pan, roll, dolly, movement, and saved views. `camera.cpp` has no SDL or ImGui dependency; CTL uses operations on `UiState`. |
| Screenshots | `scene_screenshot.cpp` renders into its own color/depth framebuffer, blits to a readback texture, then writes PNG. It does not capture the desktop. |
| Capture ordering | The automation queue keeps a screenshot active until GPU readback and PNG writing finish. Later camera commands cannot overtake it. |
| Analysis readiness | Capture waits for visible analysis results through `comparisonsReadyForScreenshot`; invalid or failed results produce errors. |
| Structured automation | Local authenticated JSON-RPC, instance discovery, JSON CLI output, typed commands, timeouts, command lookup, and retry keys. |
| State persistence | Camera and scene settings already use the canonical `UiState`, operations, and `.woby` mapping. |

Useful code locations:

- `src/main.cpp:2067`: startup; `:2141`: SDL window creation; `:2166`: bgfx initialization.
- `src/main.cpp:2549`: GPU finalization; `:3181`: automation dispatch;
  `:3411`: analysis updates; `:3561`: capture submission; `:3594`: frame/readback completion.
- `src/control_scene.cpp:501`: camera commands through state operations.
- `src/scene_screenshot.cpp:60`: independent render targets; `:228`: capture rendering;
  `:469`: readback completion.
- `src/comparison_view.cpp:1382`: analysis runtime updates; `:1409`: capture readiness.
- `doc/automation.md` and `doc/ctl-commands.md`: current API contracts.

## Hidden window versus fully windowless

| Mode | Scope | Assessment |
| --- | --- | --- |
| Hidden desktop window | Create an SDL window with `SDL_WINDOW_HIDDEN`; keep the current desktop runtime. | Smallest initial change for local scripts, but retains window-system dependencies and background UI/render work. It does not establish support for servers without a desktop. |
| Windowless rendering | Initialize bgfx without native window/display handles; render only to explicit framebuffers. | Recommended long-term mode. The installed D3D11 backend passed the isolated probe. Requires runtime/UI separation. |
| No hardware GPU | Use a supported software graphics implementation. | Separate compatibility and performance work. Not established by this investigation; headless does not imply GPU-free. |

SDL documents hidden window creation in
[SDL_CreateWindow](https://wiki.libsdl.org/SDL3/SDL_CreateWindow).
Bgfx documents headless initialization when the native window handle is null,
subject to backend support, in its
[PlatformData reference](https://bkaradzic.github.io/bgfx/bgfx.html).
Its upstream [headless example](https://github.com/bkaradzic/bgfx/blob/master/examples/50-headless/headless.cpp)
also renders into an offscreen framebuffer and reads the result back.
Use the installed API when implementing: this checkout uses `init.resolution`,
whereas the current upstream example uses a newer initialization layout.

## Required changes

1. Add a runtime launch option for headless mode. Keep it outside `UiState` and
   the `.woby` format. Initialize bgfx with empty platform data, zero backbuffer
   width/height, and no display vsync. Keep explicit capture dimensions separate
   from the nonexistent backbuffer. Reject a Noop renderer or missing capture
   capabilities with a useful error.
2. Extract shared runtime structs and free functions for startup, loading, GPU
   finalization, scene history, command execution, analysis updates, and capture
   completion. Both desktop and headless modes must use the same state operations
   and dispatch behavior. Avoid duplicating the substantial command visitor.
3. Keep desktop event processing, keyboard/mouse camera input, window title/size,
   menus, panes, dialogs, and default-backbuffer rendering in the desktop adapter.
   The current code always touches desktop views; those submissions must be
   skipped in windowless mode, not merely assigned a zero-sized viewport.
4. Retain an ImGui context, fonts, and the bgfx ImGui renderer for export overlays.
   Screenshot code currently accesses ImGui shared draw data and the font atlas
   even when labels are disabled. A small offscreen frame setup can supply
   display size and delta time without `ImGui_ImplSDL3`. Legends, dimensions, grid
   labels, and first-frame dynamic font uploads must keep working. Removing ImGui
   entirely would be extra work with little benefit for the first version.
5. Preserve the command-to-capture ordering. Continue updating workers, uploading
   results, and advancing `bgfx::frame()` until readback completes. Going to sleep
   after submitting a capture can stall it. Once genuinely idle, wait for work
   rather than rendering continuously; background completions must wake the loop
   or be polled at a bounded interval.
6. Make headless failures and lifecycle operations noninteractive. Report errors
   through stderr/RPC and use existing explicit dirty-scene policies. Suppress
   the startup message-box fallback. Define pane-only controls as unsupported or
   harmless in this mode, and expose mode/renderer/capture capabilities in status.

The startup importer/preferences/asset-location paths also need auditing for
operation without SDL video initialization. SDL may remain linked for unrelated
platform helpers; removing it as a dependency is not necessary for headless use.

## AI inspection workflow

The proposed launch flag is the only new command in this example:

```text
woby --headless --instance review --file C:\models\part.obj
```

Launch as a background child process, then poll `woby ctl instances --json` until
this instance reports `ready`. The following commands already exist:

```text
woby ctl --instance review objects --json
woby ctl --instance review camera view isometric --json
woby ctl --instance review camera frame --json
woby ctl --instance review screenshot C:\output\overview.png --json
woby ctl --instance review camera orbit --yaw-degrees 45 --request-key inspect-angle-1 --json
woby ctl --instance review screenshot C:\output\side.png --json
woby ctl --instance review quit --on-dirty discard --json
```

The agent waits for each response, opens the returned PNG with its image tool,
chooses another camera pose or object, and repeats. Reuse a retry key only for a
retry of the same intended action. Prefer absolute camera placement for
reproducible views. Instance readiness means startup is complete, not that every
background analysis is idle. Screenshot completion already supplies the stronger
barrier needed for images.

Run one instance per independent inspection. The existing API assumes a single
coordinating client; separately submitted operations are not an atomic batch.

Useful subsequent additions:

- Per-request screenshot width/height and annotation options, with applied
  dimensions and camera metadata in the response. Current CTL has no size flags;
  it captures using `ui.screenshotSettings`, whose default is 1920 x 1800.
  The underlying capture is already configurable. Smaller AI previews require a
  deliberate change to the current minimum of 960 x 720.
- Pixel/normalized-coordinate picking so an agent can connect a feature in an
  image to an object or surface point. Existing picking code provides a starting
  point, but a CTL contract must define image dimensions, origin, analysis-panel
  offsets, and coordinate spaces. This is not required for initial camera use.
- A batch capture command or one-shot render process for offline jobs. The
  persistent process is preferable for interactive inspection because models
  and GPU buffers remain loaded between views.

## Validation performed

- `cmake --build --preset vs2026-vcpkg`: Debug build succeeded with no warnings
  emitted. This was an incremental build.
- `ctest --preset vs2026-vcpkg --output-on-failure`: all 541 tests selected by the
  default preset passed. The preset excludes tests labeled `slow`.
- `uv run tests/ctl_camera_smoke.py build/vs2026-vcpkg/bin/Debug/woby.exe`: passed
  actual PNG checks for zoom, pan, roll, look-at, camera restoration, persistence,
  object framing, and errors. This exercises the existing desktop runtime.
- `uv run tests/ctl_startup_screenshot_smoke.py ...`: four fresh desktop viewers
  produced eight successful annotated exports, including first-frame captures.
- Isolated Debug probe, compiled with `/W4 /WX`: no SDL initialization, no window,
  empty bgfx platform data, zero backbuffer dimensions, explicit 256 x 256 color
  and depth target, Woby camera math and color shaders, and asynchronous GPU
  readback. Both automatic backend choice and explicit D3D11 selected Direct3D 11.
  A red triangle covered 2,738 pixels at distance 3 and 10,952 at distance 1.5;
  restoring distance 3 reproduced the original buffer byte-for-byte.

Local probe sources and logs are under the ignored directory
`build/headless-investigation/native-probe`; annotated captures are under
`build/headless-investigation/startup-captures`. Test-suite output is in
`build/headless-investigation-tests.log`. Production source files were not changed.

## Remaining validation before shipping

The full application still needs end-to-end tests under the new launch mode:
startup/model loading, camera commands, immediate capture, analysis readiness,
labels and dimensions, save/reload, failure propagation, shutdown during pending
work, and idle CPU behavior. Verify absence of visible windows/dialogs and confirm
captures do not depend on window focus or minimization. New path-based fixtures
must share one unique temporary root per test, use explicit absolute paths, and
clean up on failure as required by `AGENTS.md`.

Windows service/noninteractive sessions, disconnected remote desktops, machines
without a hardware GPU, Linux without X11/Wayland, and macOS were not tested.
Installed Vulkan source contains a windowless path and Linux builds already
include SPIR-V shaders, so Linux/Vulkan is a reasonable next validation target.
Metal needs a separate backend probe. Do not promise cross-platform support or
pixel-identical output across different GPU drivers based on the Windows result.

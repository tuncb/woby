# Headless scripting and AI inspection

`woby --headless` starts a persistent viewer with offscreen rendering and the same
local API as the desktop application. It creates no native window, initializes no
SDL video driver, and opens no dialogs. Camera controls, models, analyses,
annotations, saved views, history, scene files, and PNG screenshots remain available.

The headless backends are Direct3D 11 on Windows and Vulkan on Linux. A working
graphics device/driver is required; this option does not supply a software renderer.
macOS headless launches currently fail with an explicit unsupported-platform error.
Without `--headless`, startup retains the normal desktop behavior.

## Agent workflow

1. Launch the viewer as a child process with a unique `--instance ID`. Use
   `subprocess.Popen` in Python, or `Start-Process -PassThru` in PowerShell.
   Do not wait for this process to exit before sending commands.
2. Poll `woby ctl instances --json` until your matching ID has `ready: true`.
   Check for process exit and enforce a startup deadline.
3. Load models and query `objects --json` for object IDs. Inspect per-file load
   outcomes: a model batch may report partial failures even when the command succeeds.
4. Set the camera, then request a screenshot. Wait for each response before
   issuing the next dependent command.
5. Open the returned PNG path with the agent's image tool. Choose another camera
   pose, object, appearance, or analysis and repeat. Camera commands return state;
   they do not return an image themselves.
6. Save the scene if needed, then call `quit`. For a disposable inspection use
   `quit --on-dirty discard`; normal `quit` refuses unsaved changes. Finally wait
   for process exit and retain the log if it fails.

Launch this process in the background:

```text
woby --headless --instance review --file C:\models\part.obj
```

After readiness, run these commands sequentially:

```text
woby ctl --instance review status --json
woby ctl --instance review objects --json
woby ctl --instance review camera view isometric --json
woby ctl --instance review camera frame --json
woby ctl --instance review screenshot C:\output\overview.png --json
woby ctl --instance review camera orbit --yaw-degrees 45 --request-key angle-1 --json
woby ctl --instance review screenshot C:\output\side.png --json
woby ctl --instance review quit --on-dirty discard --json
```

Use absolute model/output paths in harnesses. On Linux substitute Linux paths.
CLI relative paths resolve against the client's working directory; direct RPC
requires absolute paths. Quote paths with spaces.

On Windows, pipe PowerShell `ctl` output to `ConvertFrom-Json` or `Out-String` so
the shell waits before you read `$LASTEXITCODE`. The executable is a Windows GUI
binary even in headless mode. `woby --help` includes a complete PowerShell launch
and readiness example; in Python, use `subprocess.run` for each `ctl` invocation.

## Rendering and command guarantees

- `--headless` is a startup flag, not a `ctl` argument. One process keeps its
  models and GPU buffers loaded between captures.
- Discovery and `status` report `headless`; `status` also reports `renderer` and
  the current `screenshot` width, height, and format. In headless mode `pane` is
  null and `pane set` returns `-32602`. `capabilities` includes mode, renderer,
  and an `available` flag on each method.
- `ready` describes completed startup, not completion of every background job.
  Screenshot success means visible analyses were ready, rendering and GPU readback
  completed, and the PNG was written. Capture holds the command queue until done.
- Screenshots preserve the current camera. Default output is 1920 x 1800, with
  enabled helpers, surface annotations, and analysis labels, excluding desktop
  controls. There are currently no CTL width/height flags.
- Missing output directories are created, the extension is normalized to `.png`,
  and existing files are overwritten. Open the result's `path` to inspect the
  image. Use separate filenames to keep earlier views.
- Use `camera frame --object OBJECT_ID` to focus on a part, or `camera set` and
  `camera look-at` for repeatable absolute poses. Reuse `--request-key` only when
  retrying the same intended action, especially relative orbit/pan/dolly commands.
- One coordinating agent should own each instance. Distinct requests are not an
  atomic transaction. The authenticated HTTP endpoint remains local to the host.
- Failures return through stderr or RPC instead of dialogs. Graphics initialization
  failure exits with code 1. The runtime waits briefly when idle and keeps
  advancing frames while a screenshot needs GPU readback.

See the [command reference](ctl-commands.md) and [RPC contract](automation.md) for
parameters, timeout recovery, and scene lifecycle policies.

## Development verification

Unit tests cover launch parsing, mode discovery, and waking the idle runtime.
Graphics integration tests are opt-in because ordinary build machines may have
no suitable graphics driver. Enable them locally with:

```text
cmake --preset vs2026-vcpkg -DWOBY_TEST_HEADLESS=ON
cmake --build --preset vs2026-vcpkg
ctest --preset vs2026-vcpkg --output-on-failure
```

Use `ninja-vcpkg` for CI. The graphics tests use the project's selected Python
through `uv` with Pillow. `ctl_headless_smoke.py` forces an invalid SDL video
driver, unsets Linux display variables, and checks for native windows on Windows.
It verifies real pixels, first-frame labels, analysis readiness, persistence,
history, errors, idle pacing, and shutdown. `ctl_camera_smoke.py --headless`
checks camera motion against PNG pixels. All model/scene/output fixtures share
one temporary root per test and are removed even on failure.

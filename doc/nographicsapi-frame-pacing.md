# Frame pacing follow-up

Investigated on 29 September 2026 against pixel-coverage revision `4f91511`,
with historical main `97886f5` as the bgfx/D3D11 control. This continues the
[renderer investigation](nographicsapi-renderer-investigation.md).

The adopted workaround uses compatible mailbox presentation on supported
Windows surfaces, with rendering capped at twice the actual target display's
refresh rate. This bypasses the measured FIFO presentation backpressure while
bounding the extra rendering work. Unsupported surfaces and other platforms
retain their previous presentation policy; headless rendering remains unpaced.
The exact driver/compositor cause remains
unproven.

The original investigation below is retained as history, including the failed
one-frame-per-refresh mailbox experiment and its then-current recommendation
to keep FIFO. The [continued investigation](#continued-investigation-and-adopted-workaround)
records the later evidence and adopted implementation. Final builds, window
lifecycle checks, large-model controls, and all 677 Debug tests passed.

## What is established

The remaining light-scene slowdown reproduces after the pixel-coverage change.
In the first fresh empty-scene comparison, NoGraphicsAPI reached 115.63 FPS and
historical main reached 119.97 FPS. Disabling per-frame performance logging and
periodic automation polling did not remove the loss: 115.74 FPS.

The earlier "acquire wait" is mostly **presentation-context fence reuse**, not
`vkAcquireNextImageKHR`. With two presentation contexts, the measured means
after warm-up were:

| Operation | Mean milliseconds |
|---|---:|
| Presentation-context fence wait inside `gpu::acquire` | 7.818 |
| Actual `vkAcquireNextImageKHR` | 0.018 |
| Woby frame-resource timeline wait | 0.050 |
| Command recording after acquire | 0.128 |
| Submit and present | 0.246 |

With three contexts, the presentation-context wait fell to 0.018 ms while
Woby's frame-resource wait rose to 7.699 ms. The driver returned the requested
number of swapchain images: two, three, and four in their respective tests.
This rules out the driver silently retaining a two-image swapchain as an
explanation for the previous three-context result. Adding contexts does not
remove the underlying scheduling wait.

A presentation fence protects resource reuse; it is not a guarantee that the
image has appeared on screen. The Vulkan specification explicitly allows it
to signal before presentation completes. See
[VkSwapchainPresentFenceInfoKHR](https://docs.vulkan.org/refpages/latest/refpages/source/VkSwapchainPresentFenceInfoKHR.html).

## Display and driver configuration

SDL's display enumeration and actual window association identify the displays:

| Display | Desktop bounds | Refresh rate | Role |
|---|---|---:|---|
| LG TV SSCR2 | 3840 x 2160 at (0, 0) | 120 Hz | Primary; default benchmark target |
| Lenovo DisplayHDR | 2560 x 1600 at (3840, 537) | 165 Hz | Laptop panel |

The default 1280 x 720 window is entirely on the LG TV. The earlier 165 Hz versus
120 Hz discrepancy therefore describes two different displays. Moving a fresh
test instance to the laptop panel produced 112.49 FPS, with display intervals
frequently spanning two 165 Hz refresh periods. Borderless fullscreen on the TV
produced 109.26 FPS and still used `Composed: Flip`; it did not obtain independent
flip or exclusive fullscreen. Moving to the laptop also changed drawable size
to 1276 x 711 through DPI handling, so this is a scheduling diagnostic rather
than an equal-resolution rendering benchmark.

Hardware remains the RTX 3070 Laptop GPU with NVIDIA 616.92 on AC power.
`vulkaninfo` reports Vulkan 1.4.351 and support for `VK_KHR_present_id`,
`VK_KHR_present_wait`, their newer `2` variants, and `VK_EXT_present_timing`.
The experiment below used the original present-id/present-wait pair; support
alone is not evidence that a particular pacing strategy will work well.

No monitor refresh rates, driver profiles, VRR settings, or power settings were
changed during this original phase. Later temporary per-application driver
controls are recorded below; they were restored, and global settings were
unchanged. Vulkan implicit layers were disabled for benchmark processes.

## Targeted experiments

Each short run uses a fresh process, four-second warm-up and 12-second trace.
Grid, origin, and dimensions are hidden; the empty scene includes the normal
UI pane. PresentMon 2.6.0 captures only the viewer PID. Later "quiet" runs remove
per-frame logging and sample the app frame counter at the measurement endpoints.

| Variant | Application FPS | Display interval p95, ms | Mean present-to-display delay, ms |
|---|---:|---:|---:|
| Historical main, D3D11 | 119.97 | 10.47 | 38.93 |
| Current production, FIFO | 115.63 | 10.56 | 14.36 |
| Instrumented FIFO, two contexts | 113.52 | 16.64 | 14.37 |
| FIFO, three contexts | 116.34 | 11.07 | 14.09 |
| FIFO, four contexts | 118.72 | 10.95 | 13.90 |
| FIFO plus `DwmFlush` | 109.32 | 16.65 | 14.87 |
| Quiet production FIFO | 115.74 | 14.40 | 14.38 |
| Quiet FIFO, 120 FPS cap | 114.57 | 16.64 | 14.38 |
| FIFO, wait for newest present ID | 93.40 | 16.68 | 14.76 |
| FIFO, wait for previous present ID | 110.09 | 16.65 | 14.80 |
| FIFO, wait for present ID two frames back | 113.72 | 14.73 | 14.47 |
| Quiet mailbox, 120 FPS cap | 119.96 | 9.68 | 10.30 |
| Quiet mailbox plus `DwmFlush` | 165.46 | 11.24 | 9.85 |

The context-count runs vary enough that the four-context result is not proof
of a fix. Explicit present-wait adds a dependency but did not improve this
configuration. `DwmFlush` is also not a usable target-display limiter here:
mailbox rendered about 165 FPS while the target TV is 120 Hz, and many presents
were not displayed.

The first capped-mailbox result is promising: only four measured display
intervals exceeded 12.5 ms, versus 70 for quiet FIFO. However, creating the
swapchain directly in mailbox mode changed the Windows path from
`Composed: Flip` to `Composed: Copy with GPU GDI`. These are experimental results,
not a validated default or a claim of equivalent presentation behavior.

## Longer test and preserving the flip path

The surface reports FIFO and mailbox as compatible modes through swapchain
maintenance. A second experiment declares both in
`VkSwapchainPresentModesCreateInfoKHR`, creates the swapchain in FIFO mode,
and selects mailbox for presentation using `VkSwapchainPresentModeInfoKHR`.
Unlike creating directly in mailbox mode, this retained `Composed: Flip` in
the measured Windows trace. This is observed driver behavior, not a Vulkan
guarantee about which Windows path a driver must choose.

The experimental limiter uses a monotonic deadline and `SDL_DelayPrecise`.
Its rate is explicitly supplied for each test; it does not automatically follow
monitor changes. All three runs below disable per-frame logging and periodic
automation polling and use a four-second warm-up plus 30-second trace.

| Variant on 120 Hz TV | App FPS | Display interval p95, ms | Display intervals over 12.5 ms | Mean present-to-display delay, ms |
|---|---:|---:|---:|---:|
| Current production FIFO | 114.49 | 16.57 | 184 / 3415 (5.39%) | 14.57 |
| Direct mailbox, capped at 120 | 120.00 | 11.09 | 108 / 3514 (3.07%) | 10.30 |
| FIFO-compatible mailbox, capped at 120 | 120.03 | 8.43 | 53 / 3558 (1.49%) | 9.77 |

The improvement is primarily steadier delivery: the compatible-mode experiment
cut the fraction of long display intervals by about 72% and the mean measured
present-to-display delay by about 33% in this comparison. It still has occasional
long intervals. Application FPS alone would conceal that difference between
the two mailbox variants.

**The repeat did not sustain this improvement.** Another 30-second run of the
same TV configuration produced 119.98 application FPS but a 16.66 ms display
interval p95, with 314 / 3325 measured display intervals over 12.5 ms (9.44%).
That is worse than the FIFO control's tail. A refresh-rate cap alone is therefore
not sufficient: producing 120 frames per second does not ensure 120 evenly
spaced display updates. This repeat is a reason not to adopt the experiment as
the production default.

On the 165 Hz laptop panel, compatible mailbox with a 165 FPS cap produced
165.03 FPS over 30 seconds, a 6.10 ms display interval p95, and 10.24 ms mean
present-to-display delay. All 4950 recorded presents had display timing, and
none exceeded 12.5 ms. The contrast strengthens the case for investigating
display-specific synchronization; it does not prove which driver or compositor
component is responsible.

Removing the cap from compatible mailbox produced 1281 application FPS. Most
presents were not displayed. Retaining the flip path therefore does not itself
provide a useful frame limiter.

## Heavy-model control

Powerplant was retested in all three render modes with the same geometry,
camera payload, 1280 x 720 window, 4x MSAA, and 4-pixel vertex markers. Both
processes exited successfully. These use the earlier model harness's four-second
warm-up and 12-second sampling window; no display-interval trace was taken for
this control.

| Powerplant mode | Production FIFO FPS | Compatible mailbox + 120 cap FPS |
|---|---:|---:|
| Solid | 108.11 | 120.51 |
| Solid + edges | 39.09 | 39.51 |
| Solid + vertices | 38.60 | 38.78 |

The solid result reproduces the light-scene presentation limit. The edge and
vertex modes remain around 39 FPS, with roughly 25 ms of GPU rendering time.
This is not a new geometry or vertex-rendering optimization. The short
frame-counter measurement can report slightly above the configured cap;
120.51 should be interpreted as approximately 120, not a higher display rate.

## Interpretation

The measured CPU cost and GPU rendering cost leave ample headroom in an empty
scene. The evidence points to interaction between Vulkan presentation,
NVIDIA's Windows presentation path, and the two-display configuration. It does
not prove a specific driver defect or isolate mixed refresh rates as the cause.

NVIDIA explains that its DXGI-backed Vulkan path can copy presented images into
internal buffers and return the Vulkan images before display. Repeated acquired
image indices are legal and are not, by themselves, evidence of incorrect Woby
synchronization. See the
[NVIDIA explanation of image reuse](https://forums.developer.nvidia.com/t/vulkan-swapchain-fifo-stutter-due-to-incorrect-image-rotation/355848/8).
NVIDIA also recommends explicit present-wait for latency control, while the
same discussion contains unresolved multi-monitor reports. Our present-wait
experiment above did not resolve Woby's pacing on this machine. See
[NVIDIA's presentation discussion](https://forums.developer.nvidia.com/t/vsync-behaviour-with-dxgi-swapchain/372433/2).

PresentMon's display intervals and present-to-display delays are presentation
telemetry, not end-to-end input latency. In particular, the D3D11 baseline's
higher throughput comes with substantially more queued presentation delay.
See [PresentMon metric definitions](https://github.com/GameTechDev/PresentMon/blob/main/README-ConsoleApplication.md).

## Original recommendation before the continued investigation

Keep FIFO as the production default for now. Neither more presentation
contexts, `DwmFlush`, the tested present-wait strategies, nor a fixed-refresh
mailbox cap was a reliable fix for the two-monitor setup. Do not replace the
current pacing with an uncapped mailbox loop.

The next implementation experiment should use actual swapchain timing feedback
to align presentation to the target display, rather than only matching its
nominal FPS. `VK_EXT_present_timing` is available at the physical-device level
on this driver. Before using it, query the actual window surface's timing
capabilities and supported presentation stages; then track timing/clock-domain
changes when moving between displays. Keep a supported fallback and a bounded
number of outstanding frames. The
[Khronos present-timing sample](https://docs.vulkan.org/samples/latest/samples/api/swapchain_present_timing/README.html)
describes the surface queries and feedback needed for that experiment.

For root-cause isolation, the most useful external controls are a run with only
the intended monitor active and a per-application NVIDIA "Prefer native"
OpenGL/Vulkan presentation setting. Those controls were not changed in this
investigation, so the precise role of mixed refresh rates versus NVIDIA's DXGI
presentation layer remains unproven. Likewise, `VK_KHR_present_wait2` and
`VK_EXT_present_timing` were enumerated but not implemented in this experiment.

A production pacing change should be judged by repeated display-interval
traces, latency, and GPU use across both screens, window moves/resizes,
minimize/restore, and both light and GPU-bound scenes. A single FPS average is
insufficient, as the repeated 120 FPS runs demonstrate.

## Reproduction and retained evidence

Local evidence is under `build/pacing-followup` (ignored):

- `summary.json` and `analyze.py`: PresentMon interval statistics and timer means.
- Each run folder: `presentmon.csv`, `empty-baseline.json`, viewer and trace logs.
- `quiet_presentmon.py`: endpoint frame-counter sampling and display metadata.
- `graphics-instrumentation.patch`, `ngapi-instrumentation.patch`: temporary
  application and private-dependency experiments.
- `instrument.py`, `add_present_wait.py`, `add_flip_mailbox.py`: construction
  scripts for the successive diagnostic builds.
- `vulkan-summary.txt`, `vulkan-info.txt`: driver, feature and surface inventory.
- `powerplant-fifo` and `powerplant-flip-mailbox`: heavy-model A/B measurements.

The short initial runs used the existing
`build/renderer-investigation/empty_presentmon.py` harness. The longer runs used
`quiet_presentmon.py` with `WOBY_BENCH_SECONDS=30`. Experimental environment
variables were process-local: `WOBY_PROFILE_PRESENT_IMAGES`,
`WOBY_PROFILE_DISPLAY`, `WOBY_PROFILE_FULLSCREEN`, `WOBY_PROFILE_MODE`,
`WOBY_PROFILE_CAP`, `WOBY_PROFILE_DWM_FLUSH`, `WOBY_PROFILE_PRESENT_WAIT`, and
`WOBY_PROFILE_FLIP_MAILBOX`. They are diagnostic switches, not supported Woby
settings or part of the production API.

The unchanged pixel-coverage portable executable used as the production control
has SHA-256
`c02872dd91934d9f42ba6171f3ef77914666d8be86600ddb9c2536ce5cd482c2`.

## Original restoration and checks

The compatible-mailbox and present-wait experiments also completed Debug
windowed smoke runs with no Vulkan validation messages or reported present-wait
errors. Those short checks establish basic API validity, not production readiness
or reliable pacing. The final diagnostic Debug and Release builds were warning-free.

Both `src/graphics.cpp` and the private NoGraphicsAPI source were then restored
byte-for-byte from their pre-experiment copies. Full production Debug and Release
builds completed without warnings. The portable release package is unchanged.
All 668 production Debug tests passed in 148.77 seconds, including the windowed,
headless, and GPU checks, with implicit Vulkan layers disabled.
Only investigation documentation remains as a repository change; no new
production behavior or unit-test cases were added.

Validation logs are under `build/pacing-followup`: `prototype-debug-build.log`,
the two `validation-*` folders, `restored-debug-build.log`,
`restored-release-build.log`, and `restored-debug-tests.log`.

## Continued investigation and adopted workaround

Further tests isolated a useful application workaround without establishing the
precise closed-driver cause. FIFO waits remained in presentation/resource reuse
despite deeper rendering and presentation contexts, explicit waits, and queue
experiments. Selecting compatible mailbox removes this queue backpressure.
Supplying up to two completed frames per target refresh was more repeatable on
both displays than attempting to synchronize exactly one frame per refresh.

### Target timing and unsuccessful controls

`VK_EXT_present_timing` exposed a concrete timing discrepancy. With the window
on the 120 Hz LG TV, the swapchain timing properties reported 6,060,600 ns
(165 Hz), while available `FIRST_PIXEL_OUT` timestamps advanced in approximately
8,333,333 ns steps (120 Hz). The nominal swapchain timing properties therefore
cannot safely drive Woby's target-display limiter on this configuration. The
adopted limiter instead queries SDL's current mode for the display containing
the window, and updates its deadline when that rate changes.

An EXT presentation-stage timestamp of zero means **unavailable**, not proof
that the frame was dropped. Gaps between available stage timestamps and the
rate of available feedback do not establish physical display cadence. In
addition, stage-local timestamps from different stages cannot be subtracted
to calculate presentation latency. These restrictions apply to the retained
diagnostic summaries, including their `stage4_available` and
`available_feedback_*` fields. See
[VkPastPresentationTimingEXT](https://docs.vulkan.org/refpages/latest/refpages/source/VkPastPresentationTimingEXT.html)
and [VkTimeDomainKHR](https://docs.vulkan.org/refpages/latest/refpages/source/VkTimeDomainKHR.html).

The display-specific worker experiments used Windows vertical-blank waits and
tested several phase offsets. Exact-rate pacing remained inconsistent: one
precise TV run had an 8.40 ms PresentMon display-interval p95, while its repeat
had 16.58 ms despite both rendering approximately 120 FPS. A laptop run had
a 12.10 ms p95 at approximately 165 FPS. No D3DKMT or DXGI vertical-blank worker
is shipped in the adopted implementation.

Per-application NVIDIA controls also failed to resolve the tail:

| FIFO driver control on the TV | Application FPS | PresentMon display interval p95, ms |
|---|---:|---:|
| Prefer native Vulkan presentation | 120.53 | 16.66 |
| Prefer layered presentation | 115.91 | 16.65 |
| Application-controlled preferred refresh rate | 110.50 | 16.66 |
| Highest available preferred refresh rate | 115.84 | 16.69 |

The global preferred-refresh setting was already "Highest available". The
application-controlled and highest-available overrides both left the Vulkan
swapchain timing property at 165 Hz on the 120 Hz TV. All temporary application
profile changes were restored; no global driver, monitor, VRR, or power setting
was changed. These controls do not prove that a particular NVIDIA setting or
presentation layer is the underlying cause.

### Adopted implementation

Windows windowed devices explicitly opt into compatible mailbox presentation.
The dependency queries the surface's supported modes and FIFO compatibility
before declaring FIFO and mailbox in `VkSwapchainPresentModesCreateInfoKHR`.
It creates the swapchain as FIFO and requests mailbox per present through
`VkSwapchainPresentModeInfoKHR`. The measured Windows path remains
`Composed: Flip`; this is an observation on this driver, not a Vulkan guarantee.
If the required support is absent, the implementation retains FIFO. Headless
devices remain unpaced; non-Windows presentation is unchanged.

The pure `frame_pacing` policy caps application rendering at twice SDL's actual
target-display refresh rate: 240 FPS for this TV and 330 FPS for this laptop
panel. Invalid or unavailable rates fall back to 60 Hz, producing a 120 FPS
cap. The accepted display-rate range is 20–1000 Hz. Integer nanosecond deadlines
advance from the prior planned deadline during normal operation, avoiding
cumulative sleep drift. Rate changes reset the schedule. Late frames run
immediately and reset the next deadline, avoiding catch-up bursts.

The runtime applies `SDL_DelayPrecise` after GPU resource reuse and image
acquisition. A GPU-bound frame that has already exceeded its deadline incurs
no extra limiter wait. Minimized/hidden windows stop requesting mailbox and
use a 20 FPS idle cap: a lifecycle check found that inactive FIFO retirement
alone could otherwise run thousands of frames per second. Returning to an
active window resets the normal deadline immediately. All temporary
`WOBY_PROFILE_*` hooks and timing
instrumentation have been removed from the production code.

The tradeoff is additional CPU/GPU work in light scenes, bounded to two rendered
frames per nominal target refresh. This is not a geometry optimization or a
claim that the monitor displays every rendered frame. It deliberately leaves
more than one completed-frame opportunity per refresh to avoid the unstable
phase relationship seen with the exact-rate experiments.

### Repeated bounded-mailbox measurements

Fresh-process captures with the compatible mailbox path produced the following
results. Counts include positive, available PresentMon display-change intervals;
they are recorded presentation events, not independent physical scanout proof.

| Target and cap | Capture, seconds | Application FPS | Recorded display intervals | Mean display interval, ms | Display interval p95, ms |
|---|---:|---:|---:|---:|---:|
| TV, 240 FPS | 30 | 239.98 | 3598 | 8.33355 | 8.3738 |
| TV, 240 FPS repeat | 40 | 239.98 | 4798 | 8.33354 | 8.3964 |
| Laptop, 330 FPS | 40 | 329.87 | 6596 | 6.060615 | 8.7633 |
| Laptop, 330 FPS repeat | 40 | 329.98 | 6599 | 6.060616 | 8.4595 |

The TV traces sustain an approximately 8.33 ms mean and p95 on both runs.
Laptop means match approximately 6.06 ms, but the tails contain non-quantized
jitter: the 8.46–8.76 ms p95 values are not a clean multiple of the nominal
refresh interval. PresentMon observations support this application workaround;
they do not establish exact physical scanout timing or end-to-end input latency.

### Continued evidence and validation status

The final implementation's Debug and Release builds completed without warnings.
All 677 Debug tests passed in 152.56 seconds, including eight new deadline/idle
pacing cases and the new headless mailbox fallback test. All 15 focused Release
pacing, native-renderer, picking, and vertex-render checks passed in 16.27 seconds.

Release and Debug lifecycle checks passed moving the ordinary production binary
between the TV and laptop, resizing, returning to the TV, minimizing, restoring,
and exiting cleanly. These checks used owned-window Win32 operations and the
public control API, with no profiling hooks. The final Release capture on the TV
had an 8.36 ms display-interval p95. Subsequent TV lifecycle segments had
8.37–8.40 ms p95 and no intervals over 1.5 target refresh periods. Laptop segments
retained a 6.06 ms mean, with the mixed-monitor timing caveat above. Minimized
cadence measured 19.93 FPS in Release and 20.35 FPS in Debug; before the idle fix
the minimized loop reached approximately 4185 FPS. Debug lifecycle logs contain
no Vulkan validation warnings or errors.

The 0.818 GB Powerplant control used the same 12.76-million-triangle scene,
camera, 4x MSAA, pane, and measurement harness on this machine:

| Mode | Pixel-coverage FIFO control, rendered FPS | Final pacing, rendered FPS |
|---|---:|---:|
| Solid | 107.36 | 239.92 |
| Solid + edges | 39.68 | 40.05 |
| Solid + vertices | 38.88 | 39.03 |

The solid result reflects the different presentation policy and 240 FPS rendering
cap, not a geometry speedup or a 240 Hz TV output. The GPU-bound modes show no
material regression. Their final median GPU times were 24.35 and 24.88 ms.

The portable build is
`build/frame-pacing-fixed/woby-windows-x64/woby.exe`, with archive
`build/frame-pacing-fixed/woby-frame-pacing-windows-x64-release.zip`.
All 74 packaged files were verified against manifest sizes and SHA-256 hashes;
the portable executable also passed a fresh window/control/clean-exit smoke run.
Executable SHA-256:
`2c313ba0515c7f2667d47c32d0039a8d146b880a521d9e26394a121f98a27a96`.

Further local evidence is under `build/pacing-fix` (ignored):

- `mailbox-tv-cap240`, `mailbox-tv-cap240-repeat`,
  `mailbox-laptop-cap330`, and `mailbox-laptop-cap330-repeat`: the repeated
  bounded-mailbox captures, endpoint FPS measurements, and viewer logs.
- `timing-validation`, `relative120-tv`, `hint-default`, and
  `hint-disallowed`: EXT timing queries and target-display experiments.
- `native-driver`, `layered-driver`, `refresh-application`, and
  `refresh-highest`: temporary per-application driver controls;
  `nvapi-backup` and `refresh-profile-backup` retain restoration evidence.
- `vblank-phase*`, `worker-fixed-*`, and `precise-*`: discarded display-clock
  and phase experiments; these are not production dependencies.
- `fifo-six-both`, `fifo-second-queue`, `acquire-readiness`, and `split-*`:
  buffering, queue, and wait-placement controls.
- `summary.json` and `analyze.py`: diagnostic summaries, including the explicit
  unavailable-stage-timestamp caveat. Display interval p95 above uses the
  analyzer's nearest observed percentile.
- `graphics-complete-experiments.cpp` and `ngapi-complete-experiments.cpp`:
  retained diagnostic source snapshots.
- `final-debug-build.log`, `final-release-build.log`, `final-debug-tests.log`,
  and `final-release-focused-tests.log`: final warning-free build/test evidence.
- `final-tv`, `verified-window-lifecycle`, `verified-debug-window-lifecycle`,
  and `portable-smoke`: final production-binary timing and lifecycle checks.
- `final-powerplant` and `control-powerplant`: large-model A/B measurements.

# GPU marker hover picking

The viewport uses visible-marker ID picking on supported graphics backends.
The normal point draw writes its color and a compact RGBA8 identity attachment.
A compute pass examines a 7×7 neighborhood within three pixels of the pointer,
including each MSAA sample. The selected marker is highlighted in the same GPU
frame. Fully hidden and fully transparent markers cannot be selected.

Each submitted point draw gets a distinct range of IDs, including multiple files
and repeated scene instances. No spatial index is constructed. Selection uses
the nearest covered pixel center, with the lowest ID breaking ties. Surface and
edge fragments clear marker IDs where they cover the scene. For translucent
overlap, the last depth-passing fragment owns the sample; blended pixels can
still contain color from earlier markers whose IDs were overwritten.

Coordinates arrive independently through an eight-slot readback ring. Requests
retain draw ranges and transforms, without borrowing mesh pointers. After three
application-frame advances, the readback is requested from bgfx; only completed
results are inspected. A busy ring skips coordinate requests while highlighting
continues. Scene edits, replacement, disabling hover, and viewport resizing
invalidate previous requests. File identities and bounds are checked before
resolving coordinates. The coordinate panel can lag the current highlight.
The pinned D3D11 backend may still block internally during readback; no explicit
application wait is introduced into the frame loop.

Runtime resources live in `GpuMarkerPicker`, outside `UiState`. The picker is
enabled while the pointer is available over a viewport with vertex markers.
Hover picking pauses during camera navigation (including wheel zoom and keyboard
movement) and resumes after 150 ms without movement. Active orbit, roll, and pan
drags keep it paused. Both GPU and CPU picking use this gate; queued coordinate
results are invalidated while paused. Other rendering and screenshot exports use
their original shaders. Comparison rendering clears the ID attachment when it
covers a marker. Screenshot views are separate from the picking passes.

The GPU path requires compute, independent blending, multiple render targets,
texture blit/readback, and a top-left texture origin. Unsupported backends and
allocation/setup failures retain the existing CPU hover picker. Bottom-left
backends remain on that fallback until their orientation is validated.

The previously measured transparency cost is retained in this implementation;
performance improvements are deferred. See the historical
[experiment report](marker-picking-experiment.md) for the measurements.

`marker_pick_tests.cpp` covers identity ranges, multi-model coordinate resolution,
snapshot transforms, stale results, frame-counter wrap, and capability fallback.
With `WOBY_TEST_HEADLESS=ON`, `marker_pick_gpu_tests.cpp` also exercises the actual
rendering/lookup/highlight/readback path, including IDs above 2²⁴, MSAA, alpha,
occlusion, resizing, and cancellation while a result is in flight.

# Item 12: typed graphics parameters

Baseline: `46a27585c5bc8638a8bde5d601e5cead71376631`. This investigation changes
research targets and documents. Application behavior is unchanged.

## Decision

Implement typed setters as a small adapter cleanup. A uniform-name change can
proceed independently of an integer shader-layout change. Do not set a frame
performance target from the dispatch probe. Merge the shader-layout change only
after graphics readback tests pass with the new layout.

## Current work and actual contract

`graphics::setUniform` in `src/graphics.cpp` obtains a shared uniform resource,
validates its type/count, compares its name with up to eight strings, validates
the destination capacity, and copies 16 or 32 bytes. There is no uniform-name
hash table and no per-call string allocation. The name exists in a retained
`Uniform`; short names use the implementation's small-string storage. The
resource lookup copies a `shared_ptr`. Typed setters can remove the handle
lookup, reference-count changes, name comparisons, and invalid name/count/type
combinations.

Call sites include source surfaces, point sprites, imported line sprites,
procedural triangle edges, analysis display, marker lookup/highlight, and
annotations. The ImGui sampler handles are accepted by `setTexture` but that
function ignores the sampler handle. Texture ownership is obtained separately
and must remain. Uniform cleanup does not remove texture ownership.

`WobyRoot` is already typed. Its x64 size is 344 bytes, with static offset
assertions in `shaders/native/root.h`. Submission copies encoder state into an
operation, resets the encoder, later builds `rootData`, and allocates the root
in retained GPU-visible frame storage. Typed writes must retain this sequencing.
Writing a live GPU root from UI code would violate the current ownership and
frame model.

The shader reconstructs point offsets, marker IDs, marker source IDs, and
triangle offsets from two float halves. This encoding is exact for every
`uint32_t`: each half is at most 65,535 and is exactly representable as float.
The change removes packing and decoding; it does not fix a demonstrated ID
precision defect. A single float per ID would introduce such a defect above
16,777,216. The probe checks 0, 1, 65,535, 65,536, 16,777,217,
2,147,483,648, and 4,294,967,295.

## Measurement scope

`experiments/simplification-12/main.cpp` measures actual production `setUniform`
with the Noop adapter, against an isolated, out-of-line typed packet setter.
Each packet writes color, two point vec4s, UV grid, edge parameters, and marker
base: five calls and 96 payload bytes. It uses 10, 1,000, and 20,000 packets,
five alternating repeats, and prepacked inputs. It includes resource dispatch
and copies. It excludes legacy float encoding, draw submission, matrix
composition, root staging, pipelines, driver work, GPU work, and frames.

The candidate sets one packet. That is a possible typed batch API, but its
timing is a lower bound on an integrated replacement. A fine-grained API with
five out-of-line setters will have a different cost. These counts are workload
scenarios, not measured packet counts from the viewer. The tiny scenario bounds
whether this change matters for ordinary small scenes; the dense scenario shows
where dispatch may be measurable.

The serial Release run used a Ryzen 7 5800H and MSVC 19.51.36243.0 with
`/O2 /Ob2 /DNDEBUG`. Values below are milliseconds per whole workload, with
the median and full range of five samples. Each sample is a normalized batched
average: time many rounds, then divide by rounds. The rounds count is recorded
in the raw data; these are not single-frame latency samples.

| Packets | Current five writes | One typed packet |
| ---: | ---: | ---: |
| 10 | 0.001737 (0.001728–0.001760) | 0.0000471 (0.0000469–0.0000476) |
| 1,000 | 0.170750 (0.170522–0.171689) | 0.004698 (0.004669–0.004871) |
| 20,000 | 3.473590 (3.433809–4.148614) | 0.098717 (0.096540–0.114591) |

At 1,000 packets the current boundary costs about 34 ns per uniform write.
At 10 packets the whole current boundary costs only 1.74 microseconds.
At 20,000 packets a few milliseconds of dispatch can matter, but that scene
must actually issue these five writes per packet. A typical surface sets fewer
fields; this probe combines surface, point, edge, and marker settings in one
packet. The isolated typed packet is 72 bytes and uses one call, compared with
five calls and 96 payload bytes. The difference therefore bounds a batch API;
it does not predict the saving of five typed setters or a complete viewer frame.
The wide last dense sample is retained in the raw data.

The field/ID contract passed in Debug and Release, including full packet content
and exact reconstruction of the uint32 boundary cases. Raw samples are in
[simplification-12-results.json](simplification-12-results.json). The shared
[run metadata](simplification-12-14-15-run-metadata.json) records machine,
compiler flags, timestamps, source/generated-copy hashes, and executable hashes.
Reproduce with `experiments/simplification-12/run.ps1` after building the optional
Release probe target; its output guard requires a fresh output path for reruns.

## Memory

Deleting uniform resources saves a small number of objects, short names,
shared-pointer control blocks, handle slots, and caller handles. The normal
startup has six creation sites: four main data uniforms, one triangle-edge
uniform, and the ImGui sampler. The lazy marker picker adds six more. This is
a fixed application resource count; it does not scale with mesh vertices. The
current root remains 344 bytes if fields retain
their old slots. Replacing point offset/source halves with two uints saves
eight logical bytes; a uint marker base saves twelve; a uint triangle offset
saves four. A compact layout can save up to 24 logical bytes before padding.
This is a layout calculation, not a measured process saving. The adapter's
`allocate` rounds each request to 16 bytes, so the existing 344-byte root uses
352 arena bytes. The 320-byte candidate is a layout estimate; this investigation
has not compiled that shared C++/Slang root or validated its shader ABI.
A replacement that later verifies at 320 bytes would use 320 arena bytes:
32 fewer allocated bytes and 24 fewer copied bytes per root. At 20,000 roots,
that is 625 KiB less arena use per submission frame. Actual heap commitment
changes in page increments, and frame pages are retained for reuse. The new
layout needs C++ offset assertions, shader layout/reflection checks, and render
ID readback checks; a name-only setter change keeps
the current size and arena use.

## Implementation and merge checks

1. Add structs and free setters to `graphics.h`, owned by runtime code. Keep
   `UiState` free of graphics handles and root state.
2. Change callers and remove name-based resources. Keep typed validation for
   values whose range affects GPU indexing; compile-time type safety does not
   prove a buffer index is in range.
3. Change C++ and Slang layout together. Assert every offset and total size.
   Compile and validate SPIR-V; generate and compile Metal on Apple hardware.
4. Test the marker boundary values above, point/source offset differences,
   compact and indexed points, imported lines, procedural edges, quality/UV/
   distance display, transparency, and export. Check reset-after-submit with
   consecutive draws that set different fields.
5. Run the existing graphics, marker-pick, adaptive-point, scene-edge,
   scene-renderer, and analysis-display readback tests. Measure complete CPU
   submission only if dispatch savings are needed to justify the change.

The API cleanup has low numerical risk. The integer/layout phase has a wider
validation surface. The two phases should have separate review and validation.

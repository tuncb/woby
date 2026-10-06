# Merge validation — 2026-10-06

**Promotion:** The unified loader now replaces legacy loading in normal builds.
The application and CI opt-in switches have been removed. The results below
record the pre-promotion validation, when both switch settings were available.

The code candidate is `f7f5d6fc0a1f3e4b76457185a5be5110b1b07124` on `codex/issue-112-rapidobj-prototype`.
It includes main at `15376d3`, resolves the CMake test-target conflict by retaining
both sets of targets, and keeps `WOBY_RAPIDOBJ_PROTOTYPE` OFF by default.
The checks below support merging the opt-in implementation.

## Application builds and tests

The application builds below used the Visual Studio 2026 + vcpkg preset with workflow
profiling and benchmarks OFF. Debug builds and the default-parser Release
build completed without compiler warnings. The local suites include all slow
and GPU/rendering tests; they were not limited to the preset’s fast subset.

| Local configuration | Passing tests |
| --- | ---: |
| Debug, prototype ON (integrated main) | 992 |
| Debug, prototype OFF | 992 |
| Release, prototype OFF | 992 |
| Affected rendering tests after final Metal fix | 12 |

The integrated prototype-ON suite preceded the renderer portability fixes.
Its affected point-rendering tests were rerun after those fixes. The last
Metal reference correction produces byte-identical Vulkan shaders; rebuilding
both local configurations and rerunning the 12 affected tests also passed.

Release CI uses Ninja + vcpkg and tests the final code revision on all three
platforms with each application parser setting:

| Parser setting | Windows x64 | Linux x64 | macOS ARM64 |
| --- | --- | --- | --- |
| [ON](https://github.com/tuncb/woby/actions/runs/37493965429) | 961 passed | 952 passed | 953 passed |
| [OFF](https://github.com/tuncb/woby/actions/runs/37493971391) | 961 passed | 952 passed | 953 passed |

CI runs its development and slow suites. Hardware-dependent headless rendering
is OFF in CI; Windows GPU coverage comes from the local runs above. Cross-platform
build/test success is not a cross-platform performance measurement.

The Linux and macOS Release logs contain warnings in dependencies and unchanged
main code/tests (aggregate initializers, GCC string-concatenation diagnostics,
Metal vertex-resource warnings and duplicate SDL linkage). Their warning sets
are identical with the prototype ON and OFF; none points to the parser or its
new tests. These CI builds passed, but are not claimed to be warning-free.
The local Windows Debug/Release builds above are warning-free.

## Memory safety and concurrency

The new standalone `woby_rapidobj_stress_tests` passed on Windows and under
GCC 14.2 in Ubuntu/WSL with AddressSanitizer + UndefinedBehaviorSanitizer
(including leak detection), then separately with ThreadSanitizer.
Each sanitizer run passed all three cases and 33,500 assertions without a
sanitizer diagnostic. The parser headers and stress source did not change
during integration or the renderer fixes.

The seeded cases compare sequential memory input with native file reads using
1, 2, 4, and 16 workers and both block/range paths. They cover long-record
fallback, continuations, CRLF, missing final newline, weighted/freeform records,
negative indices and metadata. Separate cases check independent first-error
line expectations at read boundaries and repeated cancellation followed by
caller-buffer reuse. Every fixture uses its own absolute temporary root and
RAII cleanup.

## Worker-count sensitivity

Five fresh processes per parser/model/worker setting: **120 processes**, zero
excluded samples, matching ordered fingerprints, and zero regression flags.
Explicit worker limits apply to both parsers. The legacy limit is injected
only into the generated benchmark header because its public API has no worker
option. The default setting preserves each parser’s normal worker selection.
Inputs were warmed and runs gated by two CPU samples at or below 15%.

| Worker limit | Model | Legacy median | Prototype median | Prototype / legacy |
| --- | --- | ---: | ---: | ---: |
| 1 | quads | 87.77 ms | 91.60 ms | 1.044× |
| 1 | lines | 44.36 ms | 41.90 ms | 0.945× |
| 1 | cloud | 2406.37 ms | 2301.39 ms | 0.956× |
| 2 | quads | 53.17 ms | 50.18 ms | 0.944× |
| 2 | lines | 26.18 ms | 22.32 ms | 0.853× |
| 2 | cloud | 1234.76 ms | 1133.56 ms | 0.918× |
| 4 | quads | 33.92 ms | 32.23 ms | 0.950× |
| 4 | lines | 16.44 ms | 14.97 ms | 0.911× |
| 4 | cloud | 661.28 ms | 628.43 ms | 0.950× |
| Default (16 here) | quads | 27.13 ms | 21.02 ms | 0.775× |
| Default (16 here) | lines | 14.30 ms | 12.76 ms | 0.892× |
| Default (16 here) | cloud | 346.10 ms | 333.56 ms | 0.964× |

Single-worker quads are 4.4% slower; all other measured combinations are faster.
This meets the existing 5% / 0.1 ms tolerance. Results describe medians;
individual invocations vary. Worker limits on one machine are not substitutes
for measurements on separate low-core hardware. Cold/network storage and other
CPUs remain unmeasured.

The production parser hashes match the earlier complete 180-process, 12-model
campaign. Integrating main and fixing renderer portability did not change them.
See [raw worker measurements](worker-scaling.json) and
[validation metadata](merge-validation.json).

## Issues found during validation

- The first stress fixture used an unsupported oversized pure comment. It was
  corrected to a supported long trailing comment. A stale Release object from
  an overlapping compile was rebuilt; subsequent tests pass.
- One UV screenshot run timed out while local compilation was active. It passed
  within the original timeout when rerun; all integrated full suites later pass.
- The older branch lacked main’s `<cstring>` fix for the Linux graphics backend.
  That existing fix was brought in before main was integrated.
- Main’s new Metal point shaders tried atomic operations on vector elements and
  requested a value-returning 64-bit max. Scalar fields retain the eight-byte
  scratch layout, and narrow Metal intrinsics select the supported operations.
  A reference-emission mistake in the first helper was corrected before the
  final successful CI run. Vulkan’s path retains the normal Slang intrinsic.

The Metal mapping uses [Slang’s target-specific interoperation mechanism](https://shader-slang.org/slang/user-guide/a1-04-interop.html). The void-returning
64-bit max is also documented in the [GPUWeb 64-bit atomic proposal](https://github.com/gpuweb/gpuweb/blob/main/proposals/atomic-64-min-max.md).

No parser correctness or performance fix was required by this final validation.

## Reproduction

```powershell
cmake --preset vs2026-vcpkg -DWOBY_RAPIDOBJ_PROTOTYPE=ON -DWOBY_PROFILE_LOAD_WORKFLOW=OFF -DWOBY_BUILD_RAPIDOBJ_PROTOTYPE_BENCHMARK=OFF -DWOBY_TEST_HEADLESS=ON
cmake --build --preset vs2026-vcpkg --config Debug
ctest --test-dir build/vs2026-vcpkg -C Debug --output-on-failure -j4
# Repeat with the prototype OFF and with --config / -C Release.
# Enable the benchmark target, build Release, and run on an idle machine:
uv run experiments/parser-throughput/run.py PATH/TO/woby_parser_benchmark.exe workers-1.json --only quads lines cloud --rounds 5 --workers 1 --max-background-cpu 15
# Repeat with --workers 2, 4, and 0.
```

For the standalone Linux sanitizer target, compile `tests/rapidobj_stress_tests.cpp`
as C++20 with the vendored parser and doctest include directories, `-pthread`,
`-O0 -g -fno-omit-frame-pointer -fsanitize=address,undefined`; run with
`ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1`.
Recompile separately with `-fsanitize=thread` and run with
`TSAN_OPTIONS=halt_on_error=1`. Sanitizer checks used GCC because this WSL
installation’s Clang lacked its sanitizer runtime libraries.

## Promotion to the standard loader

The application now routes both `loadObjMesh` and `loadObjMeshText` directly to
the unified loader. The CMake option and CI input selecting the experimental
path have been removed. Explicit legacy entry points remain for comparison
tests and benchmarks, and the workflow instrumentation still generates its
legacy comparison path.

The new standard-loader regression case checks mixed polygons, lines, points,
and weighted trimmed freeforms through both public entry points against the
reference loader. With no parser-selection definition, all **993 tests passed
in both Debug and Release**, including 11 graphics tests and four slow tests.
Both application builds completed without compiler warnings. The workflow
generator, its 12 analysis tests, four parser-analysis tests, and 19 CI helper
tests also passed.

The production parser headers are unchanged from the validated candidate above;
the promotion changes application dispatch and build configuration.

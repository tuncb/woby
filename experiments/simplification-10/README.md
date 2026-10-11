# Exact intersection predicate experiment

The probe keeps the production BVH, conservative disjoint filters, budgets,
finding order, and report geometry. CMake copies `src/mesh_intersections.cpp`
to the build directory and adds counters and a candidate selection switch.
The application uses its original translation unit.

The candidate uses adaptive `orient2d` and `orient3d` from the installed CDT
`predicates.h`. It constructs no intersection coordinates. Inputs outside
the conservative nonzero-coordinate range `[2^-200, 2^200]` use the existing
rational implementation. Zero coordinates are allowed. The candidate and
oracle source files (`predicate.cpp` and `main.cpp`) use `/fp:strict` on
MSVC. The generated production wrapper uses `/fp:precise`, as in the
production build. This keeps the conservative filters at their normal
cost. The optional FMA contract also selects `/arch:AVX2`. Adaptive
arithmetic assumes round-to-nearest. The contracts do not vary the process
rounding mode; a production worker must verify it or use rational fallback.

The self-test forces the unchanged rational clipping path as an oracle. A
separate rational separating-axis implementation checks closed intersections
without shared IDs. It also checks raw adaptive orientation signs directly
against rational determinants and an independent integer determinant
oracle that decodes the input double bits.

From the repository root:

```powershell
cmake --preset vs2026-vcpkg -DWOBY_BUILD_SIMPLIFICATION_PROBES=ON
cmake --build --preset vs2026-vcpkg --config Release --target woby_simplification_10_probe
build/vs2026-vcpkg/bin/Release/woby_simplification_10_probe.exe --self-test
experiments/simplification-10/run.ps1 -Runs 5
experiments/simplification-10/run.ps1 -Model C:/work/temp/uploads_files_2720101_BusGameMap.obj -Runs 3
```

On a CPU with AVX2, configure with
`-DWOBY_BUILD_SIMPLIFICATION_FMA_CONTRACT=ON` and build
`woby_simplification_10_fma_contract`. Run it with `--self-test` to check
the FMA implementation. This target is off by default.

Do not run builds, tests, or other benchmarks during timed runs. Result
comparison and allocation passes are outside the timings. Synthetic runs
are unlimited and must finish with complete results. The real-model run
uses production default budgets and records truncation explicitly. It also
runs a fixed 25,000-face prefix in each topology mode without either budget.

Allocation values are requested C++ heap payload. They measure allocation
traffic, live scratch, and retained result payload in this executable.
They are not process working set or allocator reserved memory. Recording
is disabled during timings, but all three variants use the same replacement
allocator. A normal-allocator application check is required before merge.

The measured raw results, hashes, and compiler-scope pilot are retained in
[the investigation data](../../doc/simplification-10-predicates-results.json).
The measured source is frozen in that data's build metadata. The later
`CMAKE_CONFIGURE_DEPENDS` entry only makes a production source edit trigger
regeneration of the wrapper. It did not change the measured binary.

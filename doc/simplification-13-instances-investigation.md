# Item 13: distance queries in local coordinates

Decision: consider a distance-only cache if repeated placement changes are a measured bottleneck, after selecting and validating its numerical contract. Do not replace world-space queries for all accepted inputs. The prototype finds a concrete numerical change, so it is not ready for a production merge.

This investigation applies to commit `46a27585c5bc8638a8bde5d601e5cead71376631` on 11 October 2026. Item 13 is the working number for the unnumbered local-coordinate proposal.

## Measured boundary

The [probe](../experiments/simplification-13/main.cpp) includes the production distance implementation. Both routes use its unchanged tree builder, nearest traversal, degeneracy test and triangle-distance function. The baseline uses `comparisonWorldMesh` to bake selected parts, then builds one world tree. The candidate builds one source-local tree, transforms queries by the inverse similarity matrix and scales squared distances back. Each instance has a padded experimental world box. A linear box scan selects instances; this is not an implemented top-level hierarchy.

The synthetic input has 12,800 triangles and 6,561 vertices. It uses one, eight and 32 copies of the same geometry. A real BusGameMap input tests one rotated instance at scales 1 and 0.01. Each case warms both paths and alternates build and query order. Array capacity bytes exclude input geometry, allocator overhead and process memory. Both routes are resident during the sample; their process working set cannot be attributed separately.

The probe reports the initial uncached instance metadata cost separately. Its simple setup scans all source vertices for each instance. A production implementation must compute local bounds once per geometry owner and cache them. Excluding this setup from repeated query timings is valid only after that cache exists.

See [raw measurements and hashes](simplification-13-results.json). The script is [run.ps1](../experiments/simplification-13/run.ps1).

## Results

Times below are medians in milliseconds. Synthetic cases use seven alternating samples and 4,096 nearest queries. BusGameMap cases use five samples and 2,048 queries. Preparation columns are separate stages; their median sum is not a measured full-comparison time.

| Input; instances; scale | World bake | World tree build | Local tree build | World queries | Local queries | Initial uncached instance metadata |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Synthetic; 1; 1, identity rotation | 1.055 | 11.287 | 11.274 | 6.482 | 6.427 | 0.158 |
| Synthetic; 1; 0.5, rotated | 1.083 | 10.621 | 11.013 | 8.159 | 6.327 | 0.087 |
| Synthetic; 8; 1, rotated | 7.722 | 97.356 | 11.031 | 8.733 | 17.067 | 0.699 |
| Synthetic; 32; 1, rotated | 29.975 | 421.322 | 11.225 | 10.025 | 52.137 | 2.738 |
| BusGameMap; 1; 1, rotated | 78.314 | 1249.311 | 1236.887 | 14.283 | 11.915 | 7.857 |
| BusGameMap; 1; 0.01, rotated | 79.837 | 1245.705 | 1233.272 | 14.104 | 11.844 | 7.824 |

One cold instance has similar tree construction cost. The rotated Bus query boundary is about 16.6% faster locally, but its cold tree still takes about 1.24 seconds. A retained tree could avoid that construction after placement-only changes; these timings do not implement or measure the application's cache publication and invalidation path.

Repeated instances reduce construction substantially, but this candidate's queries regress. Eight instances take 1.95 times the world-route query time; 32 take 5.20 times. Using the stage medians and the same query distribution, the cold preparation saving is consumed after roughly 46,000 queries for eight instances, or 43,000 for 32. These are interpolation estimates, not measured full-comparison break-even points. A full comparison may issue enough queries to lose the cold preparation gain. A top-level tree can reduce the linear instance search, but overlapping instance bounds can still require multiple local-tree queries; its benefit is not established here.

The 32-instance query ranges are 9.77–10.95 ms for world space and 51.56–56.34 ms locally. Its tree-build ranges are 420.05–425.27 and 10.97–11.68 ms. Bus at scale 1 spans 13.84–15.31 ms for world queries and 11.83–12.17 ms locally; tree construction spans 1237.41–1335.54 and 1228.07–1253.49 ms. The repeated-instance query regression and its construction saving both persist across the observed samples.

| Input; instances | World tree MiB | One local tree MiB | Avoidable world-bake payload MiB | Instance-array bytes |
| --- | ---: | ---: | ---: | ---: |
| Synthetic; 1 | 1.319 | 1.319 | 0.522 | 184 |
| Synthetic; 8 | 8.594 | 1.319 | 4.228 | 1,656 |
| Synthetic; 32 | 34.375 | 1.319 | 16.755 | 7,728 |
| BusGameMap; 1, either scale | 96.546 | 96.546 | 78.431 | 184 |

At 32 instances, tree-array payload alone falls by 96.2%, from 34.375 to 1.319 MiB, plus the instance array. Including the removable bake would give a larger saving, but that bake remains necessary if another consumer still uses it. These are array-capacity bytes, with no allocator or input-mesh accounting. Retaining a local cache also keeps that tree alive between jobs.

Ordinary samples have maximum absolute distance differences from `3.89e-16` to `5.52e-12`. No sampled query changes classification at tolerance 0.25. However, 3,629–3,982 of 4,096 synthetic queries and 2,035 or 2,047 of 2,048 Bus queries differ bit for bit. The maximum error is small for these views, but the distance distribution is not identical. The extreme counterexample below changes the distance by one full unit.

Production `compareSurface` retains double distance samples for statistics; its GPU face samples are separately cast to float. `surfacePercentAboveTolerance` compares retained doubles with a double argument. The scene tolerance field is float, normalized to 0 through `1e12`. Thus GPU quantization does not remove the statistics-contract question. The 0.25 test establishes that one sampled threshold agrees; it does not prove all accepted scene thresholds or the public double-tolerance API agree. Complete comparison samples and their area weights remain an integration gate.

## Numerical blocker

For a unit triangle translated by `1e16`, world baking rounds its three distinct vertices to one position. A query at `(1e16 + 2, 1e16, 0)` has world-route distance **2** and local-route distance **1**. The self-test requires both values. Finite scene translations are not restricted to a small coordinate range. A nearby representable float translation has the same loss of unit detail.

This is an accepted-input behavior change, rather than a tree traversal error. The local route describes the mathematical transformed triangle more accurately, but the current route measures the rounded world triangle. Rotation and scale also change rounded midpoint and degeneracy calculations. Ordinary samples can agree to a small error and still change a strict `distance > tolerance` decision when the tolerance lies between the two values. A fixed 0.25 tolerance with no observed flips does not prove equivalence for arbitrary tolerances.

The ordinary-range box padding is an experiment, not a proved interval bound. Before production use, derive outward rounding for composed transforms and reject unsupported exponent ranges. Keep a world-route fallback if the current numerical contract must remain unchanged. Alternatively, approve and document a change in the distance definition, then revise the independent expected results.

## Memory and reuse limits

A local tree for one cold instance has the same triangle and node payload as a world tree. Its advantage comes from avoiding a bake, sharing the tree among repeated instances, and retaining the tree across placement changes. Retaining that cache also adds memory while analysis is idle: the current job tree is temporary. Use an explicit cache budget and eviction policy.

World meshes can still serve display, picking, heat maps, dimensions and other detectors. Savings that include removal of baked world geometry are an upper bound until those consumers use source geometry and instance records. Tree-only savings are separately meaningful. A source file with many independently transformed groups needs a tree key for membership and ranges, rather than one whole-file tree. Several unrelated source meshes also need separate local owners.

Exact-position welding, cross-part intersections and source batching do not follow from distance invariance. Keep those operations in their current world-coordinate contract. The [PBRT transformed-primitive discussion](https://www.pbr-book.org/4ed/Primitives_and_Intersection_Acceleration/Primitive_Interface_and_Geometric_Primitives) provides a primary example of local queries and shared geometry; its ray contract does not prove Woby distance equivalence.

## Implementation gates

1. Limit the route to distance queries over validated similarity transforms. Include geometry owner, selected group membership, source ranges and degeneracy policy in the cache key. Build on a worker and publish only to a matching request.
2. Choose the numerical contract and test extreme translation, scale, cancellation, degenerate triangles and tolerances beside the decision boundary. Retain an independent world-route oracle.
3. Cache local bounds and source trees. Measure cold preparation, repeated placement changes, retained idle memory, peak job memory and eviction separately. Do not include removal of display geometry until it is removed.
4. Replace the linear instance scan only when many-instance queries justify it. Preserve previous-face hints and source IDs. Check full comparison distributions, areas, finding order, cancellation and late publication.
5. Measure the complete comparison, rendering and CPU pick paths before a merge. This probe measures a feasibility boundary and nearest-query stages only.

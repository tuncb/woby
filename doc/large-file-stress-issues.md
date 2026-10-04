# GitHub work from the large-file stress baseline

Created on 4 October 2026 from the [measurement report](large-file-stress-results.md). The [GitHub tracker #90](https://github.com/tuncb/woby/issues/90) records shared evidence, priorities, scope boundaries and the suggested work order. All 20 focused issues carry the `performance` label plus `bug` or `enhancement`.

The original baseline archive is unchanged. New issue drafts and publication receipts are in `build/large-file-stress-issues-20261004`; the issues contain measurements and reproduction steps without requiring an unpublished report link.

| Priority | Issue | Focus |
| --- | --- | --- |
| P1 | [#91](https://github.com/tuncb/woby/issues/91) | Crash: native OBJ limit/failure paths intermittently exit with 0xC0000374 |
| P1 | [#92](https://github.com/tuncb/woby/issues/92) | Crash investigation: San Miguel surface comparison repeatedly exits with 0xC0000409 |
| P1 | [#93](https://github.com/tuncb/woby/issues/93) | Import: reject known GPU-capacity failures before allocating tens of GiB |
| P2 | [#94](https://github.com/tuncb/woby/issues/94) | Comparison: distance heatmap capacity rejection arrives after 74–162 seconds |
| P1 | [#95](https://github.com/tuncb/woby/issues/95) | UV quality reports ready on BearTrap although the heatmap cannot render |
| P1 | [#96](https://github.com/tuncb/woby/issues/96) | Performance: reduce large-mesh diagnostic memory amplification |
| P2 | [#97](https://github.com/tuncb/woby/issues/97) | Performance: bound UV-overlap workspace on very large meshes |
| P2 | [#98](https://github.com/tuncb/woby/issues/98) | Performance: budget aggregate memory for concurrent analysis preparation |
| P1 | [#99](https://github.com/tuncb/woby/issues/99) | Annotations: projection and edits stall the UI for about 7.5 seconds on Bennu |
| P1 | [#100](https://github.com/tuncb/woby/issues/100) | Performance: analysis Properties rebuild costs about 46 ms every frame on San Miguel |
| P2 | [#101](https://github.com/tuncb/woby/issues/101) | Performance: diagnostic scene/helper submission still costs about 30 ms per frame |
| P2 | [#102](https://github.com/tuncb/woby/issues/102) | Performance: native keycap tessellation takes 132 seconds for a 22.7 MB model |
| P2 | [#103](https://github.com/tuncb/woby/issues/103) | Performance: UV metric and display-setting edits repeatedly rebuild for 15–115 seconds |
| P2 | [#104](https://github.com/tuncb/woby/issues/104) | Performance: BearTrap import finalization blocks a frame for 2.24 seconds |
| P2 | [#105](https://github.com/tuncb/woby/issues/105) | Performance: surface-quality mode changes cause about 3-second view_setup stalls |
| P2 | [#106](https://github.com/tuncb/woby/issues/106) | Performance: dense mesh edge/vertex overlays reduce BearTrap to about 5 FPS |
| P2 | [#107](https://github.com/tuncb/woby/issues/107) | Performance: 100M-point cloud renders at 2.43 FPS and below 1 FPS with large or transparent points |
| P3 | [#108](https://github.com/tuncb/woby/issues/108) | Performance: full diagnostic JSON exports take 94–116 seconds for 1.3–1.5 GB |
| P2 | [#109](https://github.com/tuncb/woby/issues/109) | Benchmark bug: detectors-expanded fails on BusGameMap with invalid vector subscript |
| P1 | [#110](https://github.com/tuncb/woby/issues/110) | Benchmark infrastructure: lock and record drawable size before cross-build FPS comparisons |

Start with the two crash investigations, establish controlled measurements before FPS comparisons, then prioritize annotation/UI stalls and resource limits. The tracker describes dependencies without assuming shared root causes. Closed historical issues were reviewed and relevant new issues reference them; no existing issues were reopened or closed.

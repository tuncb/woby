# Self-intersections

Open `crossing.obj`, add it to either analysis side, and click Run for
Self-intersections. Expected: one pair (triangles 1 and 2), two affected faces,
complete status in automatic or exact-position topology. Show draws both faces
in red; selecting the pair frames and highlights both triangles.

`python tests/ctl_intersections_smoke.py build/vs2026-vcpkg/bin/Debug/woby.exe build/intersection-qa`
checks the real viewer, settings, save/load, both sides, and exports a screenshot.

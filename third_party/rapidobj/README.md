# RapidOBJ

`include/rapidobj/rapidobj.hpp` is vendored from
[guybrush77/rapidobj](https://github.com/guybrush77/rapidobj) at commit
`fe4c779314b0daed19530c8b5aa323c789d08f81`.

The header is distributed under the MIT license in `LICENSE` and retains its
embedded third-party notices.

Two Woby fixes are applied directly to the upstream header:

- Open Windows paths with `CreateFileW` so Unicode filenames work.
- Restore each source polygon's winding after Earcut triangulation, using signed
  areas in the same projection for the polygon and its triangles.

`tests/obj_mesh_tests.cpp` covers both fixes. Preserve them when updating the
header. CMake includes this directory as a system include path and links Threads
for the parser's Linux threading support.

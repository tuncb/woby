# RapidOBJ

`include/rapidobj/rapidobj.hpp` is vendored from
[guybrush77/rapidobj](https://github.com/guybrush77/rapidobj) at commit
`fe4c779314b0daed19530c8b5aa323c789d08f81`.

The header is distributed under the MIT license in `LICENSE` and retains its
embedded third-party notices.

Woby changes are applied directly to the upstream header:

- Open Windows paths with `CreateFileW` so Unicode filenames work.
- Drain outstanding block reads before freeing their destination buffers on all
  parser exits, including errors and exception unwinding. Use the file handle for
  Windows read completion and detect null event-creation failures.
- Check aligned-buffer allocation and propagate parse, merge, and triangulation
  worker exceptions through joined futures. Allocation or thread creation failure
  must not leave detached workers using the caller's buffers.
- Restore each source polygon's winding after Earcut triangulation, using signed
  areas in the same projection for the polygon and its triangles.

- Parse, merge, and triangulate positions as doubles. Normals, UVs, colors, and
  material values remain floats. Woby recenters positions before triangulation
  and creates float vertices only afterward.

`tests/obj_mesh_tests.cpp`, `tests/freeform_tests.cpp`,
`tests/coordinate_origin_tests.cpp`, and the Windows-only
`tests/rapidobj_reader_tests.cpp` cover these changes. The standalone reader tests
delay I/O completion to check buffer lifetime deterministically. Preserve these
changes when updating the header. CMake includes this directory as a system include path and links Threads
for the parser's Linux threading support.

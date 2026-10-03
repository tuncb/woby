#include "trace.h"

namespace mesh_lab {
std::string_view internalObjSource()
{
    return R"obj(# Internal example: two quads folded along a shared edge.
# UV and normal seams split source positions p2 and p3.
o folded_sheet
v 1000000000.000125 -1 0
v 1000000002.000125 -1 0
v 1000000002.000125 1 0
v 1000000000.000125 1 0
v 1000000002.000125 1 2
v 1000000000.000125 1 2
vt 0 0
vt 1 0
vt 1 1
vt 0 1
vt 0.2 0
vt 0.8 0
vt 0.8 1
vt 0.2 1
vn 0 0 1
vn 0 -1 0
f 1/1/1 2/2/1 3/3/1 4/4/1
f 4/5/2 3/6/2 5/7/2 6/8/2
)obj";
}
} // namespace mesh_lab

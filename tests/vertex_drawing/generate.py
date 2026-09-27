"""Extract the actual production upload/draw code; fail on changed anchors."""
from pathlib import Path
import sys

root, output = map(Path, sys.argv[1:])
text = (root / "src/scene_renderer.cpp").read_text()


def section(start, end):
    assert text.count(start) == 1, start
    assert text.count(end) == 1, end
    return text[text.index(start):text.index(end)]


helpers = section("struct PointSpriteVertex {", "std::array<float, 4> scaledRgbColor(")
helpers += section("std::vector<uint32_t> buildLineIndices(", "uint64_t renderState(")
helpers += section("uint64_t renderState(", "void submitColorRange(")
helpers += section("void submitPointSpriteRange(", "void submitHelperBuffer(")
public = section("bgfx::VertexLayout meshVertexLayout()", "bgfx::VertexLayout helperLineVertexLayout()")
public += section("GpuMesh createGpuMesh(", "void destroyModelRuntimes(")
anchor = "            meshLayout);"
assert public.count(anchor) == 1
# Same upload path; shader variant must request a shader-resource view at creation.
public = public.replace(anchor, "            meshLayout, vertex_probe::meshReadFlags);")
output.mkdir(parents=True, exist_ok=True)
(output / "production.inc").write_text("namespace woby {\nnamespace {\n" + helpers
    + "\n}\n" + public + "\n}\n")

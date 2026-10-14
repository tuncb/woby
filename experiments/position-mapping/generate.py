"""Allow position-only triangles through the staged path in comparison builds."""
import argparse
from pathlib import Path


def replace(text, old, new):
    if text.count(old) != 1:
        raise ValueError(f"Position mapping anchor drift: {old!r}")
    return text.replace(old, new, 1)


def write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists() or path.read_text(encoding="utf-8") != text:
        path.write_text(text, encoding="utf-8")


def mapper(text):
    return '#include "control.h"\n' + replace(text,
        "stagedCornerMapping && attributes && indexCount != 0",
        "stagedCornerMapping && (attributes || position_mapping::forcePositionFaces) && indexCount != 0")


def generate(root, output, traced=None):
    write(output / "obj_mesh.cpp", mapper((root / "src/obj_mesh.cpp").read_text(encoding="utf-8")))
    text = '#include "control.h"\n#include "verify.h"\n' + (root / "experiments/rapidobj_prototype.cpp").read_text(encoding="utf-8")
    text = replace(text, 'int wmain(int argc, wchar_t** argv) {',
        'int wmain(int argc, wchar_t** argv) {\n    if (argc == 3 && std::wstring_view(argv[1]) == L"verify") { return position_mapping::verifyFile(std::filesystem::path(argv[2])); }')
    for prefix in ("std::wstring_view", "std::string_view"):
        literal = 'L"' if prefix == "std::wstring_view" else '"'
        old = f'&& {prefix}(argv[1]) != {literal}fused")'
        text = replace(text, old, old[:-1] + f' && {prefix}(argv[1]) != {literal}forced")')
        old = f'    const bool prototype = {prefix}(argv[1]) != {literal}legacy";'
        text = replace(text, old,
            f'    position_mapping::forcePositionFaces = {prefix}(argv[1]) == {literal}forced";\n' + old)
    text = replace(text, '{"mode", prototype ? staged ? "prototype" : "fused" : "legacy"}',
        '{"mode", position_mapping::forcePositionFaces ? "forced" : prototype ? staged ? "prototype" : "fused" : "legacy"}')
    text = text.replace("legacy|prototype|fused absolute.obj", "legacy|prototype|fused|forced absolute.obj")
    write(output / "main.cpp", text)
    if traced:
        write(output / "traced_obj_mesh.cpp", mapper((traced / "obj_mesh.cpp").read_text(encoding="utf-8")))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--traced", type=Path)
    args = parser.parse_args()
    generate(args.root.resolve(strict=True), args.output.resolve(), args.traced)

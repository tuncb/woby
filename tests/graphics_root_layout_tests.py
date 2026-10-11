"""Check the compiled SPIR-V root ABI against the C++ offset contract."""

import struct
import sys
from pathlib import Path


EXPECTED = {
    'modelViewProj': 0, 'model': 64, 'color': 128, 'pointParams': 144,
    'pointOffset': 160, 'pointSourceFirst': 164, 'markerBase': 168,
    'markerQuery': 172, 'markerOptions': 188, 'comparison': 204, 'uvGrid': 220,
    'vertices': 240, 'pointIds': 248, 'stride': 256, 'texture0': 260,
    'texture1': 264, 'image1': 268, 'sampler': 272, 'triangleEdges': 276,
    'triangleOffset': 284, 'transparency': 288, 'analysisFaces': 304,
    'faceKind': 312, 'firstFace': 316,
}
INTEGER_FIELDS = {
    'pointOffset', 'pointSourceFirst', 'markerBase', 'stride', 'texture0',
    'texture1', 'image1', 'sampler', 'triangleOffset', 'faceKind', 'firstFace',
}


def text(words):
    return struct.pack(f'<{len(words)}I', *words).split(b'\0', 1)[0].decode()


def check(path):
    data = path.read_bytes()
    assert len(data) % 4 == 0, path
    words = struct.unpack(f'<{len(data)//4}I', data)
    assert words[0] == 0x07230203, path
    names, members, offsets, types = {}, {}, {}, {}
    cursor = 5
    while cursor < len(words):
        length, opcode = words[cursor] >> 16, words[cursor] & 0xffff
        assert length and cursor + length <= len(words), path
        args = words[cursor+1:cursor+length]
        if opcode == 5:  # OpName
            names[args[0]] = text(args[1:])
        elif opcode == 6:  # OpMemberName
            members[args[:2]] = text(args[2:])
        elif opcode == 72 and args[2] == 35:  # OpMemberDecorate Offset
            offsets[args[:2]] = args[3]
        elif opcode in (21, 30):  # OpTypeInt / OpTypeStruct
            types[args[0]] = (opcode, args[1:])
        cursor += length
    roots = [key for key, name in names.items() if name in ('WobyRoot', 'WobyRoot_c')]
    for root in roots:
        opcode, fields = types[root]
        assert opcode == 30 and len(fields) == len(EXPECTED), path
        actual = {members[root, index]: offsets[root, index] for index in range(len(fields))}
        assert actual == EXPECTED, (path, actual)
        for index, field in enumerate(fields):
            if members[root, index] in INTEGER_FIELDS:
                assert types[field] == (21, (32, 0)), (path, members[root, index])
    return bool(roots)


if __name__ == '__main__':
    directory = Path(sys.argv[1]).resolve()
    checked = {path.stem for path in directory.glob('*.bin') if check(path)}
    required = {'vs_marker_point', 'vs_point_sprite', 'vs_line_sprite', 'vs_mesh_edges',
                'vs_triangle_lines', 'vs_comparison', 'fs_marker_highlight_msaa', 'cs_marker_lookup_msaa'}
    assert required <= checked, f'Missing compiled root contracts: {required - checked}'
    print(f'{len(checked)} compiled roots: all 24 offsets and 11 uint32 fields match the 320-byte C++ ABI')

"""Compile marker/transparency shaders and check Metal multisample reads."""

import re
import subprocess
import sys
import tempfile
from pathlib import Path


def test_marker_shader(compiler, source, ngapi, entry):
    with tempfile.TemporaryDirectory(prefix='woby-marker-shader-') as temporary:
        output = Path(temporary) / f'{entry}.metal'
        subprocess.run([
            str(compiler), str(source), '-target', 'metal', '-DNOGRAPHICSAPI_METAL',
            '-warnings-as-errors', 'all', '-entry', entry,
            '-stage', 'compute' if entry.startswith('cs_') else 'fragment',
            '-fvk-use-c-layout', '-matrix-layout-row-major',
            '-I', str(ngapi / 'include'), '-I', str(ngapi / 'utility/include'),
            '-o', str(output),
        ], check=True, cwd=temporary)
        metal = output.read_text()
        textures = re.findall(r'texture2d_ms<float,\s*access::read>\s+(\w+)\s*=', metal)
        if entry not in ('fs_transparent_mesh', 'fs_transparency_resolve_single'):
            assert textures, f'{entry}: expected multisample texture'
        for texture in textures:
            reads = re.findall(r'\b' + re.escape(texture) + r'\)*\.read\(\s*([^\n;]+)', metal)
            assert reads, f'{entry}: expected multisample texture read'
            assert all(re.match(r'(uint2|vec<uint,\s*2>)\s*\(', read) for read in reads), (
                f'{entry}: Metal multisample coordinates must be unsigned: {reads}')
        if sys.platform == 'darwin':
            subprocess.run(['xcrun', '-sdk', 'macosx', 'metal', '-std=metal4.0',
                            '-Werror', '-c', str(output), '-o', str(output.with_suffix('.air'))],
                           check=True, cwd=temporary)


if __name__ == '__main__':
    compiler, source, ngapi = (Path(value).resolve() for value in sys.argv[1:])
    for entry in ('cs_marker_lookup_single', 'cs_marker_lookup_msaa',
                  'fs_marker_highlight_single', 'fs_marker_highlight_msaa',
                  'fs_transparent_mesh', 'fs_transparency_resolve_single', 'fs_transparency_resolve_msaa'):
        test_marker_shader(compiler, source, ngapi, entry)
        print(f'{entry}: Metal compilation and applicable multisample-read checks passed')

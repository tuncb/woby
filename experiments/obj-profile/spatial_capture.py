"""Add key capture to generated diagnostic sources. Regenerate to remove it.

Usage: spatial_capture.py GENERATED_DIRECTORY EMPTY_CAPTURE_DIRECTORY
Capture changes diagnostic timings. Do not use this executable for timing runs.
The output format is native little-endian uint32 Morton key, uint32 source ID.
Use one load of a single-group model. Calls append keys in original input order.
"""
import json
import sys
from pathlib import Path


def capture(generated, destination):
    if destination.exists():
        raise FileExistsError(destination)
    changes = []
    for name, anchor, label in (
        ("point_cloud", "        radix(keys,cancel,helpers);", "points"),
        ("surface_annotation", "        stableSpatialRadixSort(keys,", "annotations"),
    ):
        path = generated / (name + ".cpp")
        text = path.read_text(encoding="utf-8")
        if "WOBY_PROFILE_SPATIAL_CAPTURE" in text:
            raise ValueError("Regenerate sources before another capture: " + name)
        if text.count(anchor) != 1:
            raise ValueError("Capture anchor is missing or duplicated: " + name)
        filename = json.dumps(str(destination / (label + ".keys")))
        code = f'''        {{
            static_assert(sizeof(keys[0]) == 8);
            std::ofstream capture({filename}, std::ios::binary | std::ios::app);
            capture.exceptions(std::ios::badbit | std::ios::failbit);
            capture.write(reinterpret_cast<const char*>(keys.data()), static_cast<std::streamsize>(keys.size() * 8));
            capture.close();
        }}
'''
        changes.append((path, '// WOBY_PROFILE_SPATIAL_CAPTURE\n#include <fstream>\n' + text.replace(anchor, code + anchor)))
    destination.mkdir(parents=True, exist_ok=False)
    for path, text in changes:
        path.write_text(text, encoding="utf-8")


if __name__ == "__main__":
    capture(*(Path(arg).resolve() for arg in sys.argv[1:]))

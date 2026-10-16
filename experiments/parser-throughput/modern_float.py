"""Replace the embedded number decoder in an isolated generated parser header."""
from pathlib import Path
import shutil
import sys


def substitute(text, header):
    first = "// Code below is from: https://github.com/fastfloat/fast_float/releases/download/v3.5.1/fast_float.h"
    last = "// clang-format on\n\n//\n// Third-party includes End"
    if text.count(first) != 1 or text.count(last) != 1:
        raise ValueError("Embedded fast_float boundary drift")
    if "namespace fast_float" not in header:
        raise ValueError("Replacement is not a fast_float header")
    start, end = text.index(first), text.index(last)
    if end <= start:
        raise ValueError("Embedded fast_float boundaries are reversed")
    return text[:start] + "// Research decoder replacement; production source is unchanged.\n" + header + "\n\n" + text[end:]


def generate(source, output, header):
    output.mkdir(parents=True, exist_ok=True)
    text = (source / "rapidobj.hpp").read_text(encoding="utf-8")
    replacement = header.read_text(encoding="utf-8")
    (output / "rapidobj.hpp").write_text(substitute(text, replacement), encoding="utf-8")
    prototype = (source / "prototype.hpp").read_text(encoding="utf-8")
    if "read_u64(" not in replacement and "read8_to_u64(" in replacement:
        # Newer fast_float versions renamed the eight-byte reader. Adapt only
        # the generated research copy used by the common OBJ numeric paths.
        prototype = prototype.replace("fast_float::read_u64(", "fast_float::read8_to_u64(")
    (output / "prototype.hpp").write_text(prototype, encoding="utf-8")


if __name__ == "__main__":
    generate(*(Path(value).resolve() for value in sys.argv[1:]))

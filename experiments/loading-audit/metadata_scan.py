"""Inspect rare OBJ metadata keywords without a Python loop over every face."""
import json
from pathlib import Path
import sys
import time


def scan(source, block_bytes=32 * 1024**2):
    counts = {"s": 0, "usemtl": 0}
    first = {}; smoothing_values = set(); carry = b""; offset = 0; backslash = False
    for block in iter(lambda: source.read(block_bytes), b""):
        data = carry + block
        end = data.rfind(b"\n") + 1
        if end == 0:
            carry = data
            continue
        complete, carry = data[:end], data[end:]
        backslash |= b"\\" in complete
        for keyword in (b"s", b"usemtl", b"f"):
            if keyword == b"f" and "f" in first:
                continue
            position = complete.find(keyword)
            while position >= 0:
                after = position + len(keyword)
                line_start = complete.rfind(b"\n", 0, position) + 1
                if after < len(complete) and complete[after] in (9, 32) and not complete[line_start:position].strip(b" \t"):
                    key = keyword.decode("ascii")
                    first.setdefault(key, offset + position)
                    if key == "f":
                        break
                    counts[key] += 1
                    if key == "s":
                        line_end = complete.find(b"\n", after)
                        argument = complete[after:line_end].split(b"#", 1)[0].strip()
                        smoothing_values.add(argument.decode("ascii", errors="replace"))
                position = complete.find(keyword, position + len(keyword))
        offset += end
    if carry:
        # Reuse the same scanner for the final non-newline-terminated line.
        import io
        tail = scan(io.BytesIO(carry + b"\n"), len(carry) + 1)
        backslash |= tail["contains_backslash"]
        for key, value in tail["counts"].items(): counts[key] += value
        for key, value in tail["first_keyword_byte"].items(): first.setdefault(key, offset + value)
        smoothing_values.update(tail["smoothing_arguments"])
    safe = not backslash
    return {"counts": counts, "first_keyword_byte": first, "smoothing_arguments": sorted(smoothing_values),
        "contains_backslash": backslash,
        "uniform_zero_smoothing_proven": safe and smoothing_values.issubset({"off", "0"}),
        "uniform_material_id_proven": safe and counts["usemtl"] == 1 and "f" in first and first["usemtl"] < first["f"],
        "method": "Whole-file rare-keyword scan with whitespace-aware physical line starts; any backslash disables uniformity inference to avoid continuation ambiguity"}


if __name__ == "__main__":
    start = time.perf_counter()
    with Path(sys.argv[1]).open("rb") as source:
        result = scan(source)
    result["wall_seconds"] = time.perf_counter() - start
    Path(sys.argv[2]).write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps(result))

"""Count physical OBJ record prefixes and sample numeric formats in bounded memory."""
from collections import Counter
import json
from pathlib import Path
import sys
import time

KEYWORDS = (b"v", b"vt", b"vn", b"f", b"g", b"o", b"l", b"p", b"usemtl", b"mtllib", b"surf", b"curv")


def counts(stream, block_bytes=32 * 1024**2):
    patterns = [b"\n" + key + whitespace for key in KEYWORDS for whitespace in (b" ", b"\t")]
    totals = Counter()
    tail = b"\n"
    longest = max(map(len, patterns))
    formats = {}
    blocks = 0
    for block in iter(lambda: stream.read(block_bytes), b""):
        blocks += 1
        prefix = block[:longest - 1]
        boundary = tail + prefix
        for pattern in patterns:
            totals[pattern] += block.count(pattern) + boundary.count(pattern) - tail.count(pattern) - prefix.count(pattern)
        if blocks % 8 == 1:
            for line in block[:256 * 1024].splitlines()[1:-1]:
                fields = line.split()
                if not fields or fields[0] not in (b"v", b"vt", b"vn", b"f"):
                    continue
                kind = fields[0].decode()
                samples = formats.setdefault(kind, [])
                if len(samples) < 4096:
                    samples.append(fields[1:])
        tail = (tail + block)[-(longest - 1):]
    result = {key.decode(): sum(totals[b"\n" + key + whitespace] for whitespace in (b" ", b"\t")) for key in KEYWORDS}
    return result, {kind: {"sample_rows": len(rows), "mean_fields": sum(map(len, rows)) / len(rows),
        "mean_row_payload_bytes": sum(sum(map(len, row)) + max(0, len(row) - 1) for row in rows) / len(rows),
        "scientific_tokens": sum(b"e" in value.lower() for row in rows for value in row),
        "negative_tokens": sum(value.startswith(b"-") for row in rows for value in row),
        "slash_tokens": sum(b"/" in value for row in rows for value in row)} for kind, rows in formats.items()}


if __name__ == "__main__":
    start = time.perf_counter()
    path = Path(sys.argv[1]).resolve(strict=True)
    with path.open("rb") as stream:
        records, formats = counts(stream)
    result = {"method": "Exact physical record prefixes at column zero, space or tab delimiter; numeric samples are not a full grammar validation",
              "records": records, "formats": formats, "wall_seconds": time.perf_counter() - start}
    Path(sys.argv[2]).write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps(result))

"""Make an explicitly labeled finite-only derivative of a vertex-only OBJ cloud.

Never alters the source, overwrites a destination, or silently changes indexed
geometry. A separate manifest records discarded rows and both file hashes.
"""

import argparse
import hashlib
import json
import math
from pathlib import Path


def finite_vertices(source, destination):
    source, destination = source.resolve(), destination.resolve()
    if source == destination:
        raise ValueError("Source and output must differ")
    source_hash, target_hash = hashlib.sha256(), hashlib.sha256()
    counts = dict(source_vertices=0, kept_vertices=0, removed_nonfinite_vertices=0)
    destination.parent.mkdir(parents=True, exist_ok=True)
    with destination.open("xb") as out:
        try:
            with source.open("rb") as stream:
                for line in stream:
                    source_hash.update(line)
                    words = line.split()
                    if not words or words[0].startswith(b"#"):
                        pass
                    elif words[0] == b"v" and len(words) >= 4:
                        counts["source_vertices"] += 1
                        if not all(math.isfinite(float(value)) for value in words[1:4]):
                            counts["removed_nonfinite_vertices"] += 1
                            continue
                        counts["kept_vertices"] += 1
                    else:
                        raise ValueError("Finite filtering requires a vertex-only OBJ; references and other statements are not remapped")
                    out.write(line)
                    target_hash.update(line)
        except BaseException:
            out.close()
            destination.unlink()
            raise
    return dict(source=str(source), output=str(destination), **counts,
                source_sha256=source_hash.hexdigest(), output_sha256=target_hash.hexdigest(),
                transformation="Remove v records with non-finite XYZ; preserve all other bytes; no coordinate transform")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    result = finite_vertices(args.source, args.output)
    args.output.with_suffix(".provenance.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps(result))


if __name__ == "__main__":
    main()

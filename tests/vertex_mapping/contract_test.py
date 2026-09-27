"""Check experimental maps against the current loader using isolated fixtures."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def run(binary, mode, path, cwd):
    suffix = ".exe" if os.name == "nt" else ""
    process = subprocess.run([binary / f"woby_mapping_{mode}{suffix}", path], cwd=cwd,
                             text=True, capture_output=True)
    data = json.loads(process.stdout)
    assert process.returncode == 0 and data["ok"], data
    return data


def main():
    binary = Path(sys.argv[1]).resolve()
    base = "v 0 0 0\nv 2 0 0\nv 2 2 0\nv 1 1 0\nv 0 2 0\nv 9 9 9\n"
    fixtures = [
        base + "g concave\nf 1 2 3 4 5\ng reversed\nf -2 -3 -4\n",
        base + "vt 0 0\nvt 1 1\nvt 0 0\nvn 0 0 1\nvn 0 0 -1\nvn 0 0 1\n"
        "g one\nf 1/1/1 2/1/1 3/1/1\ng seam\nf 1/2/2 3/2/2 2/2/2\n"
        "g duplicate_values\nf 1/3/3 2/3/3 3/3/3\ng missing\nf 1 3 5\n",
        # Source position count exceeds corner count: direct table needs all positions.
        base + "v 3 3 3\nv 4 4 4\nf 6 7 8\n",
        # Exercise secondary rehash and repeated seam hits.
        base + "".join(f"vt {i/100} 0\n" for i in range(100))
        + "".join(f"f 1/{i} 2/{i} 3/{i}\nf 1/{i} 3/{i} 5/{i}\n" for i in range(1, 101)),
    ]
    with tempfile.TemporaryDirectory(prefix="woby-mapping-") as temporary:
        root = Path(temporary)
        for i, content in enumerate(fixtures):
            path = root / f"fixture-{i}-ü.obj"
            path.write_text(content, encoding="utf-8")
            reference = run(binary, "baseline", path, root)
            assert reference["layout"] == [32, 0, 12, 24]
            for mode in ("hybrid", "adaptive"):
                result = run(binary, mode, path, root)
                for key in ("hashes", "bounds", "radius", "vertices", "indices", "nodes", "point_entries"):
                    assert reference[key] == result[key], (i, mode, key)
                assert result["map"]["direct"] == (mode == "adaptive" and i in (0, 2))
                assert result["map"]["primary_bytes"] == result["map"]["position_count"] * 4
                if result["map"]["direct"]:
                    assert result["map"]["key_capacity_bytes"] == result["map"]["bucket_bytes"] == 0
                if i == 3:
                    assert result["map"]["secondary_entries"] > 16
                assert all(value >= 0 for value in result["stages"].values())
        suffix = ".exe" if os.name == "nt" else ""
        failed = subprocess.run([binary / f"woby_mapping_baseline{suffix}", "relative.obj"],
                                cwd=root, capture_output=True, text=True)
        assert failed.returncode != 0 and not json.loads(failed.stdout)["ok"]
        output = root / "sweep.jsonl"
        subprocess.run([sys.executable, Path(__file__).resolve().with_name("run.py"),
                        "--bin", binary, "--models", root, "--output", output,
                        "--rounds", "1", "--variants", "adaptive"],
                       cwd=root, check=True, capture_output=True, text=True)
        rows = [json.loads(line) for line in output.read_text().splitlines()]
        assert len(rows) == len(fixtures) and all(row["variant"] == "adaptive" for row in rows)
    print("Mapping contract passed (all variants, four fixtures).")


if __name__ == "__main__":
    main()

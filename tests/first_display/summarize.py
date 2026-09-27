"""Package raw records and medians without confusing nested stage totals."""
import json
from pathlib import Path
import statistics
import subprocess
import sys

source, destination = map(Path, sys.argv[1:])
rows = [json.loads(line) for line in source.read_text().splitlines()]
assert len({(row["width"], row["height"]) for row in rows}) == 1, "Mixed rendering resolutions"
models = sorted({row["file"] for row in rows})
summary = {}
for model in models:
    runs = [row for row in rows if row["file"] == model]
    assert len(runs) >= 2
    for field in ("vertices", "triangles", "groups", "vertex_bytes", "index_bytes", "point_entries", "map"):
        assert all(row[field] == runs[0][field] for row in runs), (model, field)
    metrics = {}
    for row in runs:
        stages = row["stages"]
        derived = {
            "mapping_ms": stages["map_allocate_ms"] + stages["map_loop_ms"],
            "normals_ms": stages["normal_check_ms"] + stages["normal_generate_ms"],
            "gpu_point_preparation_ms": sum(stages[k] for k in ("gpu_index_validate_ms", "point_allocate_ms", "point_ranges_ms")),
            "gpu_staging_ms": stages["vertex_staging_ms"] + stages["index_staging_ms"],
            "first_execute_and_wait_ms": row["first_frame_execute_ms"] + row["first_gpu_wait_ms"],
            "post_model_to_first_complete_ms": row["load_to_first_complete_ms"] - row["model_file_ms"],
            "post_model_to_loop_ms": row["load_to_loop_ms"] - row["model_file_ms"],
        }
        for key, value in {**stages, **{k: v for k, v in row.items() if k.endswith("_ms")}, **derived}.items():
            metrics.setdefault(key, []).append(value)
    summary[Path(model).name] = {
        "count": len(runs),
        "metrics": {key: {"median": statistics.median(values), "min": min(values), "max": max(values)}
                    for key, values in metrics.items()},
        "geometry": {field: runs[0][field] for field in ("vertices", "triangles", "groups", "vertex_bytes", "index_bytes", "point_entries", "map")},
    }
output = {
    "revision": subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip(),
    "method": "MSVC Release; full windowed viewer; D3D11 single render thread; GPU event completion; screenshot on following frame; serial fresh processes per model (count in summary); identical resolution required; no cache eviction",
    "runs": rows, "summary": summary,
}
destination.write_text(json.dumps(output, indent=2) + "\n")
for key in ("parse_ms", "triangulate_ms", "source_copy_ms", "map_allocate_ms", "map_loop_ms",
            "normal_check_ms", "normal_generate_ms", "bounds_ms", "temporary_release_ms", "cpu_load_ms",
            "annotation_cache_ms", "group_state_ms", "gpu_index_validate_ms", "point_allocate_ms", "point_ranges_ms",
            "vertex_staging_ms", "index_staging_ms", "gpu_prepare_ms", "post_model_to_loop_ms", "first_frame_cpu_ms",
            "first_execute_and_wait_ms", "load_to_first_complete_ms", "startup_to_first_complete_ms"):
    print("| " + key + " | " + " | ".join(f"{item['metrics'][key]['median']/1000:.3f}" for item in summary.values()) + " |")
print(json.dumps({name: item["geometry"] for name, item in summary.items()}, indent=2))

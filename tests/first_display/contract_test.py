"""Probe contract: all fixture files share a unique temporary root."""
from pathlib import Path
import sys
import tempfile
from run import run

binary = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix="woby-first-display-") as temporary:
    root = Path(temporary)
    model = root / "seams.obj"
    model.write_text("v 0 0 0\nv 2 0 0\nv 0 2 0\n"
                     "vt 0 0\nvt 1 1\nvn 0 0 1\n"
                     "g a\nf 1/1/1 2/1/1 3/1/1\n"
                     "g b\nf 1/2/1 2/2/1 3/2/1\n")
    result = run(binary, model, root / "result.json")
    assert result["vertices"] == 6 and result["triangles"] == 2 and result["groups"] == 2
    assert result["map"]["secondary_entries"] == 3 and not result["map"]["direct"]
    assert result["vertex_bytes"] == 6 * 32 and result["index_bytes"] == 6 * 4
    assert result["point_entries"] == 6
    required = ("parse_ms", "triangulate_ms", "source_copy_ms", "map_allocate_ms", "map_loop_ms",
                "normal_check_ms", "normal_generate_ms", "bounds_ms", "temporary_release_ms",
                "annotation_cache_ms", "group_state_ms", "gpu_index_validate_ms", "point_allocate_ms",
                "point_ranges_ms", "vertex_staging_ms", "index_staging_ms")
    assert all(stage in result["stages"] for stage in required)
    assert list(root.glob("*.tga")), "Missing rendered screenshot"
    assert result["load_to_first_complete_ms"] >= result["load_to_loop_ms"]
    assert result["startup_to_first_complete_ms"] >= result["load_to_first_complete_ms"]
print("First-display probe contract passed.")

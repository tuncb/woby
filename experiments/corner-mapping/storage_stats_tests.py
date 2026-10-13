"""Tests for buffer arithmetic, OBJ topology auditing, and probe validation."""
import copy
import hashlib
import io
import json
from pathlib import Path
import tempfile
import unittest
import sys
from storage_stats import audit_obj, measure, probe_counts, storage


def batch():
    return {"status": "complete", "records": [{"probe": {
        "geometry": {"triangles": 2, "vertices": 4, "source_positions": 5,
                     "line_segments": 0, "point_indices": 0,
                     "geometry_fingerprint": 1, "normal_fingerprint": 2, "source_fingerprint": 3},
        "memory": {"vertex_size_bytes": 128},
        "events": [{"name": "corner_mapping", "data": {"secondary_vertices": 1}}],
    }}]}


CHECK = unittest.TestCase()
def test_shared_square_counts_indices_in_net_saving():
    result = storage(2, 4)
    CHECK.assertEqual(result["expanded_vertex_bytes"], 192)
    CHECK.assertEqual(result["shared_vertex_bytes"], 128)
    CHECK.assertEqual(result["triangle_index_bytes"], 24)
    CHECK.assertEqual(result["saved_bytes"], 40)

def test_unshared_vertices_have_index_overhead():
    result = storage(1, 3)
    CHECK.assertEqual(result["saved_bytes"], -12)
    CHECK.assertEqual(result["net_saving_percent"], -12.5)

def test_six_references_give_surface_saving():
    result = storage(200, 100)
    CHECK.assertEqual(result["indexed_bytes_per_triangle"], 28)
    CHECK.assertAlmostEqual(result["net_saving_percent"], 70.833333333)

def test_invalid_counts_and_sizes_are_rejected():
    for args in ((0, 1), (1, 4), (True, 1), (1, 1, 0), (1, 1, 32, -1), (1, 1.5)):
        with CHECK.subTest(args=args), CHECK.assertRaises(ValueError):
            storage(*args)

def test_source_audit_handles_comments_crlf_and_final_line():
    source = b"# f 9 9 9\r\nv 0 0 0\r\n f 1/1 2/2 3/3 # comment\r\nf 1 2 3 4\np 1\nl 1 2\nsurf 0 1 0 1 1 2\ncurv 0 1 1 2\nf 1 2 3"
    result = audit_obj(io.BytesIO(source), 7)
    CHECK.assertEqual(result["source_faces_by_corner_count"], {3: 2, 4: 1})
    CHECK.assertEqual(result["triangles_from_faces"], 4)
    CHECK.assertEqual(result["bytes"], len(source))
    CHECK.assertEqual(result["sha256"], hashlib.sha256(source).hexdigest())
    CHECK.assertEqual([result[k] for k in ("line_records", "point_records", "surface_records", "curve_records")], [1, 1, 1, 1])

def test_continuations_across_every_chunk_boundary():
    source = b"\tf 1 2 \\\r\n 3 \\\n4\nf 1\t2\t3"
    for size in range(1, len(source) + 1):
        with CHECK.subTest(size=size):
            result = audit_obj(io.BytesIO(source), size)
            CHECK.assertEqual(result["source_faces_by_corner_count"], {3: 1, 4: 1})
            CHECK.assertEqual(result["sha256"], hashlib.sha256(source).hexdigest())

def test_malformed_faces_and_chunk_sizes_are_rejected():
    for source, size in ((b"f 1 2\n", 8), (b"f 1 2 # 3\n", 8), (b"", 0)):
        with CHECK.assertRaises(ValueError):
            audit_obj(io.BytesIO(source), size)

def test_probe_counts_keep_seams_and_unused_positions():
    result = probe_counts(batch())
    CHECK.assertEqual(result["used_source_positions"], 3)
    CHECK.assertEqual(result["unused_source_positions"], 2)
    CHECK.assertEqual(result["extra_vertices_for_normal_or_uv_seams"], 1)

def test_disagreeing_geometry_and_seams_are_rejected():
    for kind in ("geometry", "seams"):
        data = batch()
        data["records"].append(copy.deepcopy(data["records"][0]))
        probe = data["records"][1]["probe"]
        if kind == "geometry":
            probe["geometry"]["geometry_fingerprint"] += 1
        else:
            probe["events"][0]["data"]["secondary_vertices"] += 1
        with CHECK.assertRaises(ValueError):
            probe_counts(data)

def test_wrong_layout_and_mixed_primitives_are_rejected():
    for kind in ("layout", "lines", "status"):
        data = batch()
        probe = data["records"][0]["probe"]
        if kind == "layout":
            probe["memory"]["vertex_size_bytes"] = 64
        elif kind == "lines":
            probe["geometry"]["line_segments"] = 1
        else:
            data["status"] = "partial"
        with CHECK.assertRaises(ValueError):
            probe_counts(data)


def write_fixture(root, source):
    model = root / "model.obj"
    model.write_bytes(source)
    data = batch()
    data["model"] = {"path": str(model), "bytes": len(source), "sha256": hashlib.sha256(source).hexdigest()}
    measured = root / "batch.json"
    measured.write_text(json.dumps(data), encoding="utf-8")
    return measured, model


def test_full_file_audit_rejects_changed_model():
    with tempfile.TemporaryDirectory(prefix="woby-storage-stats-") as directory:
        root = Path(directory).resolve()
        measured, model = write_fixture(root, b"f 1 2 3\nf 1 3 4\n")
        CHECK.assertEqual(measure(measured)["saved_bytes"], 40)
        model.write_bytes(b"f 1 2 4\nf 1 3 4\n")
        with CHECK.assertRaisesRegex(ValueError, "differs"):
            measure(measured)


def test_source_triangle_count_must_match_import_count():
    with tempfile.TemporaryDirectory(prefix="woby-storage-stats-") as directory:
        root = Path(directory).resolve()
        measured, _ = write_fixture(root, b"f 1 2 3\n")
        with CHECK.assertRaisesRegex(ValueError, "triangle count"):
            measure(measured)


if __name__ == "__main__":
    suite = unittest.TestSuite(unittest.FunctionTestCase(value) for name, value in sorted(globals().items())
                               if name.startswith("test_") and callable(value))
    result = unittest.TextTestRunner().run(suite)
    sys.exit(0 if result.wasSuccessful() else 1)

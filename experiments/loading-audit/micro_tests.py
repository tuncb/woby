# /// script
# requires-python = ">=3.11"
# dependencies = ["duckdb>=1.3", "pefile>=2024", "capstone>=5", "psutil>=6"]
# ///
"""Check weighted events, scoped SQL, cache populations, and topology."""
import struct
import io
import unittest
import duckdb
from micro_analyze import metrics, weighted_counts, counter_query
from micro_run import decode_topology, affinity_sets, SETS
from metadata_scan import scan

check = unittest.TestCase()


def test_weighted_events_and_distinct_cache_populations():
    definitions = [{"hwId": event, "mask": mask, "interval": interval}
                   for event, mask, interval in [(0x76, 0, 2), (0xC0, 0, 10), (0xC2, 0, 5), (0xC3, 0, 1)]]
    counts = weighted_counts(definitions, [10, 10, 4, 2])
    result = metrics(counts)
    check.assertEqual(result["ipc"], 5)
    check.assertEqual(result["branch_miss_percent"], 10)
    check.assertEqual(result["branch_misses_per_kinst"], 20)
    for key, value in [("064:08", 20), ("064:f0", 80), ("045:0f", 90), ("045:f0", 10)]:
        counts[key] = {"estimated_events": value}
    result = metrics(counts)
    check.assertEqual(result["selected_l2_data_miss_percent"], 20)
    check.assertEqual(result["dtlb_refill_l2_miss_percent"], 10)
    check.assertIsNone(metrics({})["ipc"])


def test_unresolved_functions_and_process_thread_window_scope():
    with duckdb.connect(":memory:") as database:
        database.execute("CREATE TABLE Module(id INTEGER,path VARCHAR)")
        database.execute("CREATE TABLE Function(id INTEGER,name VARCHAR)")
        database.execute("CREATE TABLE UnifiedSampleSeries(processId INTEGER,threadId INTEGER,windowId INTEGER,moduleId INTEGER,functionId INTEGER,n INTEGER)")
        database.execute("INSERT INTO Module VALUES (1,'runtime.dll')")
        database.execute("INSERT INTO UnifiedSampleSeries VALUES (1,2,10,1,999,3),(1,3,10,1,999,4),(9,2,10,1,999,5),(1,2,11,1,999,6)")
        check.assertEqual(counter_query(database, "sum(s.n)", 1, 10, 11, [2]), (3,))
        check.assertEqual(counter_query(database, "sum(s.n)", 1, 10, 11, [2], "function"), [("runtime.dll", "[unresolved function]", 3)])
        with check.assertRaisesRegex(ValueError, "Empty thread"):
            counter_query(database, "sum(s.n)", 1, 10, 11, [])


def test_windows_topology_and_affinity_domains():
    def core(mask):
        body = bytearray(40); body[0] = 1
        struct.pack_into("<H", body, 22, 1); struct.pack_into("<QH", body, 24, mask, 0)
        return struct.pack("<II", 0, 48) + body
    decoded = decode_topology(core(3) + core(12))
    check.assertEqual(affinity_sets(decoded)["physical"], [0, 2])
    check.assertEqual(affinity_sets(decoded)["all"], [0, 1, 2, 3])
    with check.assertRaisesRegex(ValueError, "Invalid Windows"):
        decode_topology(struct.pack("<II", 0, 0))
    for events in SETS.values():
        check.assertEqual(len(events), 6)
        check.assertTrue(all(interval >= 25000 for _, _, interval in events))


def test_metadata_boundaries_and_conservative_uniformity():
    text = b"# s 5\n  usemtl ground\r\n\ts off\nv 1 2 3\nf 1 1 1"
    for block in (1, 3, 8, 64):
        result = scan(io.BytesIO(text), block)
        check.assertEqual(result["counts"], {"s": 1, "usemtl": 1})
        check.assertTrue(result["uniform_zero_smoothing_proven"])
        check.assertTrue(result["uniform_material_id_proven"])
    result = scan(io.BytesIO(b"s\\\n 1\nf 1 1 1\n"), 3)
    check.assertFalse(result["uniform_zero_smoothing_proven"])


if __name__ == "__main__":
    test_weighted_events_and_distinct_cache_populations()
    test_unresolved_functions_and_process_thread_window_scope()
    test_windows_topology_and_affinity_domains()
    test_metadata_boundaries_and_conservative_uniformity()
    print("Parser microarchitecture accounting checks passed")

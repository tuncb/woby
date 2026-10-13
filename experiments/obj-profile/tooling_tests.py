# /// script
# requires-python = ">=3.11"
# dependencies = ["duckdb>=1.3", "pefile>=2024", "capstone>=5"]
# ///
"""Test source generation, PMU ranges, sample scaling, and instruction decoding."""
import json
import ctypes
import mmap
import os
from pathlib import Path
import tempfile
import unittest

import capstone
import duckdb
from deep_tools import find_counted_loop, instruction_category, preserve_capture_symbols, record_from_log, summarize
from production_tools import map_functions, nearest_function
from memory_map import MEM_COMMIT, MEM_FREE, MEM_IMAGE, MEM_MAPPED, MEM_PRIVATE, MEM_RESERVE, MBI64, read_process_map, summarize_regions
from generate import generate as generate_profile, replace
from gpu_viewer_generate import generate as generate_upload_viewer


def test_profile_generation_current_sources():
    check = unittest.TestCase()
    source_root = Path(__file__).resolve().parents[2]
    sources = {name: (source_root / "src" / name).read_bytes()
               for name in ("model_mesh.cpp", "ui_state.cpp", "main.cpp", "frame_pacing.cpp")}
    with tempfile.TemporaryDirectory(prefix="woby_profile_generation_") as directory:
        fixture_root = Path(directory).resolve()
        profile_output = fixture_root / "profile"
        generate_profile(source_root, profile_output)
        for name in ("obj_mesh.cpp", "model_mesh.cpp", "scene_mesh_preparation.cpp", "point_cloud.cpp",
                     "surface_annotation.cpp", "ui_state.cpp", "rapidobj/rapidobj.hpp", "rapidobj/prototype.hpp"):
            check.assertTrue((profile_output / name).is_file(), name)
        model = (profile_output / "model_mesh.cpp").read_text(encoding="utf-8")
        check.assertIn('    if (!mesh.preparationDeferred) { mesh.originalBounds = originalMeshBounds(mesh); }\n'
                       '    obj_profile::end(measuredFinalize);', model)
        groups = (profile_output / "ui_state.cpp").read_text(encoding="utf-8")
        check.assertIn('obj_profile::begin("group_render_bounds");\n'
                       '        group.localBounds = mesh.preparationDeferred ? mesh.bounds', groups)
        check.assertIn('obj_profile::next(measuredGroup, "group_original_bounds");\n'
                       '        if (!mesh.preparationDeferred) { group.originalBounds = originalMeshBounds(', groups)
        upload_output = fixture_root / "upload"
        generate_upload_viewer(source_root, upload_output)
        pacing = (upload_output / "frame_pacing.cpp").read_text(encoding="utf-8")
        check.assertIn("uint64_t{1'000'000}", pacing)
        check.assertNotIn("uint64_t{50'000'000}", pacing)
        viewer = (upload_output / "main.cpp").read_text(encoding="utf-8")
        check.assertIn("finalize.researchUploadedBytes.resize(finalize.files.size());", viewer)
        check.assertIn("researchStats.uploadedBytes - researchPreviousBytes", viewer)
        check.assertLess(viewer.index("researchPreviousBytes = researchStats.uploadedBytes;"),
                         viewer.index('spdlog::info("research upload_frame bytes={} wall_ms={}"'))
        check.assertLess(viewer.index('spdlog::info("research upload_frame bytes={} wall_ms={}"'),
                         viewer.index("if (!researchComplete) { return {}; }"))
    for name, original in sources.items():
        check.assertEqual((source_root / "src" / name).read_bytes(), original)


def test_generation_rejects_anchor_drift():
    check = unittest.TestCase()
    for text in ("missing marker", "anchor anchor"):
        with check.assertRaisesRegex(ValueError, "Profile anchor drift"):
            replace(text, "anchor", "replacement")
    check.assertEqual(replace("one anchor only", "anchor", "replacement"), "one replacement only")


def test_memory_map_grouping():
    check = unittest.TestCase()
    regions = [
        {"base": 0x10000, "allocation_base": 0x10000, "size": 4096, "state": MEM_COMMIT, "type": MEM_PRIVATE, "protect": 4},
        {"base": 0x11000, "allocation_base": 0x10000, "size": 8192, "state": MEM_RESERVE, "type": MEM_PRIVATE, "protect": 0},
        {"base": 0x13000, "allocation_base": 0x10000, "size": 4096, "state": MEM_COMMIT, "type": MEM_PRIVATE, "protect": 2},
        {"base": 0x20000, "allocation_base": 0x20000, "size": 16384, "state": MEM_COMMIT, "type": MEM_IMAGE, "protect": 32, "mapped_file": "sample.dll"},
        {"base": 0x30000, "allocation_base": 0, "size": 32768, "state": MEM_FREE, "type": 0, "protect": 0},
    ]
    result = summarize_regions(regions)
    check.assertEqual(MBI64.size, 48)
    check.assertEqual(result["totals"], {"private_committed_bytes": 8192, "private_reserved_bytes": 8192, "image_committed_bytes": 16384})
    check.assertEqual(len(result["allocations"]), 2)
    check.assertEqual(result["allocations"][0]["mapped_file"], "sample.dll")
    check.assertEqual(result["allocations"][1]["region_count"], 3)
    check.assertEqual(result["allocations"][1]["protections"], [2, 4])


def test_native_memory_map():
    check = unittest.TestCase()
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.VirtualAlloc.argtypes = (ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32, ctypes.c_uint32)
    kernel.VirtualAlloc.restype = ctypes.c_void_p
    kernel.VirtualFree.argtypes = (ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32)
    kernel.VirtualFree.restype = ctypes.c_int
    reserved = kernel.VirtualAlloc(None, 32*1024*1024, MEM_RESERVE, 4)
    if not reserved:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        if not kernel.VirtualAlloc(reserved, 2*1024*1024, MEM_COMMIT, 4):
            raise ctypes.WinError(ctypes.get_last_error())
        ctypes.memset(reserved, 0x11, 4096)
        snapshot = read_process_map(os.getpid(), resident=True)
        row = next(item for item in snapshot["allocations"] if item["base"] == reserved)
        check.assertEqual(row["committed_bytes"], 2*1024*1024)
        check.assertEqual(row["reserved_bytes"], 30*1024*1024)
        check.assertEqual(row["types"], ["private"])
        check.assertEqual(row['queried_pages'], 512)
        check.assertGreaterEqual(row['valid_pages'], 1)
        check.assertLessEqual(row['valid_pages'], 512)
        check.assertEqual(row['shared_valid_pages'], 0)
        with tempfile.TemporaryDirectory(prefix="woby_memory_map_") as directory:
            fixture = Path(directory).resolve() / "mapped_fixture.bin"
            fixture.write_bytes(bytes(65536))
            with fixture.open("r+b") as stream, mmap.mmap(stream.fileno(), 0) as mapped:
                mapped[0] = 1
                snapshot = read_process_map(os.getpid())
                row = next(item for item in snapshot["allocations"] if item["mapped_file"].endswith("mapped_fixture.bin"))
                check.assertEqual(row["types"], ["mapped"])
                check.assertEqual(row["committed_bytes"], 65536)
                check.assertTrue(any(region["type"] == MEM_MAPPED for region in snapshot["regions"]))
    finally:
        if not kernel.VirtualFree(reserved, 0, 0x8000):
            raise ctypes.WinError(ctypes.get_last_error())
    check.assertFalse(any(row["base"] == reserved for row in read_process_map(os.getpid())["allocations"]))


def test_log_and_instruction_decoding():
    check = unittest.TestCase()
    with tempfile.TemporaryDirectory(prefix="woby_pmu_log_") as directory:
        log = Path(directory).resolve() / "log.txt"
        log.write_text('Profiling started\n{"clock_origin":{},"compiler_isa":"SSE2","events":[]}\n', encoding="utf-8")
        check.assertEqual(record_from_log(log)["compiler_isa"], "SSE2")
    decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    for encoded, expected in [("f20f58c1", "SSE scalar"), ("660f58c1", "SSE128 packed/move"),
                              ("c5f558c2", "AVX256"), ("c5f358c2", "AVX scalar"), ("7500", "control flow")]:
        check.assertEqual(instruction_category(next(decoder.disasm(bytes.fromhex(encoded), 0))), expected)
    body = bytes.fromhex("660f59ca660f58cb660f59ca660f58cb4883e80175ea")
    loop = find_counted_loop(body, 4)
    check.assertEqual((loop["start_rva"], loop["end_rva"]), (0, len(body)))
    check.assertEqual([row["mnemonic"] for row in loop["instructions"]], ['mulpd', 'addpd', 'mulpd', 'addpd', 'sub', 'jne'])
    check.assertIsNone(find_counted_loop(bytes.fromhex('660f59ca'), 0))


def test_qpc_range_and_event_scaling():
    check = unittest.TestCase()
    with tempfile.TemporaryDirectory(prefix="woby_pmu_ranges_") as directory:
        root = Path(directory).resolve()
        session = root / "uprof-synthetic"
        session.mkdir()
        events = [{"encodedEventId": event + 0x1000000, "hwId": event, "mask": 0, "interval": interval}
                  for event, interval in ((0x76, 100), (0xC0, 200))]
        metadata = {"collection": {"events": {"pmcEvents": {"pmcEventList": events}},
                                   "schedule": {"start": 1_000_000_000, "elapsed": 200}}}
        (session / "session.uprof").write_text(json.dumps(metadata), encoding="utf-8")
        probe = {"compiler_isa": "SSE2", "clock_origin": {"steady_epoch_nanoseconds": 1_000_000_000},
                 "events": [{"name": "test_phase", "start_ms": 10, "end_ms": 110, "wall_ms": 100,
                             "user_ms": 100, "average_logical_cpus": 1}]}
        (root / "uprof-synthetic-console.log").write_text(json.dumps(probe) + "\n", encoding="utf-8")
        database = duckdb.connect(str(session / "cpu.db"))
        database.execute('CREATE TABLE Module(id INTEGER,path VARCHAR)')
        database.execute('INSERT INTO Module VALUES(1,?)', [str(root / "fixture.exe")])
        database.execute('''CREATE TABLE UnifiedSampleSeries(windowId INTEGER,moduleId INTEGER,moduleRVA INTEGER,
            coreId INTEGER,threadId INTEGER,
            "event#0x1000076" DOUBLE,"event#0x10000c0" DOUBLE)''')
        database.executemany('INSERT INTO UnifiedSampleSeries VALUES(?,1,0,0,100,?,?)',
                             [(9, 9, 9), (10, 2, 2), (109, 3, 3), (110, 4, 4)])
        database.close()
        result = summarize(session, root)
        phase = result["stages"][0]
        check.assertTrue(result["qpc_metadata_alignment"])
        check.assertFalse(result["exact_qpc_alignment"])
        check.assertEqual(phase["counts"]["076:00"]["samples"], 5)
        check.assertEqual(phase["counts"]["0c0:00"]["estimated_events"], 1000)
        check.assertEqual(phase["ipc_estimate"], 2)
        check.assertEqual(phase["alignment_sensitivity_plus_16ms"]["076:00"]["shifted_estimated_events"], 700)
        check.assertEqual(phase["instruction_sample_categories"], {"unresolved": 5})
        check.assertEqual([row[0] for row in result["timeline"]], [0, 100])
        check.assertEqual(result["timeline"][0][1:], (11, 11))
        check.assertEqual(phase["cycle_samples_by_thread"], [{"id": 100, "samples": 5, "percent": 100}])
        check.assertEqual(phase["cycle_samples_by_logical_cpu"], [{"id": 0, "samples": 5, "percent": 100}])


def test_map_alias_and_range_resolution():
    check = unittest.TestCase()
    functions = map_functions("""
 0001:00000000 funcA 0000000140001000 f i core:a.obj
 0001:00000000 aliasA 0000000140001000 f core:b.obj
 0001:00000020 funcB 0000000140001020 f core:b.obj
 0002:00000000 data 0000000140002000 f core:b.obj
""", 0x140000000)
    check.assertEqual(list(functions), [0x1000, 0x1020])
    check.assertEqual(len(functions[0x1000]), 2)
    check.assertIsNone(nearest_function(list(functions), 0xFFF))
    check.assertEqual(nearest_function(list(functions), 0x101F), 0x1000)
    check.assertEqual(nearest_function(list(functions), 0x1020), 0x1020)


def test_reuse_preserves_the_captured_binary():
    check = unittest.TestCase()
    with tempfile.TemporaryDirectory(prefix="woby_pmu_symbols_") as directory:
        root = Path(directory).resolve()
        exe, probe, session = root / "probe.exe", root / "probe.json", root / "session"
        session.mkdir()
        exe.write_bytes(b"captured executable")
        exe.with_suffix('.pdb').write_bytes(b"captured symbols")
        symbols = preserve_capture_symbols(exe, session, probe, {"compiler_isa": "SSE2"}, True)
        original_record = probe.read_bytes()
        exe.write_bytes(b"new build")
        exe.with_suffix('.pdb').write_bytes(b"new symbols")
        preserve_capture_symbols(exe, session, probe, {}, False)
        check.assertEqual((symbols / exe.name).read_bytes(), b"captured executable")
        check.assertEqual((symbols / 'probe.pdb').read_bytes(), b"captured symbols")
        check.assertEqual(probe.read_bytes(), original_record)
        (symbols / exe.name).write_bytes(b"wrong executable")
        with check.assertRaisesRegex(ValueError, 'capture hash'):
            preserve_capture_symbols(exe, session, probe, {}, False)


if __name__ == "__main__":
    tests = unittest.TestSuite(unittest.FunctionTestCase(test) for test in
                               (test_profile_generation_current_sources, test_generation_rejects_anchor_drift,
                                test_log_and_instruction_decoding, test_qpc_range_and_event_scaling,
                                test_map_alias_and_range_resolution, test_reuse_preserves_the_captured_binary,
                                test_memory_map_grouping, test_native_memory_map))
    result = unittest.TextTestRunner(verbosity=2).run(tests)
    raise SystemExit(not result.wasSuccessful())

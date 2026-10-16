# /// script
# requires-python = ">=3.11"
# dependencies = ["duckdb>=1.3", "pefile>=2024", "capstone>=5"]
# ///
"""Check calculation/move distinctions and timing-sample separation."""
import unittest
import duckdb
from parser_simd import opcode_kind, encoding, assembly_inventory, comparison, sample_rows


check = unittest.TestCase()


def test_scalar_and_packed_operations():
    check.assertEqual(opcode_kind("vmulsd", "xmm0, xmm1, xmm2"), "Scalar FP calculation / compare")
    check.assertEqual(opcode_kind("mulsd", "xmm0, xmm1"), "Scalar FP calculation / compare")
    check.assertEqual(opcode_kind("vaddpd", "ymm0, ymm1, ymm2"), "Packed calculation / compare")
    check.assertEqual(opcode_kind("vpcmpeqb", "ymm0, ymm1, ymm2"), "Packed calculation / compare")
    check.assertEqual(opcode_kind("vmovdqu", "ymm0, ymmword ptr [rax]"), "SIMD-register moves / setup")
    check.assertEqual(opcode_kind("vmovsd", "xmm0, qword ptr [rax]"), "SIMD-register moves / setup")
    check.assertEqual(opcode_kind("vxorps", "xmm0, xmm0, xmm0"), "SIMD-register moves / setup")
    check.assertEqual(opcode_kind("vpand", "ymm0, ymm1, ymm2"), "Vector logical / mask")
    check.assertEqual(opcode_kind("imul", "rax, rcx"), "Integer / control / other")
    check.assertEqual(encoding("shrx", "rax, rbx, rcx"), "BMI2 scalar integer")
    check.assertEqual(encoding("vmulsd", "xmm0, xmm1, xmm2"), "VEX / 128-bit register")

def test_proc_boundaries_and_static_locations():
    text = """?ParseFace@detail PROC ; function
    mov rax, rbx
    vmovdqu ymm0, YMMWORD PTR [rax]
    vpcmpeqb ymm0, ymm1, ymm2
?ParseFace@detail ENDP
?Other PROC
    vaddpd ymm0, ymm1, ymm2
?Other ENDP
?dtor$0@?ParseFace@detail PROC
    ret
?dtor$0@?ParseFace@detail ENDP
"""
    face = assembly_inventory(text)["General face references"]
    check.assertEqual(face["static_instruction_locations"], 3)
    check.assertEqual(face["kinds"]["Packed calculation / compare"], 1)

def test_unresolved_functions_and_process_thread_scope():
    with duckdb.connect(":memory:") as database:
        database.execute('CREATE TABLE Module(id INTEGER,path VARCHAR)')
        database.execute('CREATE TABLE Function(id INTEGER,name VARCHAR)')
        database.execute('''CREATE TABLE UnifiedSampleSeries(windowId INTEGER,processId INTEGER,threadId INTEGER,
            moduleId INTEGER,functionId INTEGER,moduleRVA INTEGER,"event#0x1000076" DOUBLE,"event#0x10000c0" DOUBLE)''')
        database.execute("INSERT INTO Module VALUES (1,'runtime.dll')")
        database.execute("INSERT INTO UnifiedSampleSeries VALUES (10,20,30,1,999,40,5,6),(10,21,30,1,999,40,7,8),(10,20,31,1,999,40,9,10)")
        check.assertEqual(sample_rows(database, 10, 11, 20, [30]), [("runtime.dll", "[unresolved function]", 40, 5, 6)])

def test_samples_excluded_and_fingerprint_checked():
    base = {"compiler_isa": "AVX2", "workers_requested": 16, "round": 1,
        "events": [{"name": name, "wall_ms": 3, "cpu_ms": 30, "average_logical_cpus": 10}
                   for name in ("parser_total", "parser_scan_collect", "parser_merge")],
        "geometry": {"fingerprint": 5}}
    result = comparison([base])
    check.assertEqual(result["groups"][0]["wall_ms"]["median"], 3)
    with check.assertRaisesRegex(ValueError, "Sampled runs"):
        comparison([dict(base, instruction_samples=[{"count": 1}])])
    with check.assertRaisesRegex(ValueError, "fingerprints differ"):
        comparison([base, dict(base, geometry={"fingerprint": 6})])



if __name__ == "__main__":
    test_scalar_and_packed_operations()
    test_proc_boundaries_and_static_locations()
    test_unresolved_functions_and_process_thread_scope()
    test_samples_excluded_and_fingerprint_checked()
    print("Four SIMD accounting checks passed")

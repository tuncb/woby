import unittest
import analysis
import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from modern_float import substitute


def records():
    return [dict(model='mesh',variant=v,round=r,fingerprint=42,total_ms=10+r,scan_ms=8+r)
            for v in ('legacy','prototype') for r in (1,2)]


class ComparisonContracts(unittest.TestCase):
    executable = None

    def test_decoder_substitution_requires_valid_boundaries(self):
        with self.assertRaisesRegex(ValueError, "boundary drift"):
            substitute("no embedded decoder", "namespace fast_float {}")
        first = "// Code below is from: https://github.com/fastfloat/fast_float/releases/download/v3.5.1/fast_float.h"
        last = "// clang-format on\n\n//\n// Third-party includes End"
        self.assertEqual(substitute("before\n" + first + "\nold\n" + last + "\nafter", "namespace fast_float {}"),
                         "before\n// Research decoder replacement; production source is unchanged.\nnamespace fast_float {}\n\n" + last + "\nafter")

    def test_preloaded_parser_preserves_attributes_and_negative_references(self):
        if self.executable is None:
            self.skipTest("No executable supplied")
        with tempfile.TemporaryDirectory(prefix="woby parser input contract ") as temporary:
            root = Path(temporary).resolve()
            model = root / "negative.obj"
            model.write_text("v 1.23456789012345 0 0\nv 0 1e-12 0\nv 0 0 -3.5\n"
                             "vt .1 .2\nvt .3 .4\nvt .5 .6\nvn 0 0 1\n"
                             "f -3/-3/-1 -2/-2/-1 -1/-1/-1\n", encoding="utf-8")
            rows = [json.loads(subprocess.check_output([str(self.executable), mode, str(model), "64", "2"],
                              cwd=root, text=True, timeout=20)) for mode in ("prototype", "prototype-memory")]
            self.assertEqual(rows[0]["fingerprint"], rows[1]["fingerprint"])
            self.assertEqual(rows[1]["retained_input_bytes"], model.stat().st_size)
            self.assertEqual(rows[1]["input_kind"], "preloaded-memory")
            self.assertEqual(rows[1]["faces"], 1)
            self.assertEqual(rows[1]["positions"], 3)

    def test_medians_and_ranges(self):
        result=analysis.summarize(records(),['mesh'],['legacy','prototype'],2)
        self.assertEqual(result[0]['variants']['prototype']['total_ms'],dict(median=11.5,minimum=11,maximum=12))
        self.assertEqual(analysis.regressions(result),[])

    def test_missing_and_duplicated_rounds_fail(self):
        for missing in (True,False):
            rows=records()
            if missing: rows.pop()
            else: rows[-1]['round']=1
            with self.assertRaisesRegex(ValueError,'rounds'):
                analysis.summarize(rows,['mesh'],['legacy','prototype'],2)

    def test_changed_geometry_and_invalid_times_fail(self):
        for key,value in [('fingerprint',3),('total_ms',float('nan')),('scan_ms',-1)]:
            rows=records(); rows[-1][key]=value
            with self.assertRaises(ValueError):
                analysis.summarize(rows,['mesh'],['legacy','prototype'],2)

    def test_regression_gate_does_not_hide_a_slower_model_in_an_average(self):
        rows=records()
        for row in rows:
            if row['variant']=='prototype': row['total_ms']*=1.2
        result=analysis.summarize(rows,['mesh'],['legacy','prototype'],2)
        self.assertEqual(analysis.regressions(result)[0]['model'],'mesh')

    def test_full_precision_mismatch_cannot_hide_behind_float_fingerprints(self):
        rows=records()
        for row in rows:
            row['precise_fingerprint']=10 if row['variant']=='legacy' else 20
        analysis.summarize(rows,['mesh'],['legacy','prototype'],2)
        rows[-1]['precise_fingerprint']=21
        with self.assertRaisesRegex(ValueError,'Double-precision'):
            analysis.summarize(rows,['mesh'],['legacy','prototype'],2)


if __name__=='__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", type=Path)
    args, remaining = parser.parse_known_args()
    ComparisonContracts.executable = args.executable.resolve(strict=True) if args.executable else None
    unittest.main(argv=[sys.argv[0], *remaining])

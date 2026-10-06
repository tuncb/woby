import unittest
import analysis


def records():
    return [dict(model='mesh',variant=v,round=r,fingerprint=42,total_ms=10+r,scan_ms=8+r)
            for v in ('legacy','prototype') for r in (1,2)]


class ComparisonContracts(unittest.TestCase):
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


if __name__=='__main__': unittest.main()

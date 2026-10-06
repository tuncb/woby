"""Contracts for paired comparisons, missing data, and instrumentation drift."""
import copy
import unittest

import analyze
import generate
import idle


def sample(variant='legacy', round_index=1):
    return dict(workload='mesh', variant=variant, round=round_index, geometry={'fileCount':1,'vertexCount':3},
        image={'size':[10,10],'sha256':'same pixels'}, mesh_capacities={'render_vertex_bytes':96},
        preparation_capacities={'upload_bytes':108}, annotation_capacities={'snapshot_vertex_bytes':96},
        timings={'fully_prepared_ms':10.0}, phases={'parse':2.0}, memory={'copied_position_bytes':72 if variant!='prototype' else 0},
        steady={'gpu_ms':{'median':1.0},'cpu_submit_ms':{'median':.5},'frame_interval_ms':{'median':4.2}},
        navigation=None, pacing={'targetFps':240}, annotation_ready=True)


class Comparisons(unittest.TestCase):
    def test_cpu_spike_requires_a_new_quiet_window(self):
        samples=iter([17,3,18,4,5])
        self.assertEqual(idle.wait_for_quiet(lambda:next(samples),10),[4,5])
        with self.assertRaises(StopIteration): next(samples)

    def test_invalid_cpu_measurements_cannot_pass_the_guard(self):
        for invalid in (-1,101,float('nan')):
            with self.assertRaises(ValueError): idle.wait_for_quiet(lambda:invalid,10)
            with self.assertRaises(ValueError): idle.wait_for_quiet(lambda:1,invalid)

    def setUp(self):
        self.manifest={'workloads':[{'id':'mesh'}], 'variants':['legacy','prototype'], 'rounds':2}
        self.runs=[sample(v,r) for v in self.manifest['variants'] for r in (1,2)]

    def test_each_process_has_equal_weight(self):
        self.runs[0]['steady']['gpu_ms']={'median':2.0,'count':2}
        self.runs[1]['steady']['gpu_ms']={'median':8.0,'count':2000}
        result=analyze.compare(self.runs,self.manifest)[0]['variants']['legacy']
        self.assertEqual(result['render']['steady_gpu_ms']['median'],5.0)
        self.assertEqual(result['render']['steady_gpu_ms']['samples'],2)

    def test_missing_round_is_rejected(self):
        with self.assertRaisesRegex(ValueError,'Missing or repeated rounds'):
            analyze.compare(self.runs[:-1],self.manifest)

    def test_duplicate_round_is_rejected(self):
        self.runs[-1]['round']=1
        with self.assertRaisesRegex(ValueError,'Missing or repeated rounds'):
            analyze.compare(self.runs,self.manifest)

    def test_changed_pixels_are_not_a_speedup(self):
        self.runs[-1]['image']['sha256']='different pixels'
        with self.assertRaisesRegex(ValueError,'Mismatched image'):
            analyze.compare(self.runs,self.manifest)

    def test_changed_geometry_is_rejected(self):
        self.runs[-1]['geometry']['vertexCount']=4
        with self.assertRaisesRegex(ValueError,'Mismatched geometry'):
            analyze.compare(self.runs,self.manifest)

    def test_copy_ablation_may_change_temporary_memory(self):
        result=analyze.compare(self.runs,self.manifest)[0]
        self.assertEqual(result['variants']['prototype']['memory']['copied_position_bytes']['median'],0)
        self.assertEqual(result['variants']['legacy']['memory']['copied_position_bytes']['median'],72)

    def test_missing_gpu_timing_is_not_zero(self):
        for row in self.runs:
            row['steady']['gpu_ms']=None
        result=analyze.compare(self.runs,self.manifest)[0]
        self.assertIsNone(result['variants']['legacy']['render']['steady_gpu_ms'])

    def test_no_negative_or_nan_timings(self):
        for value in (-1,float('nan'),float('inf')):
            with self.assertRaises(ValueError):
                analyze.values([1,value])

    def test_interfered_measurements_are_rejected(self):
        with self.assertRaisesRegex(ValueError,'interfered'):
            analyze.summarize_run({'status':'interfered'})

    def test_anchor_drift_fails_closed(self):
        for source in ('missing', 'old old'):
            with self.assertRaises(ValueError):
                generate.replace(source,'old','new')
        self.assertEqual(generate.replace('prefix old suffix','old','new'),'prefix new suffix')


if __name__=='__main__':
    unittest.main()

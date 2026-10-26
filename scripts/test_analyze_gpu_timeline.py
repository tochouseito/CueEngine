"""GPU Clock 対応の桁落ち、境界、Frame 結合を検証する。"""

import csv
import tempfile
import unittest
from fractions import Fraction
from pathlib import Path

from analyze_gpu_timeline import analyze, project


class GpuTimelineTests(unittest.TestCase):
    def test_large_ticks_keep_submicrosecond_precision(self):
        tick = 1_792_393_187_287_185_408
        calibration = {'cpu_tick': 12345678900000, 'begin_tick': tick, 'frequency': 1_000_000_000}
        self.assertEqual(project(tick + 1, calibration, 10_000_000),
                         Fraction(12345678900000) + Fraction(1, 100))

    def test_frame_join_and_boundary_are_explicit(self):
        fields = ('kind,frame,queue,queue_type,detail,name,begin_tick,end_tick,frequency,cpu_tick,'
                  'qpc_before,qpc_after,status').split(',')
        base = 1_792_393_187_287_185_408
        rows = [
            [0, 10, 7, 0, 11, 'BeforeSubmit', base, 0, 1_000_000_000, 1000, 999, 1001, 0],
            [1, 10, 7, 0, 11, 'ExecuteCommandLists', 0, 0, 0, 0, 1010, 1060, 0],
            [2, 10, 7, 0, 0, 'Pass', base + 5000, base + 6000, 1_000_000_000, 0, 0, 0, 0],
            [0, 11, 7, 0, 12, 'BeforeSubmit', base + 20000, 0, 1_000_000_000, 1200, 1199, 1201, 0],
            [2, 9, 7, 0, 0, 'Warmup', base - 5000, base - 4000, 1_000_000_000, 0, 0, 0, 0],
        ]
        with tempfile.TemporaryDirectory() as folder:
            prefix = Path(folder) / 'sample'
            Path(str(prefix) + '.log').write_text(
                'etw_clock qpc_frequency=10000000 measurement_start_ns=100000 '
                'start_qpc_before=999 start_qpc_after=1001 start_steady_ns=100000 '
                'stop_qpc_before=1999 stop_qpc_after=2001 stop_steady_ns=200000\n', encoding='utf-8')
            Path(str(prefix) + '-spans.csv').write_text(
                'name,frame,begin_ns,end_ns,duration_ns\nFrame.Render,10,0,10000,10000\n', encoding='utf-8')
            with Path(str(prefix) + '-gpu.csv').open('w', newline='', encoding='utf-8') as file:
                writer = csv.writer(file)
                writer.writerow(fields)
                writer.writerows(rows)
            result = analyze(prefix)
        gpu = result['top_render'][0]['gpu'][0]
        self.assertEqual(gpu['frame'], 10)
        self.assertEqual(gpu['fence'], 11)
        self.assertEqual(gpu['first_gpu_ns'], 5000)
        self.assertEqual(gpu['last_gpu_ns'], 6000)
        self.assertLess(gpu['first_gpu_ns'] - gpu['submit_end_ns'], 0)
        self.assertEqual(gpu['calibration_sensitivity_ns'], 0)
        self.assertEqual(result['quality']['unavailable'][0]['frame'], 9)

    def test_calibration_drift_is_visible(self):
        earlier = {'begin_tick': 10**18, 'cpu_tick': 1000, 'frequency': 10**9}
        later = {'begin_tick': 10**18 + 20000, 'cpu_tick': 1210, 'frequency': 10**9}
        self.assertEqual(project(10**18 + 5000, later, 10**7) -
                         project(10**18 + 5000, earlier, 10**7), 10)


if __name__ == '__main__':
    unittest.main()

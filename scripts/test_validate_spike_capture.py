"""循環保持の前後範囲、不足 Provider、保存範囲内の連番欠落を検証する。"""

import csv
import json
import tempfile
import unittest
from pathlib import Path

from validate_spike_capture import validate


class CaptureCoverageTests(unittest.TestCase):
    def fixture(self, folder, *, missing_range=False, sequence_gap=False):
        prefix = folder / 'on-q2'
        prefix.with_suffix('.log').write_text(
            'trigger kind=1 frame=100 duration_ns=20000000 at_ns=6000000000\n'
            'etw_clock qpc_frequency=10000000 measurement_start_ns=10000000000 '
            'start_qpc_before=100000000 start_qpc_after=100000000 start_steady_ns=10000000000\n'
            'window_checks=100 mismatches=0 changes=0 final_state=11\n'
            'trace_spans=2 dropped=0\ngpu_records=1 dropped=0 overwritten=20\n', encoding='utf-8')
        with Path(str(prefix) + '-spans.csv').open('w', newline='', encoding='utf-8') as file:
            writer = csv.writer(file)
            writer.writerow(['thread', 'frame', 'name', 'begin_ns', 'end_ns', 'source_sequence'])
            writer.writerow([5, 0, 'Coverage', 0, 8_100_000_000, 100])
            writer.writerow([5, 100, 'Frame.Render', 5_980_000_000, 6_000_000_000,
                             102 if sequence_gap else 101])
        with (folder / 'Events.csv').open('w', newline='', encoding='utf-8') as file:
            writer = csv.writer(file)
            writer.writerow(['kind', 'name', 'qpc', 'detail', 'sequence', 'old_tid', 'new_tid'])
            writer.writerow(['CSwitch', '', 100000000, 0, 0, 5, 0])
            writer.writerow(['CSwitch', '', 182000000, 0, 0, 0, 5])
            for name in ('DxgKrnl', 'DXGI', 'DWM'):
                first = 120000000 if missing_range and name == 'DWM' else 100000000
                writer.writerow(['ProviderRange', name, first, 182000000, 10, 0, 0])
        (folder / 'Done.json').write_text(json.dumps(dict.fromkeys(
            ('KernelSaved', 'UserSaved', 'KernelStopped', 'UserStopped', 'MarkersStopped', 'Merged'), True) |
            {'Error': ''}), encoding='utf-8')
        (folder / 'Export.log').write_text(
            'process_trace=0 decode_errors=0 events_lost=0 buffers_lost=0 frequency=10000000 ', encoding='utf-8')

    def test_retired_sequences_are_not_missing_samples(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            self.fixture(folder)
            self.assertTrue(validate(folder)['succeeded'])

    def test_insufficient_provider_history_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            self.fixture(folder, missing_range=True)
            result = validate(folder)
            self.assertFalse(result['succeeded'])
            self.assertFalse(result['checks']['DWM'])

    def test_gap_inside_retained_samples_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            self.fixture(folder, sequence_gap=True)
            with self.assertRaises(ValueError):
                validate(folder)

    def test_no_trigger_is_distinct_from_incomplete_data(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            (folder / 'on-q2.log').write_text(
                'trigger kind=0 frame=0 duration_ns=0 at_ns=0\n', encoding='utf-8')
            self.assertEqual(validate(folder)['status'], 'not_triggered')


if __name__ == '__main__':
    unittest.main()

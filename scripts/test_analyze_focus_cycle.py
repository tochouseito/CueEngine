"""切替の跨ぎと実測 PID 不一致を比較成功に含めないことを検証する。"""

import unittest

from analyze_focus_cycle import stable_phase, validate_focus, validate_window_shape


class FocusTests(unittest.TestCase):
    def test_transition_boundaries(self):
        self.assertIsNone(stable_phase(3_740_000_000, 4_010_000_000))
        self.assertIsNone(stable_phase(4_010_000_000, 4_020_000_000))
        self.assertEqual(stable_phase(4_250_000_000, 7_750_000_000), 1)

    def rows(self):
        return [dict(time_ns=phase * 4_000_000_000 + delta, kind=kind, phase=phase,
                     expected_pid=10 if phase % 2 == 0 else 20, foreground_pid=10 if phase % 2 == 0 else 20,
                     success=1) for phase in range(5) for delta, kind in [(0, 'request'), (1000, 'observed')]]

    def test_observed_condition(self):
        rows = self.rows()
        self.assertEqual(validate_focus(rows), [])
        rows[3]['foreground_pid'] = 30
        self.assertIn('phase_1_observed_process', validate_focus(rows))

    def test_failed_and_missing_request(self):
        rows = self.rows()
        rows[6]['success'] = 0
        self.assertIn('phase_3_request', validate_focus(rows))
        self.assertIn('phase_requests', validate_focus(rows[1:]))

    def test_window_shape_independent_of_foreground(self):
        self.assertEqual(validate_window_shape([dict(name='Probe.WindowState', detail=s) for s in [11, 10, 11]]), [])
        for state in [3, 9, 14]:
            self.assertIn('window_shape', validate_window_shape([dict(name='Probe.WindowState', detail=state)]))

    def test_missing_window_shape_is_not_success(self):
        self.assertIn('missing_window_shape', validate_window_shape([dict(name='Frame.Render', detail=0)]))


if __name__ == '__main__':
    unittest.main()

"""Scheduling 解析の境界と未観測区間の扱いを決定的な時系列で確認する。"""

import unittest
from fractions import Fraction

from analyze_spike_etw import build_states, duration_by_state


def switch(at, old, new, state):
    """ETW Export の CSwitch と同じ必要情報を用意する。"""
    return dict(kind="CSwitch", qpc=at, old_tid=old, new_tid=new,
                old_state=state, wait_reason=13)


class SchedulingTests(unittest.TestCase):
    """待機と再開、Ready 再通知、採取範囲外を独立に検証する。"""

    def test_wake_preemption_and_clipped_span(self):
        """Ready の再通知で最初の起床時刻を失わず、CPU の途中区間を切り出す。"""
        events = [switch(0, 0, 10, 1), switch(10, 10, 0, 5),
                  dict(kind="ReadyThread", qpc=25, ready_tid=10),
                  dict(kind="ReadyThread", qpc=28, ready_tid=10),
                  switch(30, 0, 10, 1), switch(40, 10, 0, 1),
                  switch(50, 0, 10, 1), switch(60, 10, 0, 4)]
        states = build_states(events, {10})
        totals = duration_by_state(states[10], Fraction(5), Fraction(55))
        self.assertEqual(totals["Running"], 20)
        self.assertEqual(totals["Waiting"], 15)
        self.assertEqual(totals["Ready"], 15)
        self.assertEqual(totals["Unknown"], 0)
        self.assertEqual(sum(totals.values()), 50)

    def test_outside_capture_is_unknown(self):
        """観測がない時間を Ready や Running の 0 ms へ混ぜない。"""
        totals = duration_by_state([(10, 20, "Running", 0)], Fraction(5), Fraction(25))
        self.assertEqual(totals["Running"], 10)
        self.assertEqual(totals["Unknown"], 10)

    def test_missing_ready_is_not_invented(self):
        """Ready の境界がない場合に全てを Waiting と断定しない。"""
        states = build_states([switch(0, 0, 10, 1), switch(10, 10, 0, 5),
                               switch(30, 0, 10, 1), switch(40, 10, 0, 4)], {10})
        totals = duration_by_state(states[10], Fraction(10), Fraction(30))
        self.assertEqual(totals["Unknown"], 20)
        self.assertEqual(totals["Waiting"], 0)


if __name__ == "__main__":
    unittest.main()

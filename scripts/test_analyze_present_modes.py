"""QPC の巨大値・不確かさ・曖昧な隣接 Present を検証する。"""

import unittest
from fractions import Fraction

from analyze_present_modes import match_present


class PresentModesTests(unittest.TestCase):
    def test_exact_and_large_ticks(self):
        base = 10**18
        self.assertEqual(match_present(Fraction(base * 2 - 1, 2), base + 2, [base, base + 10], 0), 0)
        self.assertEqual(match_present(base + 8, base + 12, [base, base + 10], 0), 1)

    def test_missing_and_ambiguous(self):
        self.assertIsNone(match_present(3, 7, [0, 10], 0))
        self.assertIsNone(match_present(0, 10, [0, 10], 0))

    def test_uncertainty_does_not_pick_nearest(self):
        self.assertEqual(match_present(11, 12, [10], Fraction(3, 2)), 0)
        self.assertIsNone(match_present(11, 12, [10, 13], Fraction(3, 2)))


if __name__ == '__main__':
    unittest.main()

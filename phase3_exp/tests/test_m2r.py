"""M2-R checks that do not touch the locked design-point parameters."""
from __future__ import annotations

import unittest

from phase3_exp.mc.e2_revise import _trial
from phase3_exp.mc.mc_sim import rates


class E2Revise(unittest.TestCase):
    def test_none_can_flip_and_fix_stays_censored(self):
        none = [_trial(i, n=64, r=1, attack="A1", scheme="NONE", r1=75) for i in range(12)]
        self.assertTrue(any(row["class"] == "FLIP" for row in none))
        r0, r1 = rates(14531, 1.5, 2.0, 64)
        fix = [_trial(i, n=64, r=r1, attack="A1", scheme="FIX", r1=r1) for i in range(12)]
        self.assertTrue(all(row["class"] == "RIGHT_CENSORED" for row in fix))
        self.assertTrue(all(row["flip_at"] is None for row in fix))
        self.assertTrue(all(row["ratio"] < 1.0 for row in fix))


if __name__ == "__main__":
    unittest.main()

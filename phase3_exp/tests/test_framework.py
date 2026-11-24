"""Unit tests that do not need the DRAM simulator."""
from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

from phase3_exp.harness.campaign import write_run
from phase3_exp.harness.classify import classify
from phase3_exp.harness.gate import GateClosed, require
from phase3_exp.harness.seeds import STREAMS, mc_seed, named_streams, sim_seed
from phase3_exp.harness.stats import percentile, wilson, zero_escape_upper
from phase3_exp.harness.workloads import expand_job, load_workloads
from phase3_exp.mc.mc_sim import aggressor_share, rates, smoke
from phase3_exp.plot import style
from phase3_exp.plot.plot_paper import figure_2a


class FrameworkTest(unittest.TestCase):
    def test_wilson_matches_the_locked_formula(self):
        z = 1.959964
        k, n = 0, 100
        p, low, high = wilson(k, n, z)
        self.assertEqual(p, 0.0)
        z2 = z * z
        denom = 1.0 + z2 / n
        center = (z2 / (2 * n)) / denom
        half = z * (z2 / (4 * n * n)) ** 0.5 / denom
        self.assertAlmostEqual(low, 0.0)
        self.assertAlmostEqual(high, min(1.0, center + half))
        self.assertAlmostEqual(zero_escape_upper(100), 0.03)
        self.assertAlmostEqual(zero_escape_upper(300), 0.01)

    def test_seeds_and_independent_streams(self):
        self.assertEqual(mc_seed(0), 20260000)
        self.assertEqual(sim_seed("grid", 0), sim_seed("grid", 0))
        self.assertNotEqual(sim_seed("grid", 0), sim_seed("grid", 1))
        first = named_streams(7)
        second = named_streams(7)
        for name in STREAMS:
            self.assertEqual(first[name].random(), second[name].random())
        fresh = named_streams(7)
        fresh["para"].random()
        other = named_streams(7)
        self.assertEqual(fresh["hc_first"].random(), other["hc_first"].random())

    def test_outcomes_are_exclusive(self):
        self.assertIsNone(classify(status="FAILED", bit_exact=True, flip_count=0, replayed=False,
                                   withheld=False, recovery_failed=False))
        self.assertEqual(classify(status="SUCCESS", bit_exact=False, flip_count=0, replayed=True,
                                  withheld=False, recovery_failed=False,
                                  phase2_status="SELECTIVE_RECOVERED")["outcome"], "O6")
        self.assertEqual(classify(status="SUCCESS", bit_exact=True, flip_count=2, replayed=False,
                                  withheld=True, recovery_failed=True)["outcome"], "O5")
        self.assertEqual(classify(status="SUCCESS", bit_exact=True, flip_count=1, replayed=True,
                                  withheld=False, recovery_failed=False,
                                  phase2_status="WHOLE_JOB_FALLBACK")["outcome"], "O4")
        self.assertEqual(classify(status="SUCCESS", bit_exact=True, flip_count=1, replayed=True,
                                  withheld=False, recovery_failed=False,
                                  phase2_status="SELECTIVE_RECOVERED")["outcome"], "O3")
        self.assertEqual(classify(status="SUCCESS", bit_exact=True, flip_count=1, replayed=False,
                                  withheld=False, recovery_failed=False)["outcome"], "O2")
        self.assertEqual(classify(status="SUCCESS", bit_exact=True, flip_count=0, replayed=False,
                                  withheld=False, recovery_failed=False)["outcome"], "O1")

    def test_gate_stops_m1(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "gate.json"
            path.write_text(json.dumps({"G1": "PENDING", "G2": "PENDING", "G3": "PENDING", "G4": "PENDING"}))
            require("M0", path)
            with self.assertRaises(GateClosed):
                require("M1", path)

    def test_campaign_refuses_overwrite_and_mixed_scale(self):
        with tempfile.TemporaryDirectory() as tmp:
            from phase3_exp.harness import campaign
            campaign.RUNS = Path(tmp) / "runs"
            path = write_run("M0", "E0", "scale_real", "g", 1, {"STATUS": "SUCCESS"})
            with self.assertRaises(FileExistsError):
                write_run("M0", "E0", "scale_real", "g", 1, {"STATUS": "SUCCESS"})
            self.assertTrue(path.exists())
            with self.assertRaises(ValueError):
                write_run("M0", "E0", "mixed", "g", 2, {"STATUS": "SUCCESS"})

    def test_w4_fanout_is_16(self):
        jobs = {job["name"]: job for job in load_workloads()}
        text = expand_job(jobs["W4"])
        self.assertEqual(text.count("kernel MUL"), 16)
        self.assertIn("kernel ADD", text)
        self.assertIn("kernel RELU", text)

    def test_rates_match_the_table(self):
        self.assertEqual(rates(14531, 1.5, 2, 64), (954, 75))
        self.assertEqual(rates(14531, 1.1, 2, 64), (1395, 103))

    def test_mc_smoke_is_reproducible_and_monotonic(self):
        result = smoke(trials=4, monotonicity_trials=3)
        self.assertTrue(result["reproducible"])
        self.assertTrue(result["monotonic"])
        self.assertGreater(result["mttf_flip_mean_r1_75"], result["mttf_flip_mean_r1_20000"])
        self.assertLess(aggressor_share("A3"), 1.0 / 16.0)

    def test_fig2a_draws_guides_and_all_workloads(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            csv_path = root / "E1_benign_profile.csv"
            csv_path.write_text(
                "workload,window_id,bank_id,max_row_act,total_bank_act,f_max,f_p95,f_p99,"
                "mean_open_cycles,p99_open_cycles\n"
                "W1,0,0,1,100,0.01,0.004,0.008,10,20\n"
                "W2,0,0,2,100,0.02,0.005,0.009,10,20\n"
                "W4,0,0,3,100,0.03,0.006,0.010,10,20\n"
                "GEMV,0,0,4,100,0.04,0.007,0.011,10,20\n"
                "DEC,0,0,5,100,0.05,0.008,0.012,10,20\n"
            )
            figure_2a(csv_path, root / "figures")
            from phase3_exp.plot.pdftext import has_label, pdf_bytes
            blob = pdf_bytes(root / "figures/fig2a_e1_benign.pdf")
            for token in ("W1", "W2", "W4", "GEMV", "DEC", "f_th"):
                self.assertTrue(has_label(blob, token), token)
            self.assertIn("0.0625", (root / "figures/manifest.json").read_text())
            source = Path(style.__file__).read_text()
            plot_source = (Path(style.__file__).parent / "plot_paper.py").read_text()
            self.assertNotIn("rcParams", plot_source)
            self.assertIn("DejaVu Serif", source)
            self.assertEqual(percentile([0.0, 1.0], 0.99), 0.99)


if __name__ == "__main__":
    unittest.main()

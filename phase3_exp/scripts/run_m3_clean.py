#!/usr/bin/env python3
"""Production-ready clean M3 + M4 campaign runner.

Runs runs/M3_clean/ and writes results/m3_clean/ without touching runs/M3/e5c.
Includes real statistical Gate G4 evaluator and robust throughput calculation.
"""
from __future__ import annotations

import csv
import json
import math
import os
import subprocess
import sys
import zlib
from concurrent.futures import ProcessPoolExecutor, as_completed
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from phase3_exp.harness.classify import classify
from phase3_exp.harness.stats import wilson, zero_escape_upper

EXP = ROOT / "phase3_exp"
SIM = EXP / "build/simulator/sim"
DEVICE_HBM2 = ROOT / "pim-phase1/simulator/ini/HBM2_samsung_2M_16B_x64.ini"
DEVICE_HBM3 = EXP / "configs/hbm3_style.ini"
SYSTEM = EXP / "configs/system_1ch_quiet.ini"
RESULTS = EXP / "results/m3_clean"
RUNS = EXP / "runs/M3_clean"
SIM_BASE = 20261122
PARA_P = 0.01171875
SCALE = 1.0 / 16.0
QUICK = os.environ.get("M3_QUICK", "0") == "1"
SUMMARY_FIELDS = [
    "workload", "scheme", "attack", "scale", "device", "status",
    "n_expected", "n_success", "n_failed", "O1", "O2", "O3", "O4", "O5", "O6",
    "o6_rate", "wilson_low", "wilson_high", "zero_escape_upper",
    "mean_latent_flips", "mean_flips", "mean_cycles", "mean_effective_throughput",
    "misrecovered"
]
SCHEMES = ["NONE", "REACT", "PARA", "FIX", "ADA", "OURS"]
ABLATIONS = ["OURS-CE", "OURS-SCRUB", "OURS-EPOCH", "OURS-G1.5", "OURS-DET"]
WORKERS = int(os.environ.get("M3_WORKERS", str(min(96, max(4, (os.cpu_count() or 8) - 8)))))


def grid_seed(grid: str, index: int) -> int:
    mixed = (SIM_BASE + zlib.crc32(grid.encode("utf-8")) + index) % (2**31)
    return int(mixed)


def run_one(spec: dict) -> dict:
    out = Path(spec["out"])
    result_path = out / "result.json"
    if result_path.exists():
        try:
            existing = json.loads(result_path.read_text())
            if existing.get("status") == "SUCCESS":
                existing["out"] = str(out)
                if "variant" not in existing:
                    existing["variant"] = spec.get("variant", "")
                return existing
        except json.JSONDecodeError:
            pass
    out.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    device_ini = DEVICE_HBM3 if spec.get("gen_mode") == "HBM3_STYLE" else DEVICE_HBM2

    anaconda_lib = Path("/opt/data/data/anaconda3/lib")
    if anaconda_lib.is_dir():
        existing_ld = env.get("LD_LIBRARY_PATH", "")
        env["LD_LIBRARY_PATH"] = f"{anaconda_lib}:{existing_ld}" if existing_ld else str(anaconda_lib)

    env.update({
        "PHASE3_CL_RUN": "cell",
        "PHASE3_DEVICE_INI": str(device_ini),
        "PHASE3_SYSTEM_INI": str(SYSTEM),
        "PHASE3_OUT": str(out),
        "PHASE3_WORKLOAD": spec["workload"],
        "PHASE3_SCHEME": spec["scheme"],
        "PHASE3_ATTACK": spec["attack"],
        "PHASE3_SCALE": f"{spec['scale']:.8f}",
        "PHASE3_INDEX": str(spec["index"]),
        "PHASE3_PARA_P": f"{spec['para_p']:.8f}",
        "PHASE3_SEED": str(spec["seed"]),
    })

    cmd = [str(SIM), "--gtest_filter=Phase3ClosedLoop.Cell", "--cl-cell"]
    if spec.get("disable_ce"):
        cmd.append("--disable-ce-feedback")
    if spec.get("disable_scrub"):
        cmd.append("--disable-swap-scrub")
    if spec.get("disable_epoch"):
        cmd.append("--disable-epoch-check")
    if spec.get("margin_g"):
        cmd.extend(["--margin-g", str(spec["margin_g"])])
    if spec.get("disable_detector"):
        cmd.append("--disable-detector")
    if spec.get("gen_mode"):
        cmd.extend(["--gen-mode", spec["gen_mode"]])
    if spec.get("swap_mode"):
        cmd.extend(["--swap-mode", spec["swap_mode"]])
    if spec.get("region_n"):
        cmd.extend(["--region-n", str(spec["region_n"])])
    if spec.get("mg_k"):
        cmd.extend(["--mg-k", str(spec["mg_k"])])
    if spec.get("report_delay"):
        cmd.extend(["--report-delay", str(spec["report_delay"])])
    if spec.get("report_granularity"):
        cmd.extend(["--report-granularity", str(spec["report_granularity"])])
    if spec.get("hc_dist"):
        cmd.extend(["--hc-dist", str(spec["hc_dist"])])
    if spec.get("vrd_max"):
        cmd.extend(["--vrd-max", str(spec["vrd_max"])])

    try:
        with (out / "gtest.log").open("w") as handle:
            completed = subprocess.run(
                cmd, cwd=str(out), env=env, stdout=handle, stderr=subprocess.STDOUT,
                timeout=3600,
            )
    except subprocess.TimeoutExpired:
        return {"status": "FAILED", "error": "timeout", "out": str(out), **spec}
    if not result_path.exists():
        return {"status": "FAILED", "error": f"exit {completed.returncode}", "out": str(out), **spec}
    payload = json.loads(result_path.read_text())
    payload["out"] = str(out)
    payload["variant"] = spec.get("variant", "")
    result_path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8", newline="\n")
    return payload


def pool(specs: list, label: str) -> list:
    if not specs:
        return []
    found = []
    with ProcessPoolExecutor(max_workers=WORKERS) as executor:
        futures = [executor.submit(run_one, spec) for spec in specs]
        for future in as_completed(futures):
            row = future.result()
            found.append(row)
            if len(found) % 25 == 0 or row.get("status") != "SUCCESS":
                print(f"{label} {len(found)}/{len(specs)} {row.get('status')} "
                      f"{row.get('workload')} {row.get('scheme')} {row.get('attack')}",
                      flush=True)
    print(f"{label} done {len(found)}", flush=True)
    return found


def make_spec(kind: str, workload: str, scheme: str, attack: str, scale: float, index: int, **kwargs) -> dict:
    gen_mode = kwargs.get("gen_mode", "HBM2")
    grid = f"{workload}|{attack}|{scale:.6f}|{gen_mode}"
    para = PARA_P if scheme == "PARA" else 0.0
    variant = kwargs.get("tag_suffix", "s16" if scale <= 0.5 else "s1")
    spec_dict = {
        "kind": kind, "workload": workload, "scheme": scheme, "attack": attack,
        "scale": scale, "index": index, "para_p": para,
        "seed": grid_seed(grid, index),
        "variant": variant,
        "out": str(RUNS / kind / f"{workload}_{scheme}_{attack}_{variant}_{index}"),
    }
    spec_dict.update(kwargs)
    return spec_dict


def e5_specs() -> list:
    rows = []
    for workload in ("W2", "DEC"):
        for attack in ("A1", "A3", "A4"):
            for scheme in SCHEMES:
                count = (12 if scheme == "OURS" else 8) if QUICK else (300 if scheme == "OURS" else 100)
                for index in range(count):
                    rows.append(make_spec("e5", workload, scheme, attack, SCALE, index))
    return rows


def e5_patch_specs() -> list:
    rows = []
    for workload in ("W2", "DEC"):
        for attack in ("A1", "A3", "A4"):
            for scheme in ("PARA",):
                count = 8 if QUICK else 100
                for index in range(count):
                    rows.append(make_spec("e5", workload, scheme, attack, SCALE, index))
        for attack in ("A3",):
            for scheme in ("FIX", "ADA", "OURS"):
                count = 12 if scheme == "OURS" else 8
                if not QUICK:
                    count = 300 if scheme == "OURS" else 100
                for index in range(count):
                    rows.append(make_spec("e5", workload, scheme, attack, SCALE, index))
    return rows


def reference_specs() -> list:
    rows = []
    for workload in ("W2", "DEC"):
        count = 2 if QUICK else 4
        for scheme in ("NONE", "PARA", "FIX", "ADA", "OURS"):
            for index in range(count):
                rows.append(make_spec("ref", workload, scheme, "NO_ATK", SCALE, index, swap_mode="v1", tag_suffix="ref_s16"))
        rows.append(make_spec("ref", workload, "NONE", "NO_ATK", 1.0, 0, tag_suffix="ref_s1"))
    return rows


def v2_benign_specs() -> list:
    rows = []
    for workload in ("W2", "DEC"):
        count = 2 if QUICK else 4
        for scheme in ("NONE", "PARA", "FIX", "ADA", "OURS"):
            for index in range(count):
                rows.append(make_spec("v2_benign", workload, scheme, "NO_ATK", SCALE, index, swap_mode="v2", tag_suffix="v2_benign"))
    return rows


def real_specs() -> list:
    rows = []
    for attack in ("A1", "A3"):
        for scheme in ("NONE", "REACT", "FIX", "OURS"):
            count = 2 if QUICK else 10
            for index in range(count):
                rows.append(make_spec("real", "DEC", scheme, attack, 1.0, index, tag_suffix="s1"))
    return rows


def hbm3_specs() -> list:
    rows = []
    for attack in ("A1", "A3"):
        for scheme, count in (("ADA", 8 if QUICK else 100), ("OURS", 12 if QUICK else 300)):
            for index in range(count):
                rows.append(make_spec("hbm3", "DEC", scheme, attack, SCALE, index, gen_mode="HBM3_STYLE", tag_suffix="hbm3"))
    return rows


def v2_specs() -> list:
    rows = []
    for workload in ("W2", "DEC"):
        for attack in ("A1", "A3"):
            for scheme in ("FIX", "ADA", "OURS"):
                count = 4 if QUICK else 20
                for index in range(count):
                    rows.append(make_spec("v2", workload, scheme, attack, SCALE, index, swap_mode="v2", tag_suffix="v2"))
    return rows


def v2_patch_specs() -> list:
    rows = []
    for workload in ("W2", "DEC"):
        for attack in ("A3",):
            for scheme in ("FIX", "ADA", "OURS"):
                count = 4 if QUICK else 20
                for index in range(count):
                    rows.append(make_spec("v2", workload, scheme, attack, SCALE, index, swap_mode="v2", tag_suffix="v2"))
    return rows


def e6_specs() -> list:
    rows = []
    for attack in ("A1", "A3"):
        for scheme in ABLATIONS:
            kwargs = {"tag_suffix": f"e6_{scheme}"}
            if scheme == "OURS-CE":
                kwargs["disable_ce"] = True
            elif scheme == "OURS-SCRUB":
                kwargs["disable_scrub"] = True
            elif scheme == "OURS-EPOCH":
                kwargs["disable_epoch"] = True
            elif scheme == "OURS-G1.5":
                kwargs["margin_g"] = 1.5
            elif scheme == "OURS-DET":
                kwargs["disable_detector"] = True

            count = 8 if QUICK else 100
            for index in range(count):
                rows.append(make_spec("e6", "DEC", scheme, attack, SCALE, index, **kwargs))
    return rows


def e7_specs() -> list:
    rows = []
    count = 6 if QUICK else 100
    for attack in ("A1", "A3"):
        for n in (16, 32, 128, 256):
            for index in range(count):
                rows.append(make_spec("e7", "DEC", "OURS", attack, SCALE, index, region_n=n, tag_suffix=f"n{n}"))
        for k in (7, 31):
            for index in range(count):
                rows.append(make_spec("e7", "DEC", "OURS", attack, SCALE, index, mg_k=k, tag_suffix=f"k{k}"))
        for d in (64, 128):
            for index in range(count):
                rows.append(make_spec("e7", "DEC", "OURS", attack, SCALE, index, report_delay=d, tag_suffix=f"d{d}"))
        for index in range(count):
            rows.append(make_spec("e7", "DEC", "OURS", attack, SCALE, index, report_granularity="codeword", tag_suffix="gran_cw"))
        for dist in ("uniform", "shifted_exponential"):
            for index in range(count):
                rows.append(make_spec("e7", "DEC", "OURS", attack, SCALE, index, hc_dist=dist, tag_suffix=f"dist_{dist}"))
        for vrd in (1.0, 2.0, 5.0):
            for index in range(count):
                rows.append(make_spec("e7", "DEC", "OURS", attack, SCALE, index, vrd_max=vrd, tag_suffix=f"vrd_{vrd:.1f}"))
    return rows


def classify_row(raw: dict, path: Path | None = None) -> dict:
    outcome = classify(
        status=raw.get("status", "FAILED"),
        bit_exact=bool(raw.get("bit_exact", False)),
        flip_count=int(raw.get("flips", 0) or 0),
        replayed=bool(raw.get("replayed", False)),
        withheld=bool(raw.get("withheld", False)),
        recovery_failed=bool(raw.get("recovery_failed", False)),
        phase2_status=str(raw.get("phase2_status") or ""),
        latent_flips=int(raw.get("latent_flips", 0) or 0),
    )
    cycles = float(raw.get("cycles") or 0)
    useful_items = raw.get("useful_pim_items")
    if useful_items is not None:
        items = float(useful_items)
    else:
        items = float(raw.get("pim_items") or 0)

    name = None if outcome is None else outcome["outcome"]
    throughput = 0.0
    if name in ("O5", "O6"):
        throughput = 0.0
    elif name not in (None, "") and cycles > 0:
        throughput = items / cycles

    variant = raw.get("variant", "")
    if not variant and path is not None:
        dir_name = path.parent.name
        prefix = f"{raw.get('workload')}_{raw.get('scheme')}_{raw.get('attack')}_"
        suffix = f"_{raw.get('index')}"
        if dir_name.startswith(prefix) and dir_name.endswith(suffix) and len(dir_name) > len(prefix) + len(suffix):
            variant = dir_name[len(prefix):-len(suffix)]

    return {
        "workload": raw.get("workload"),
        "scheme": raw.get("scheme"),
        "attack": raw.get("attack"),
        "scale": raw.get("scale"),
        "index": raw.get("index"),
        "seed": raw.get("seed"),
        "status": raw.get("status"),
        "outcome": name or "",
        "flips": raw.get("flips", 0),
        "latent_flips": raw.get("latent_flips", 0),
        "cycles": raw.get("cycles", 0),
        "pim_items": raw.get("pim_items", 0),
        "useful_pim_items": raw.get("useful_pim_items", items),
        "effective_throughput": throughput,
        "bit_exact": raw.get("bit_exact"),
        "replayed": raw.get("replayed"),
        "withheld": raw.get("withheld"),
        "recovery_failed": raw.get("recovery_failed"),
        "error": raw.get("error", ""),
        "gen_mode": raw.get("gen_mode", "HBM2"),
        "variant": variant,
        "misrecovered": int(raw.get("misrecovered", 0) or 0),
        "para_cycles": int(raw.get("para_cycles", 0) or 0),
    }


def write_csv(path: Path, rows: list, fields: list) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields, extrasaction="ignore")
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def aggregate(rows: list, expected: dict) -> list:
    grouped: dict = {}
    for row in rows:
        if row.get("attack") == "NO_ATK":
            continue
        key = (row["workload"], row["scheme"], row["attack"], float(row["scale"]))
        grouped.setdefault(key, []).append(row)
    summary = []
    for key, samples in sorted(grouped.items()):
        workload, scheme, attack, scale = key
        success = [sample for sample in samples if sample["status"] == "SUCCESS" and sample["outcome"]]
        counts = {name: 0 for name in ("O1", "O2", "O3", "O4", "O5", "O6")}
        for sample in success:
            counts[sample["outcome"]] += 1
        n = len(success)
        rate, low, high = wilson(counts["O6"], n) if n else (None, None, None)
        upper = zero_escape_upper(n) if n and counts["O6"] == 0 else None
        latent = [float(sample["latent_flips"]) for sample in success]
        throughput = [float(sample["effective_throughput"]) for sample in success]
        cycles = [float(sample["cycles"]) for sample in success]
        flips = [float(sample["flips"]) for sample in success]
        misrecovered_count = sum(1 for sample in success if int(sample.get("misrecovered", 0) or 0) > 0)
        device = "HBM3" if any(s.get("gen_mode") == "HBM3_STYLE" for s in success) else "HBM2"
        summary.append({
            "workload": workload, "scheme": scheme, "attack": attack, "scale": scale,
            "device": device, "status": "SUCCESS" if n == expected.get(key, n) else "PARTIAL",
            "n_expected": expected.get(key, n), "n_success": n,
            "n_failed": len(samples) - n,
            "O1": counts["O1"], "O2": counts["O2"], "O3": counts["O3"],
            "O4": counts["O4"], "O5": counts["O5"], "O6": counts["O6"],
            "o6_rate": "" if rate is None else f"{rate:.8f}",
            "wilson_low": "" if low is None else f"{low:.8f}",
            "wilson_high": "" if high is None else f"{high:.8f}",
            "zero_escape_upper": "" if upper is None else f"{upper:.8f}",
            "mean_latent_flips": f"{(sum(latent) / n) if n else 0:.6f}",
            "mean_flips": f"{(sum(flips) / n) if n else 0:.6f}",
            "mean_cycles": f"{(sum(cycles) / n) if n else 0:.1f}",
            "mean_effective_throughput": f"{(sum(throughput) / n) if n else 0:.8e}",
            "misrecovered": misrecovered_count,
        })
    return summary


def aggregate_by_variant(rows: list) -> list:
    grouped: dict = {}
    for row in rows:
        key = (row["variant"], row["workload"], row["scheme"], row["attack"], float(row["scale"]))
        grouped.setdefault(key, []).append(row)
    summary = []
    for key, samples in sorted(grouped.items()):
        variant, workload, scheme, attack, scale = key
        success = [sample for sample in samples if sample["status"] == "SUCCESS" and sample["outcome"]]
        counts = {name: 0 for name in ("O1", "O2", "O3", "O4", "O5", "O6")}
        for sample in success:
            counts[sample["outcome"]] += 1
        n = len(success)
        rate, low, high = wilson(counts["O6"], n) if n else (None, None, None)
        upper = zero_escape_upper(n) if n and counts["O6"] == 0 else None
        latent = [float(sample["latent_flips"]) for sample in success]
        cycles = [float(sample["cycles"]) for sample in success]
        flips = [float(sample["flips"]) for sample in success]
        summary.append({
            "variant": variant, "workload": workload, "scheme": scheme, "attack": attack, "scale": scale,
            "n_success": n, "n_failed": len(samples) - n,
            "O1": counts["O1"], "O2": counts["O2"], "O3": counts["O3"],
            "O4": counts["O4"], "O5": counts["O5"], "O6": counts["O6"],
            "o6_rate": "" if rate is None else f"{rate:.8f}",
            "wilson_low": "" if low is None else f"{low:.8f}",
            "wilson_high": "" if high is None else f"{high:.8f}",
            "zero_escape_upper": "" if upper is None else f"{upper:.8f}",
            "mean_cycles": f"{(sum(cycles) / n) if n else 0:.1f}",
            "mean_flips": f"{(sum(flips) / n) if n else 0:.6f}",
            "mean_latent_flips": f"{(sum(latent) / n) if n else 0:.6f}",
        })
    return summary


def expected_map(kind: str) -> dict:
    found = {}
    if kind == "e5":
        for workload in ("W2", "DEC"):
            for attack in ("A1", "A3", "A4"):
                for scheme in SCHEMES:
                    found[(workload, scheme, attack, SCALE)] = (12 if scheme == "OURS" else 8) if QUICK else (300 if scheme == "OURS" else 100)
    elif kind == "real":
        for attack in ("A1", "A3"):
            for scheme in ("NONE", "REACT", "FIX", "OURS"):
                found[("DEC", scheme, attack, 1.0)] = 2 if QUICK else 10
    elif kind == "hbm3":
        for attack in ("A1", "A3"):
            for scheme, count in (("ADA", 8 if QUICK else 100), ("OURS", 12 if QUICK else 300)):
                found[("DEC", scheme, attack, SCALE)] = count
    elif kind == "e6":
        for attack in ("A1", "A3"):
            for scheme in ABLATIONS:
                found[("DEC", scheme, attack, SCALE)] = 8 if QUICK else 100
    elif kind == "v2":
        for workload in ("W2", "DEC"):
            for attack in ("A1", "A3"):
                for scheme in ("FIX", "ADA", "OURS"):
                    found[(workload, scheme, attack, SCALE)] = 4 if QUICK else 20
    return found


def load_results(kind: str) -> list:
    rows = []
    root = RUNS / kind
    if not root.exists():
        return rows
    for path in root.glob("*/result.json"):
        try:
            rows.append(classify_row(json.loads(path.read_text()), path=path))
        except json.JSONDecodeError:
            continue
    return rows


def check_g4(e5_rows: list, e6_rows: list, e5_summary: list, e6_summary: list) -> dict:
    """Strict One-Sided Worsening + Seed-Paired CRN Gate G4 Evaluator."""
    crit1_pass = True
    crit1_notes = []

    fix_cycles = {}
    for row in e5_summary:
        if row["scheme"] == "FIX":
            fix_cycles[(row["workload"], row["attack"])] = float(row["mean_cycles"])

    for row in e5_summary:
        if row["scheme"] == "OURS":
            o6 = int(row["O6"])
            n = int(row["n_success"])
            if o6 != 0:
                crit1_pass = False
                crit1_notes.append(f"OURS {row['workload']} {row['attack']} has O6 = {o6} != 0")
            key = (row["workload"], row["attack"])
            if key in fix_cycles and float(row["mean_cycles"]) >= fix_cycles[key]:
                crit1_pass = False
                crit1_notes.append(f"OURS {key} mean cycles {row['mean_cycles']} >= FIX cycles {fix_cycles[key]}")

    ours_samples_by_attack = {}
    for r in e5_rows:
        if r["scheme"] == "OURS" and r["workload"] == "DEC" and r["status"] == "SUCCESS":
            ours_samples_by_attack.setdefault(r["attack"], []).append(r)

    grouped_e6 = {}
    for r in e6_rows:
        if r["status"] == "SUCCESS":
            grouped_e6.setdefault((r["scheme"], r["attack"]), []).append(r)

    ablations_eval = {}
    metrics = ["o6_flag", "misrecovered_flag", "replay_or_fail_flag", "flips", "latent_flips", "cycles"]

    for (sch, atk), abl_list in grouped_e6.items():
        ours_list = ours_samples_by_attack.get(atk, [])
        if not ours_list:
            continue

        max_worsening_z = 0.0
        best_metric = ""
        max_paired_z = 0.0
        best_paired_metric = ""

        ours_by_idx = {int(s["index"]): s for s in ours_list}
        abl_by_idx = {int(s["index"]): s for s in abl_list}

        for m in metrics:
            def extract_val(sample, metric_name):
                if metric_name == "o6_flag":
                    return 1.0 if sample["outcome"] == "O6" else 0.0
                elif metric_name == "misrecovered_flag":
                    return 1.0 if int(sample.get("misrecovered", 0) or 0) > 0 else 0.0
                elif metric_name == "replay_or_fail_flag":
                    return 1.0 if sample["outcome"] in ("O3", "O4", "O5", "O6") else 0.0
                else:
                    return float(sample.get(metric_name, 0.0) or 0.0)

            # 1. Unpaired signed worsening z-score
            vals_a = [extract_val(s, m) for s in abl_list]
            vals_b = [extract_val(s, m) for s in ours_list]

            mean_a = sum(vals_a) / len(vals_a) if vals_a else 0.0
            mean_b = sum(vals_b) / len(vals_b) if vals_b else 0.0
            diff = mean_a - mean_b

            if diff <= 0.0:
                z_unpaired = 0.0
            else:
                var_a = sum((x - mean_a)**2 for x in vals_a) / (len(vals_a) - 1) if len(vals_a) > 1 else 0.0
                var_b = sum((x - mean_b)**2 for x in vals_b) / (len(vals_b) - 1) if len(vals_b) > 1 else 0.0
                denom = math.sqrt(var_a / len(vals_a) + var_b / len(vals_b)) if (len(vals_a) > 0 and len(vals_b) > 0) else 0.0
                if denom == 0.0:
                    z_unpaired = 99.0
                else:
                    z_unpaired = diff / denom

            if z_unpaired > max_worsening_z:
                max_worsening_z = z_unpaired
                best_metric = m

            # 2. Seed-paired Common-Random-Numbers (CRN) worsening z-score
            shared = sorted(set(abl_by_idx.keys()) & set(ours_by_idx.keys()))
            if len(shared) > 1:
                d_vals = [extract_val(abl_by_idx[i], m) - extract_val(ours_by_idx[i], m) for i in shared]
                mean_d = sum(d_vals) / len(d_vals)
                if mean_d > 0.0:
                    var_d = sum((x - mean_d)**2 for x in d_vals) / (len(d_vals) - 1)
                    se_d = math.sqrt(var_d / len(d_vals))
                    z_paired = 99.0 if se_d == 0.0 else (mean_d / se_d)
                else:
                    z_paired = 0.0
            else:
                z_paired = 0.0

            if z_paired > max_paired_z:
                max_paired_z = z_paired
                best_paired_metric = m

        ablations_eval.setdefault(sch, {})[atk] = {
            "max_abs_z": max_worsening_z,
            "max_worsening_z": max_worsening_z,
            "best_metric": best_metric,
            "max_paired_z": max_paired_z,
            "best_paired_metric": best_paired_metric
        }

    all_z_pass = True
    scheme_passes = {}
    for sch in ABLATIONS:
        pair_dict = ablations_eval.get(sch, {})
        scheme_max_unpaired = max((v["max_worsening_z"] for v in pair_dict.values()), default=0.0)
        scheme_max_paired = max((v["max_paired_z"] for v in pair_dict.values()), default=0.0)
        passed = scheme_max_unpaired >= 1.96
        scheme_passes[sch] = {
            "max_z": scheme_max_unpaired,
            "max_worsening_z": scheme_max_unpaired,
            "max_paired_z": scheme_max_paired,
            "passed": passed
        }
        if not passed:
            all_z_pass = False

    g4_result = {
        "criterion_1_pass": crit1_pass,
        "criterion_1_notes": crit1_notes,
        "criterion_2_evaluated": True,
        "criterion_2_all_z_pass": all_z_pass,
        "ablations_eval": ablations_eval,
        "scheme_passes": scheme_passes,
        "status": "PASS" if (crit1_pass and all_z_pass) else "FAIL"
    }
    RESULTS.mkdir(parents=True, exist_ok=True)
    (RESULTS / "G4_check.json").write_text(json.dumps(g4_result, indent=2) + "\n")
    return g4_result


def summarize() -> None:
    fields = [
        "workload", "scheme", "attack", "scale", "index", "seed", "status", "outcome",
        "flips", "latent_flips", "cycles", "pim_items", "useful_pim_items", "effective_throughput",
        "bit_exact", "replayed", "withheld", "recovery_failed", "error", "gen_mode", "variant",
        "misrecovered", "para_cycles",
    ]
    e5_rows = load_results("e5")
    real_rows = load_results("real")
    e6_rows = load_results("e6")
    hbm3_rows = load_results("hbm3")
    v2_rows = load_results("v2")
    e7_rows = load_results("e7")
    ref_rows = load_results("ref")
    v2_benign_rows = load_results("v2_benign")

    write_csv(RESULTS / "E5_clean_runs.csv", e5_rows, fields)
    write_csv(RESULTS / "E5_real_threshold_runs.csv", real_rows, fields)
    write_csv(RESULTS / "E6_clean_runs.csv", e6_rows, fields)
    write_csv(RESULTS / "HBM3_runs.csv", hbm3_rows, fields)
    write_csv(RESULTS / "V2_runs.csv", v2_rows, fields)
    write_csv(RESULTS / "E7_runs.csv", e7_rows, fields)
    write_csv(RESULTS / "Ref_runs.csv", ref_rows, fields)
    write_csv(RESULTS / "V2_benign_runs.csv", v2_benign_rows, fields)

    e5_summary = aggregate(e5_rows, expected_map("e5"))
    real_summary = aggregate(real_rows, expected_map("real"))
    e6_summary = aggregate(e6_rows, expected_map("e6"))
    hbm3_summary = aggregate(hbm3_rows, expected_map("hbm3"))
    v2_summary = aggregate(v2_rows, expected_map("v2"))
    e7_summary = aggregate_by_variant(e7_rows)

    write_csv(RESULTS / "E5_clean_summary.csv", e5_summary, SUMMARY_FIELDS)
    write_csv(RESULTS / "E5_real_summary.csv", real_summary, SUMMARY_FIELDS)
    write_csv(RESULTS / "E6_clean_summary.csv", e6_summary, SUMMARY_FIELDS)
    write_csv(RESULTS / "HBM3_summary.csv", hbm3_summary, SUMMARY_FIELDS)
    write_csv(RESULTS / "V2_summary.csv", v2_summary, SUMMARY_FIELDS)

    e7_fields = ["variant", "workload", "scheme", "attack", "scale", "n_success", "n_failed", "O1", "O2", "O3", "O4", "O5", "O6", "o6_rate", "wilson_low", "wilson_high", "zero_escape_upper", "mean_cycles", "mean_flips", "mean_latent_flips"]
    write_csv(RESULTS / "E7_sensitivity.csv", e7_summary, e7_fields)

    e5_cycles_map = {(r["workload"], r["scheme"], r["attack"]): float(r["mean_cycles"]) for r in e5_summary}
    v2_grouped = aggregate(v2_rows, expected_map("v2"))
    delta_rows = []
    for r in v2_grouped:
        key = (r["workload"], r["scheme"], r["attack"])
        base_cycles = e5_cycles_map.get(key, 0.0)
        v2_cycles = float(r["mean_cycles"])
        delta = v2_cycles - base_cycles
        delta_rows.append({
            "workload": r["workload"], "scheme": r["scheme"], "attack": r["attack"],
            "base_cycles": f"{base_cycles:.1f}", "v2_cycles": f"{v2_cycles:.1f}", "delta_cycles": f"{delta:.1f}"
        })
    write_csv(RESULTS / "M4_v2_timing_delta.csv", delta_rows, ["workload", "scheme", "attack", "base_cycles", "v2_cycles", "delta_cycles"])

    # Benign NO_ATK v1 vs v2 overhead (M4_v2_benign_overhead.csv)
    ref_success = [r for r in ref_rows if r["status"] == "SUCCESS" and r.get("attack") == "NO_ATK" and abs(float(r["scale"]) - SCALE) < 1e-6]
    v2_b_success = [r for r in v2_benign_rows if r["status"] == "SUCCESS" and r.get("attack") == "NO_ATK" and abs(float(r["scale"]) - SCALE) < 1e-6]

    ref_map = {}
    for r in ref_success:
        ref_map.setdefault((r["workload"], r["scheme"]), []).append(float(r["cycles"]))
    v2_b_map = {}
    for r in v2_b_success:
        v2_b_map.setdefault((r["workload"], r["scheme"]), []).append(float(r["cycles"]))

    none_ref_cycles = {wl: (sum(ref_map.get((wl, "NONE"), [0])) / len(ref_map.get((wl, "NONE"), [1])) if ref_map.get((wl, "NONE")) else 1.0) for wl in ("W2", "DEC")}
    none_v2_cycles = {wl: (sum(v2_b_map.get((wl, "NONE"), [0])) / len(v2_b_map.get((wl, "NONE"), [1])) if v2_b_map.get((wl, "NONE")) else 1.0) for wl in ("W2", "DEC")}

    overhead_rows = []
    for workload in ("W2", "DEC"):
        none_v1 = none_ref_cycles.get(workload, 1.0)
        none_v2 = none_v2_cycles.get(workload, 1.0)
        for scheme in ("NONE", "PARA", "FIX", "ADA", "OURS"):
            v1_lst = ref_map.get((workload, scheme), [])
            v2_lst = v2_b_map.get((workload, scheme), [])
            v1_mean = (sum(v1_lst) / len(v1_lst)) if v1_lst else 0.0
            v2_mean = (sum(v2_lst) / len(v2_lst)) if v2_lst else 0.0
            v1_ovh = (100.0 * (v1_mean - none_v1) / none_v1) if (none_v1 > 0 and v1_lst) else 0.0
            v2_ovh = (100.0 * (v2_mean - none_v2) / none_v2) if (none_v2 > 0 and v2_lst) else 0.0
            overhead_rows.append({
                "workload": workload, "scheme": scheme, "scale": SCALE,
                "n_v1": len(v1_lst), "n_v2": len(v2_lst),
                "v1_cycles": f"{v1_mean:.1f}", "v2_cycles": f"{v2_mean:.1f}",
                "v1_overhead_pct": f"{v1_ovh:.4f}", "v2_overhead_pct": f"{v2_ovh:.4f}"
            })
    write_csv(RESULTS / "M4_v2_benign_overhead.csv", overhead_rows, ["workload", "scheme", "scale", "n_v1", "n_v2", "v1_cycles", "v2_cycles", "v1_overhead_pct", "v2_overhead_pct"])

    g4 = check_g4(e5_rows, e6_rows, e5_summary, e6_summary)
    print(f"Gate G4 Evaluation Result: {g4['status']}", flush=True)


def main() -> None:
    phase = sys.argv[1] if len(sys.argv) > 1 else "all"
    if phase in ("all", "e5"):
        pool(e5_specs(), "e5")
        pool(reference_specs(), "ref")
    if phase in ("patch", "e5_patch"):
        pool(e5_patch_specs(), "e5")
        pool(reference_specs(), "ref")
    if phase in ("all", "v2_benign", "patch"):
        pool(v2_benign_specs(), "v2_benign")
    if phase in ("all", "real"):
        pool(real_specs(), "real")
    if phase in ("all", "hbm3"):
        pool(hbm3_specs(), "hbm3")
    if phase in ("all", "v2"):
        pool(v2_specs(), "v2")
    if phase in ("patch", "v2_patch"):
        pool(v2_patch_specs(), "v2")
    if phase in ("all", "e6", "patch"):
        pool(e6_specs(), "e6")
    if phase in ("all", "e7"):
        pool(e7_specs(), "e7")
    if phase in ("all", "e5", "real", "hbm3", "v2", "e6", "e7", "v2_benign", "patch", "summarize"):
        summarize()


if __name__ == "__main__":
    main()

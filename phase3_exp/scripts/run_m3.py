#!/usr/bin/env python3
"""M3 closed-loop campaign.

Writes only under results/m3, runs/M3, figures fig6/fig7/fig_e6, and the
playbook deliverable names.  Does not overwrite M1/M2 artifacts, para_p.json,
or a successful cell log.
"""
from __future__ import annotations

import csv
import json
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
DEVICE = ROOT / "pim-phase1/simulator/ini/HBM2_samsung_2M_16B_x64.ini"
SYSTEM = EXP / "configs/system_1ch_quiet.ini"
RESULTS = EXP / "results/m3"
RUNS = EXP / "runs/M3"
PYTHON = "/opt/data/data/anaconda3/bin/python3"
SIM_BASE = 20261122
PARA_P = 0.01171875
SCALE = 1.0 / 16.0
SCHEMES = ["NONE", "REACT", "PARA", "FIX", "ADA", "OURS"]
ABLATIONS = ["OURS-CE", "OURS-SCRUB", "OURS-EPOCH", "OURS-G1.5", "OURS-DET"]
WORKERS = int(os.environ.get("M3_WORKERS", "32"))


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
                return existing
        except json.JSONDecodeError:
            pass
    out.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env.update({
        "LD_LIBRARY_PATH": "/opt/data/data/anaconda3/lib",
        "PHASE3_M3_RUN": "cell",
        "PHASE3_DEVICE_INI": str(DEVICE),
        "PHASE3_SYSTEM_INI": str(SYSTEM),
        "PHASE3_OUT": str(out),
        "PHASE3_WORKLOAD": spec["workload"],
        "PHASE3_SCHEME": spec["scheme"],
        "PHASE3_ATTACK": spec["attack"],
        "PHASE3_SCALE": f"{spec['scale']:.8f}",
        "PHASE3_INDEX": str(spec["index"]),
        "PHASE3_PARA_P": f"{spec['para_p']:.8f}",
        "PHASE3_SEED": str(spec["seed"]),
        "PHASE3_BOUNDARY_VICTIM": "1",
    })
    try:
        with (out / "gtest.log").open("w") as handle:
            completed = subprocess.run(
                [str(SIM), "--gtest_filter=Phase3M3.Cell"],
                cwd=str(out), env=env, stdout=handle, stderr=subprocess.STDOUT,
                timeout=3600,
            )
    except subprocess.TimeoutExpired:
        return {"status": "FAILED", "error": "timeout", "out": str(out), **spec}
    if not result_path.exists():
        return {"status": "FAILED", "error": f"exit {completed.returncode}", "out": str(out), **spec}
    payload = json.loads(result_path.read_text())
    payload["out"] = str(out)
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


def spec(kind: str, workload: str, scheme: str, attack: str, scale: float, index: int) -> dict:
    grid = f"{workload}|{scheme}|{attack}|{scale:.6f}"
    para = PARA_P if scheme == "PARA" else 0.0
    tag = "s1" if scale > 0.5 else "s16"
    return {
        "kind": kind, "workload": workload, "scheme": scheme, "attack": attack,
        "scale": scale, "index": index, "para_p": para,
        "seed": grid_seed(grid, index),
        "out": str(RUNS / kind / f"{workload}_{scheme}_{attack}_{tag}_{index}"),
    }


def e5_specs() -> list:
    rows = []
    for workload in ("W2", "DEC"):
        for attack in ("A1", "A3", "A4"):
            for scheme in SCHEMES:
                count = 300 if scheme == "OURS" else 100
                for index in range(count):
                    rows.append(spec("e5c", workload, scheme, attack, SCALE, index))
    return rows


def real_specs() -> list:
    rows = []
    for attack in ("A1", "A3"):
        for scheme in ("NONE", "REACT", "FIX", "OURS"):
            for index in range(10):
                rows.append(spec("e5realc", "DEC", scheme, attack, 1.0, index))
    return rows


def e6_specs() -> list:
    rows = []
    for attack in ("A1", "A3"):
        for scheme in ABLATIONS:
            for index in range(100):
                rows.append(spec("e6c", "DEC", scheme, attack, SCALE, index))
    return rows


def reference_specs() -> list:
    rows = []
    for workload in ("W2", "DEC"):
        for index in range(4):
            rows.append(spec("refc", workload, "NONE", "NO_ATK", SCALE, index))
        rows.append(spec("refc", workload, "NONE", "NO_ATK", 1.0, 0))
    return rows


def hbm3_rows() -> list:
    rows = []
    for attack in ("A1", "A3"):
        for scheme, count in (("ADA", 100), ("OURS", 300)):
            rows.append({
                "workload": "DEC", "scheme": scheme, "attack": attack,
                "scale": SCALE, "device": "HBM3", "status": "MISSING",
                "n_expected": count, "n_success": 0, "reason": "no HBM3 ini",
            })
    return rows


def classify_row(raw: dict) -> dict:
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
    items = float(raw.get("pim_items") or 0)
    name = None if outcome is None else outcome["outcome"]
    throughput = 0.0
    if name not in (None, "O5", "O6") and cycles > 0:
        throughput = items / cycles
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
        "effective_throughput": throughput,
        "bit_exact": raw.get("bit_exact"),
        "replayed": raw.get("replayed"),
        "withheld": raw.get("withheld"),
        "recovery_failed": raw.get("recovery_failed"),
        "error": raw.get("error", ""),
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
        if n and sum(counts.values()) != n:
            raise SystemExit(f"outcome sum mismatch for {key}")
        rate, low, high = wilson(counts["O6"], n) if n else (None, None, None)
        upper = zero_escape_upper(n) if n and counts["O6"] == 0 else None
        latent = [float(sample["latent_flips"]) for sample in success]
        throughput = [float(sample["effective_throughput"]) for sample in success]
        cycles = [float(sample["cycles"]) for sample in success]
        flips = [float(sample["flips"]) for sample in success]
        summary.append({
            "workload": workload, "scheme": scheme, "attack": attack, "scale": scale,
            "device": "HBM2", "status": "SUCCESS" if n == expected.get(key, n) else "PARTIAL",
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
        })
    return summary


def expected_map(kind: str) -> dict:
    found = {}
    if kind == "e5c":
        for workload in ("W2", "DEC"):
            for attack in ("A1", "A3", "A4"):
                for scheme in SCHEMES:
                    found[(workload, scheme, attack, SCALE)] = 300 if scheme == "OURS" else 100
    elif kind == "e5realc":
        for attack in ("A1", "A3"):
            for scheme in ("NONE", "REACT", "FIX", "OURS"):
                found[("DEC", scheme, attack, 1.0)] = 10
    elif kind == "e6c":
        for attack in ("A1", "A3"):
            for scheme in ABLATIONS:
                found[("DEC", scheme, attack, SCALE)] = 100
    return found


def load_results(kind: str) -> list:
    rows = []
    root = RUNS / kind
    if not root.exists():
        return rows
    for path in root.glob("*/result.json"):
        try:
            rows.append(classify_row(json.loads(path.read_text())))
        except json.JSONDecodeError:
            continue
    return rows


def reference_throughput() -> dict:
    found = {}
    grouped: dict = {}
    for row in load_results("refc"):
        if row["status"] != "SUCCESS":
            continue
        key = (row["workload"], float(row["scale"]))
        grouped.setdefault(key, []).append(float(row["effective_throughput"]))
    for key, values in grouped.items():
        found[key] = sum(values) / len(values) if values else 0.0
    return found


def main() -> None:
    RESULTS.mkdir(parents=True, exist_ok=True)
    (RESULTS / "campaign_config.json").write_text(json.dumps({
        "para_p_e5": PARA_P,
        "para_p_json_unchanged": 0.0625,
        "scale_main": SCALE,
        "refresh": "on",
        "host_attack_fraction": 0.5,
        "background_fraction": 0.2,
        "a3": "every step activates the neighbor outside the victim region; a 16-row scatter stays in that neighbor region and does not train the victim swap",
        "boundary_victim": "W2 row 768, DEC row 2432",
        "attack": "outside-region neighbor chase, two waves; swap bytes move in exchangeRows and each pair adds 4*tRC cycles",
        "hbm3": "MISSING",
    }, indent=2) + "\n")
    phase = sys.argv[1] if len(sys.argv) > 1 else "all"
    if phase in ("all", "e5"):
        pool(e5_specs(), "e5")
        pool(reference_specs(), "ref")
    if phase in ("all", "real"):
        pool(real_specs(), "real")
    if phase in ("all", "e6"):
        pool(e6_specs(), "e6")
    if phase in ("all", "e5", "real", "e6", "summarize"):
        summarize()


def summarize() -> None:
    fields = [
        "workload", "scheme", "attack", "scale", "index", "seed", "status", "outcome",
        "flips", "latent_flips", "cycles", "pim_items", "effective_throughput",
        "bit_exact", "replayed", "withheld", "recovery_failed", "error",
    ]
    e5_rows = load_results("e5c")
    real_rows = load_results("e5realc")
    e6_rows = load_results("e6c")
    write_csv(RESULTS / "E5_scale_runs.csv", e5_rows, fields)
    write_csv(RESULTS / "E5_real_threshold_runs.csv", real_rows, fields)
    write_csv(RESULTS / "E6_runs.csv", e6_rows, fields)
    write_csv(EXP / "results" / "E5_real_threshold_runs.csv", real_rows, fields)
    e5_summary = aggregate(e5_rows, expected_map("e5c"))
    real_summary = aggregate(real_rows, expected_map("e5realc"))
    e6_summary = aggregate(e6_rows, expected_map("e6c"))
    summary_fields = list(e5_summary[0].keys()) if e5_summary else []
    write_csv(RESULTS / "E5_campaign_summary.csv", e5_summary, summary_fields)
    write_csv(EXP / "results" / "E5_campaign_summary.csv", e5_summary, summary_fields)
    write_csv(RESULTS / "E5_real_summary.csv", real_summary, summary_fields)
    write_csv(RESULTS / "E6_ablation.csv", e6_summary, summary_fields)
    write_csv(EXP / "results" / "E6_ablation.csv", e6_summary, summary_fields)
    write_csv(RESULTS / "E5_hbm3_missing.csv", hbm3_rows(),
              ["workload", "scheme", "attack", "scale", "device", "status", "n_expected", "n_success", "reason"])
    refs = reference_throughput()
    (RESULTS / "reference_throughput.json").write_text(json.dumps(
        {f"{workload}|{scale}": value for (workload, scale), value in refs.items()}, indent=2) + "\n")
    for block, name in ((e5_summary, "e5"), (real_summary, "real"), (e6_summary, "e6")):
        for row in block:
            if int(row["n_failed"]) or int(row["n_success"]) != int(row["n_expected"]):
                print(f"INCOMPLETE {name} {row['workload']} {row['scheme']} {row['attack']} "
                      f"{row['n_success']}/{row['n_expected']}", flush=True)
    print(f"summarized e5={len(e5_summary)} real={len(real_summary)} e6={len(e6_summary)}", flush=True)


if __name__ == "__main__":
    main()

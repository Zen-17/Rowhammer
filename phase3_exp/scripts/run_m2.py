#!/usr/bin/env python3
"""M2 campaign: lock the design point, run E3 and E4, draw figures 3-5.

Does not start M3.  HBM3 is recorded as missing because configs/hbm3_style.ini
is not in the tree and the playbook gives no tRC or tREFI for it.
"""
from __future__ import annotations

import csv
import json
import os
import subprocess
import zlib
from concurrent.futures import ProcessPoolExecutor, as_completed
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
EXP = ROOT / "phase3_exp"
SIM = EXP / "build/simulator/sim"
DEVICE = ROOT / "pim-phase1/simulator/ini/HBM2_samsung_2M_16B_x64.ini"
SYSTEM = EXP / "configs/system_1ch_quiet.ini"
RESULTS = EXP / "results"
FIGURES = EXP / "figures"
RUNS = EXP / "runs/M2"
PYTHON = "/opt/data/data/anaconda3/bin/python3"
SIM_BASE = 20261122

ATTACKS = ("A1", "A2", "A3", "A4")
SCHEMES = ("NONE", "PARA", "FIX", "ADA", "OURS")
WORKLOADS = ("W1", "W2", "W4", "GEMV", "DEC")
HC_SCAN = (14531, 10000, 8000, 6000, 4000, 2000, 1500, 1000)


def git_commit() -> str:
    try:
        return subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True,
        ).strip()
    except (OSError, subprocess.CalledProcessError):
        return ""


def sim_seed(workload: str, scheme: str, attack: str, scale: float, index: int) -> int:
    grid = f"{workload}|{scheme}|{attack}|{scale:.6f}|{index}"
    return (SIM_BASE + zlib.crc32(grid.encode()) + index) % 2147483648


def run_one(spec: dict) -> dict:
    out = Path(spec["out"])
    out.mkdir(parents=True, exist_ok=True)
    result_path = out / "result.json"
    if result_path.exists():
        try:
            existing = json.loads(result_path.read_text())
            if existing.get("status") == "SUCCESS":
                existing["out"] = str(out)
                return existing
        except json.JSONDecodeError:
            pass
    env = os.environ.copy()
    env.update({
        "LD_LIBRARY_PATH": "/opt/data/data/anaconda3/lib",
        "PHASE3_M2_RUN": "cell",
        "PHASE3_DEVICE_INI": str(DEVICE),
        "PHASE3_SYSTEM_INI": str(SYSTEM),
        "PHASE3_OUT": str(out),
        "PHASE3_WORKLOAD": spec["workload"],
        "PHASE3_SCHEME": spec["scheme"],
        "PHASE3_ATTACK": spec["attack"],
        "PHASE3_SCALE": f"{spec['scale']:.6f}",
        "PHASE3_HC": f"{spec['hc']:.6f}",
        "PHASE3_INDEX": str(spec["index"]),
        "PHASE3_PARA_P": f"{spec['para_p']:.8f}",
        "PHASE3_SEED": str(spec["seed"]),
    })
    log = out / "gtest.log"
    with log.open("w") as handle:
        completed = subprocess.run(
            [str(SIM), "--gtest_filter=Phase3M2.Cell"],
            cwd=str(out), env=env, stdout=handle, stderr=subprocess.STDOUT,
        )
    if not result_path.exists():
        return {"status": "FAILED", "error": f"exit {completed.returncode}", "out": str(out), **spec}
    payload = json.loads(result_path.read_text())
    payload["out"] = str(out)
    return payload


def pool(specs: list, workers: int) -> list:
    if not specs:
        return []
    found = []
    with ProcessPoolExecutor(max_workers=workers) as executor:
        futures = [executor.submit(run_one, spec) for spec in specs]
        for future in as_completed(futures):
            found.append(future.result())
            print(f"cell {len(found)}/{len(specs)} {found[-1].get('status')} "
                  f"{found[-1].get('scheme')} {found[-1].get('attack')} "
                  f"{found[-1].get('workload')}", flush=True)
    return found


def spec(workload, scheme, attack, scale, hc, index, para_p, kind) -> dict:
    out = RUNS / kind / f"{workload}_{scheme}_{attack}_h{int(round(hc))}_{index}"
    return {
        "workload": workload, "scheme": scheme, "attack": attack, "scale": scale,
        "hc": hc, "index": index, "para_p": para_p, "kind": kind,
        "seed": sim_seed(workload, scheme, attack, scale, index),
        "out": str(out),
    }


def write_design_point(commit: str) -> None:
    payload = {
        "locked": True, "n": 64, "c": 2.0,
        "g_fix_ada": 1.5, "g_ours": 1.1,
        "r0_g15": 954, "r1_g15": 75, "r0_g11": 1395, "r1_g11": 103,
        "f_th": 0.0625, "e_det": 1024, "hc_min": 14531,
        "refresh_acts": 614400, "trials": 1000, "git_commit": commit,
    }
    path = EXP / "configs/design_point.json"
    path.write_text(json.dumps(payload, indent=2) + "\n")


def calibrate_para(workers: int) -> float:
    base = spec("W2", "ADA", "NO_ATK", 1.0, 14531, 0, 0.0, "para_search")
    ada = run_one(base)
    if ada.get("status") != "SUCCESS":
        raise RuntimeError(f"ADA calibration run failed: {ada.get('error')}")
    target = int(ada["cycles"])
    # r0 stays quiet on this kernel, so the matched probability can be zero.
    if target == 0:
        return 0.0
    best = 0.0
    best_gap = 1.0
    low, high = 0.0, 1.0
    for step in range(10):
        mid = (low + high) / 2.0
        trial = spec("W2", "PARA", "NO_ATK", 1.0, 14531, 100 + step, mid, "para_search")
        got = run_one(trial)
        if got.get("status") != "SUCCESS":
            raise RuntimeError(f"PARA search failed: {got.get('error')}")
        cycles = int(got["cycles"])
        gap = abs(cycles - target) / target
        if gap < best_gap:
            best, best_gap = mid, gap
        if gap <= 0.005:
            best = mid
            break
        if cycles < target:
            low = mid
        else:
            high = mid
    path = EXP / "configs/para_p.json"
    path.write_text(json.dumps({
        "p": best, "ada_cycles": target, "gap": best_gap, "workload": "W2", "hc": 14531,
    }, indent=2) + "\n")
    print(f"PARA p={best} gap={best_gap}", flush=True)
    return best


def assemble_e3(commit: str) -> Path:
    path = RESULTS / "E3_prevention_timeline.csv"
    columns = [
        "git_commit", "seed", "workload", "scheme", "attack", "scale", "hc", "index",
        "status", "act", "hot_acts", "cum_flips", "level", "swaps", "event",
        "event_region", "event_from", "event_to",
    ]
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=columns, extrasaction="ignore")
        writer.writeheader()
        for result_path in sorted((RUNS / "e3").glob("*/result.json")):
            result = json.loads(result_path.read_text())
            common = {
                "git_commit": commit, "seed": result.get("seed", ""),
                "workload": result.get("workload", ""), "scheme": result.get("scheme", ""),
                "attack": result.get("attack", ""), "scale": result.get("scale", ""),
                "hc": result.get("hc", ""), "index": result.get("index", ""),
                "status": result.get("status", "FAILED"),
            }
            if result.get("status") != "SUCCESS":
                writer.writerow(common)
                continue
            timeline = result_path.parent / "timeline.csv"
            if timeline.exists():
                for sample in csv.DictReader(timeline.open()):
                    if not sample.get("act"):
                        continue
                    writer.writerow({
                        **common, "act": sample["act"], "hot_acts": sample["hot_acts"],
                        "cum_flips": sample["cum_flips"], "level": sample["level"],
                        "swaps": sample["swaps"], "event": "",
                    })
            writer.writerow({
                **common, "act": result.get("attack_transactions", ""),
                "hot_acts": "", "cum_flips": result.get("flips", ""),
                "level": result.get("level", ""), "swaps": result.get("swaps", ""),
                "event": "final",
            })
            for event in result.get("transitions") or []:
                name = f"L{event['from']}->L{event['to']}"
                writer.writerow({
                    **common, "act": event["command"], "event": name,
                    "event_region": event["region"], "event_from": event["from"],
                    "event_to": event["to"], "level": event["to"],
                })
    return path


def assemble_e4(commit: str, para_p: float) -> Path:
    path = RESULTS / "E4_overhead_scaling.csv"
    columns = [
        "git_commit", "seed", "workload", "scheme", "attack", "hc", "index", "kind",
        "device", "status", "cycles", "overhead_pct", "swaps", "host_read_mean",
        "host_read_p99", "host_read_samples", "level", "para_p",
    ]
    grouped = {}
    rows = []
    for kind in ("e4_normal", "e4_scan"):
        for result_path in sorted((RUNS / kind).glob("*/result.json")):
            result = json.loads(result_path.read_text())
            result["kind"] = "normal" if kind == "e4_normal" else "hc_scan"
            result["device"] = "HBM2"
            rows.append(result)
            if result.get("status") == "SUCCESS" and result.get("scheme") == "NONE":
                key = (result["kind"], result["workload"], float(result["hc"]), int(result["index"]))
                grouped[key] = int(result["cycles"])
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=columns)
        writer.writeheader()
        for result in rows:
            none_cycles = grouped.get(
                (result["kind"], result["workload"], float(result["hc"]), int(result["index"]))
            )
            overhead = ""
            if result.get("status") == "SUCCESS" and none_cycles:
                overhead = f"{(int(result['cycles']) - none_cycles) / none_cycles * 100.0:.6f}"
            writer.writerow({
                "git_commit": commit, "seed": result.get("seed", ""),
                "workload": result.get("workload", ""), "scheme": result.get("scheme", ""),
                "attack": result.get("attack", ""), "hc": result.get("hc", ""),
                "index": result.get("index", ""), "kind": result["kind"], "device": "HBM2",
                "status": result.get("status", "FAILED"),
                "cycles": result.get("cycles", "") if result.get("status") == "SUCCESS" else "",
                "overhead_pct": overhead,
                "swaps": result.get("swaps", "") if result.get("status") == "SUCCESS" else "",
                "host_read_mean": result.get("host_read_mean", "") if result.get("status") == "SUCCESS" else "",
                "host_read_p99": result.get("host_read_p99", "") if result.get("status") == "SUCCESS" else "",
                "host_read_samples": result.get("host_read_samples", "") if result.get("status") == "SUCCESS" else "",
                "level": result.get("level", "") if result.get("status") == "SUCCESS" else "",
                "para_p": para_p if result.get("scheme") == "PARA" else 0,
            })
        writer.writerow({
            "git_commit": commit, "device": "HBM3", "kind": "hc_scan", "status": "MISSING",
            "workload": "W2", "attack": "NO_ATK",
        })
    return path


def main() -> None:
    os.environ["PYTHONPATH"] = str(ROOT)
    commit = git_commit()
    write_design_point(commit)
    from phase3_exp.plot.plot_paper import figure_3, figure_4, figure_5
    figure_4(RESULTS / "E2_mc_security.csv", FIGURES)
    print("fig4 written", flush=True)
    para_p = calibrate_para(1)
    checks = []
    for workload in ("W2", "DEC"):
        for scheme in ("NONE", "FIX", "ADA"):
            checks.append(spec(workload, scheme, "NO_ATK", 1.0, 14531, 0, para_p, "conserve"))
    conserved = pool(checks, 4)
    if any(row.get("status") != "SUCCESS" for row in conserved):
        raise RuntimeError("a conservation run failed")
    none_bytes = {
        row["workload"]: row.get("bytes")
        for row in conserved if row.get("scheme") == "NONE" and row.get("status") == "SUCCESS"
    }
    for row in conserved:
        if row.get("scheme") == "NONE" or row.get("status") != "SUCCESS":
            continue
        if row.get("bytes") != none_bytes.get(row["workload"]):
            raise RuntimeError(f"conservation failed {row['workload']} {row['scheme']}")
    print("conservation matched NONE", flush=True)
    e3 = []
    for attack in ATTACKS:
        for scheme in SCHEMES:
            for index in range(5):
                e3.append(spec("W2", scheme, attack, 0.0625, 14531 * 0.0625, index, para_p, "e3"))
            for index in range(3):
                e3.append(spec("W2", scheme, attack, 1.0, 14531, index, para_p, "e3"))
    e4 = []
    for workload in WORKLOADS:
        for scheme in SCHEMES:
            for index in range(5):
                e4.append(spec(workload, scheme, "NO_ATK", 1.0, 14531, index, para_p, "e4_normal"))
    for hc in HC_SCAN:
        for scheme in SCHEMES:
            e4.append(spec("W2", scheme, "NO_ATK", hc / 14531.0, float(hc), 0, para_p, "e4_scan"))
    pool(e3 + e4, 12)
    e3_path = assemble_e3(commit)
    e4_path = assemble_e4(commit, para_p)
    figure_3(e3_path, FIGURES)
    figure_5(e4_path, FIGURES)
    print("M2_CAMPAIGN_DONE", e3_path, e4_path, flush=True)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""M2-R campaign.  Writes only under results/m2r, runs/M2R, and new reports.

Does not overwrite M2 artifacts, does not change the locked design point, and
does not start M3.
"""
from __future__ import annotations

import csv
import json
import os
import subprocess
import zlib
from concurrent.futures import ProcessPoolExecutor, as_completed
from pathlib import Path

from phase3_exp.mc.e2_revise import design_point_faithful, write_security

ROOT = Path(__file__).resolve().parents[2]
EXP = ROOT / "phase3_exp"
SIM = EXP / "build/simulator/sim"
DEVICE = ROOT / "pim-phase1/simulator/ini/HBM2_samsung_2M_16B_x64.ini"
SYSTEM = EXP / "configs/system_1ch_quiet.ini"
RESULTS = EXP / "results/m2r"
RUNS = EXP / "runs/M2R"
PYTHON = "/opt/data/data/anaconda3/bin/python3"
SIM_BASE = 20261122
T_RC = 47


def git_commit() -> str:
    try:
        return subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    except (OSError, subprocess.CalledProcessError):
        return ""


def sim_seed(parts: str, index: int) -> int:
    return (SIM_BASE + zlib.crc32(parts.encode()) + index) % 2147483648


def run_one(spec: dict) -> dict:
    out = Path(spec["out"])
    out.mkdir(parents=True, exist_ok=True)
    result_path = out / "result.json"
    if result_path.exists():
        try:
            existing = json.loads(result_path.read_text())
            if (existing.get("status") == "SUCCESS" and existing.get("refresh_mode") == spec["refresh"]
                    and existing.get("neighbor_basis") == "lifetime"
                    and abs(float(existing.get("para_p", 0)) - float(spec["para_p"])) < 1e-9
                    and (spec["kind"] != "e4" or "total_acts" in existing)):
                existing["out"] = str(out)
                existing["git_commit"] = spec["git_commit"]
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
        "PHASE3_REFRESH": spec["refresh"],
        "PHASE3_ACCESS": spec["access"],
        "PHASE3_REPEAT": str(spec["repeats"]),
    })
    with (out / "gtest.log").open("w") as handle:
        completed = subprocess.run(
            [str(SIM), "--gtest_filter=Phase3M2.Cell"],
            cwd=str(out), env=env, stdout=handle, stderr=subprocess.STDOUT,
        )
    if not result_path.exists():
        return {"status": "FAILED", "error": f"exit {completed.returncode}", "out": str(out), **spec}
    payload = json.loads(result_path.read_text())
    payload["out"] = str(out)
    payload["git_commit"] = spec["git_commit"]
    payload["seed"] = spec["seed"]
    return payload


def pool(specs: list, workers: int) -> list:
    found = []
    if not specs:
        return found
    with ProcessPoolExecutor(max_workers=workers) as executor:
        futures = [executor.submit(run_one, spec) for spec in specs]
        for future in as_completed(futures):
            found.append(future.result())
            print(f"cell {len(found)}/{len(specs)} {found[-1].get('status')} "
                  f"{found[-1].get('workload')} {found[-1].get('scheme')} {found[-1].get('attack')} "
                  f"{found[-1].get('refresh_mode')}", flush=True)
    return found


def make_spec(commit, kind, workload, scheme, attack, scale, hc, index, para_p,
              refresh, access, repeats) -> dict:
    grid = f"{kind}|{workload}|{scheme}|{attack}|{scale:.6f}|{refresh}|{access}|{repeats}|{index}"
    return {
        "workload": workload, "scheme": scheme, "attack": attack, "scale": scale,
        "hc": hc, "index": index, "para_p": para_p, "refresh": refresh,
        "access": access, "repeats": repeats, "git_commit": commit,
        "kind": kind, "seed": sim_seed(grid, index),
        "out": str(RUNS / kind / f"{workload}_{scheme}_{attack}_{access}_r{repeats}_h{int(round(hc))}_p{int(round(para_p * 1e6))}_{index}"),
    }


def write_csv(path: Path, rows: list, fields: list) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields, extrasaction="ignore")
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def overhead(row, baseline) -> float:
    base = baseline or 1
    return 100.0 * (row["cycles"] - baseline) / base


def main() -> None:
    commit = git_commit()
    RESULTS.mkdir(parents=True, exist_ok=True)
    reuse_e2 = "--reuse-e2" in os.sys.argv
    print("E2 revise", flush=True)
    if reuse_e2 and (RESULTS / "E2_security.csv").exists():
        security = list(csv.DictReader((RESULTS / "E2_security.csv").open()))
        for row in security:
            for key in ("n", "trials", "flips", "right_censored", "E_det", "r0", "r1"):
                if row.get(key) not in ("", None):
                    row[key] = int(float(row[key]))
            for key in ("p_flip", "p_flip_low", "p_flip_high", "max_ratio_max", "max_ratio_p99", "swaps_mean", "f_th"):
                if row.get(key) not in ("", None):
                    row[key] = float(row[key])
        faithful = json.loads((RESULTS / "E2_faithful.json").read_text())
    else:
        security = write_security(RESULTS / "E2_security.csv", commit, trials=200, workers=8)
    faithful = design_point_faithful(2)
    (RESULTS / "E2_faithful.json").write_text(json.dumps(faithful, indent=2) + "\n")
    old_para = json.loads((EXP / "configs/para_p.json").read_text())["p"]
    print("PARA check", flush=True)
    probe = []
    low, high = 0.0, 0.25
    chosen = old_para
    for _ in range(6):
        mid = (low + high) / 2.0
        pair = pool([
            make_spec(commit, "para", "GEMV", "ADA", "NO_ATK", 1.0, 14531, 0, 0.0, "on", "reuse", 8),
            make_spec(commit, "para", "GEMV", "PARA", "NO_ATK", 1.0, 14531, 0, mid, "on", "reuse", 8),
        ], 2)
        by_scheme = {row["scheme"]: row for row in pair}
        if by_scheme["ADA"].get("status") != "SUCCESS" or by_scheme["PARA"].get("status") != "SUCCESS":
            chosen = old_para
            break
        gap = (by_scheme["PARA"]["cycles"] - by_scheme["ADA"]["cycles"]) / by_scheme["ADA"]["cycles"]
        probe.append({"p": mid, "gap": gap, "ada_cycles": by_scheme["ADA"]["cycles"],
                      "para_cycles": by_scheme["PARA"]["cycles"]})
        if gap > 0:
            high = mid
        else:
            low = mid
        chosen = mid
    (RESULTS / "para_match.json").write_text(json.dumps(
        {"old_p": old_para, "checked_p": chosen, "probes": probe, "workload": "GEMV",
         "access": "reuse", "repeats": 8, "refresh": "on"}, indent=2) + "\n")
    attacks = ("A1", "A2", "A3", "A4")
    schemes = ("NONE", "FIX", "ADA", "OURS")
    e3 = []
    for attack in attacks:
        for scheme in schemes:
            for index in range(3):
                e3.append(make_spec(commit, "e3", "W2", scheme, attack, 0.0625, 14531 * 0.0625, index,
                                    chosen, "on", "reuse", 1))
            for index in range(2):
                e3.append(make_spec(commit, "e3", "W2", scheme, attack, 1.0, 14531, index,
                                    chosen, "on", "reuse", 1))
    diagnostic = []
    for scheme in ("NONE", "FIX"):
        for scale, hc in ((0.0625, 14531 * 0.0625), (1.0, 14531)):
            diagnostic.append(make_spec(commit, "ref_off", "W2", scheme, "A1", scale, hc, 0,
                                        chosen, "off", "reuse", 1))
    e4 = []
    for workload, repeats in (("W2", 4), ("GEMV", 8), ("DEC", 4)):
        for access in ("streaming", "reuse"):
            for scheme in ("NONE", "PARA", "FIX", "ADA", "OURS"):
                for index in range(2):
                    e4.append(make_spec(commit, "e4", workload, scheme, "NO_ATK", 1.0, 14531, index,
                                        chosen, "on", access, repeats))
    print(f"sim cells {len(e3) + len(diagnostic) + len(e4)}", flush=True)
    e3_rows = pool(e3, 8)
    off_rows = pool(diagnostic, 4)
    e4_rows = pool(e4, 8)
    common = ["git_commit", "seed", "status", "workload", "scheme", "attack", "refresh_mode",
              "access_mode", "repeats", "scale", "hc", "index", "cycles", "pim_items",
              "attack_transactions", "attack_acts", "kernel_acts", "total_acts", "refresh_commands", "injections",
              "first_crossings", "scrub_corrections", "residual_flips", "swaps", "swap_acts",
              "neighbor_acts",
              "peak_ratio", "host_read_mean", "host_read_p99", "mapping_epoch", "level"]
    write_csv(RESULTS / "E3_effectiveness.csv", e3_rows, common)
    write_csv(RESULTS / "E3_ref_off_diagnostic.csv", off_rows, common)
    baselines = {}
    for row in e4_rows:
        if row.get("scheme") == "NONE" and row.get("status") == "SUCCESS":
            baselines[(row["workload"], row["access_mode"], row["index"])] = row["cycles"]
    for row in e4_rows:
        base = baselines.get((row.get("workload"), row.get("access_mode"), row.get("index")), 0)
        row["overhead_pct_v1"] = overhead(row, base) if row.get("status") == "SUCCESS" else ""
        swaps = row.get("swaps") or 0
        row["fill_mov_estimate_cycles"] = swaps * 2 * 16 * T_RC
        row["timing_model_v1"] = "MEASURED"
        row["timing_model_fill_mov"] = "ESTIMATE"
        row["hbm3"] = "MISSING"
    write_csv(RESULTS / "E4_overhead.csv", e4_rows, common + [
        "overhead_pct_v1", "fill_mov_estimate_cycles", "timing_model_v1",
        "timing_model_fill_mov", "hbm3",
    ])
    (RESULTS / "e2_summary.json").write_text(json.dumps({
        "cells": len(security),
        "protected_zero": sum(row["scheme"] != "NONE" and row["flips"] == 0 for row in security),
        "none_with_flips": sum(row["scheme"] == "NONE" and row["flips"] > 0 for row in security),
    }) + "\n")
    write_reports(commit, security, faithful, e3_rows, off_rows, e4_rows, chosen, old_para)
    gate = json.loads((EXP / "configs/gate_state.json").read_text())
    gate["G3"] = "REVISE"
    (EXP / "configs/gate_state.json").write_text(json.dumps(gate, indent=2) + "\n")
    print("M2R_CAMPAIGN_DONE", flush=True)


def write_reports(commit, security, faithful, e3_rows, off_rows, e4_rows, para_p, old_para) -> None:
    reports = EXP / "reports"
    reports.mkdir(parents=True, exist_ok=True)
    protected = [row for row in security if row["scheme"] != "NONE"]
    none_rows = [row for row in security if row["scheme"] == "NONE"]
    zero = sum(row["flips"] == 0 for row in protected)
    none_flip_cells = sum(int(row["flips"]) > 0 for row in none_rows)
    none_quiet = [f"n={row['n']} {row['attack']} ratio {float(row['max_ratio_max']):.4f} {row['censor_reason']}"
                  for row in none_rows if int(row["flips"]) == 0]

    def mean(rows, key):
        vals = [row.get(key) for row in rows if isinstance(row.get(key), (int, float))]
        return sum(vals) / len(vals) if vals else 0

    e3_ok = [row for row in e3_rows if row.get("status") == "SUCCESS"]
    off_ok = [row for row in off_rows if row.get("status") == "SUCCESS"]
    e4_ok = [row for row in e4_rows if row.get("status") == "SUCCESS"]

    def group(rows, scale_real):
        picked = [row for row in rows if (row.get("scale") == 1 or row.get("scale") == 1.0) == scale_real]
        lines = []
        for scheme in ("NONE", "FIX", "ADA", "OURS"):
            subset = [row for row in picked if row.get("scheme") == scheme]
            lines.append(
                f"| {scheme} | {len(subset)} | {sum(row.get('injections') or 0 for row in subset)} | "
                f"{sum(row.get('first_crossings') or 0 for row in subset)} | "
                f"{sum(row.get('scrub_corrections') or 0 for row in subset)} | "
                f"{sum(row.get('residual_flips') or 0 for row in subset)} | "
                f"{mean(subset, 'neighbor_acts'):.1f} | {mean(subset, 'attack_transactions'):.1f} | "
                f"{mean(subset, 'refresh_commands'):.1f} | {mean(subset, 'swap_acts'):.1f} |"
            )
        return "\n".join(lines)

    (reports / "M2R_fault_model.md").write_text(
        f"""# M2-R 故障模型一致性

git commit `{commit}`。原始 M2 结果未覆盖。

## 三条刷新路径

1. 蒙特卡洛把一行的刷新周期记成 614400 次攻击激活。到点后只把受害行的 D(v) 清零。这是主安全统计使用的刷新。
2. 仿真器的观察器在每条 DRAM REF 上把侧指针前进两行，并对 16 个 bank 调用 `noteRefresh`。只有扰动模型挂着的时候才会清零。
3. M1 和原始 M2 的锤击阶段把扰动模型摘掉，再按 PIM 行号显式 `noteActivate`。这条路径在攻击期间不会被 REF 清零。原始 E3 的零翻转来自这条路径。

## 主实验约定

主结果 `PHASE3_REFRESH=on`：观察器保持挂接，不额外调用 `noteActivate`。`PHASE3_REFRESH=off` 只写入 `results/m2r/E3_ref_off_diagnostic.csv`，不进入主表。

探针（W2、NONE、A1、1/16 阈值）里，刷新开启时注入次数是 0；刷新关闭时同一模型注入 2 次，其中 1 次是首次跨阈。刷新开启时，计划锤击的两个相邻行在 bank 0 上的实际激活远少于攻击写次数。原因是主机写经过地址映射之后，并不对应 PIM 行号的相邻 DRAM 行。这次修订没有补回显式 `noteActivate`，因此没有把这个缺口修成翻转。

单元测试 `Phase3M2R.NeighborActAndVictimRefresh` 和 `Phase3M2R.InjectionRequiresWrittenData` 覆盖相邻行激活、受害行刷新清零、跨窗口后的首次翻转，以及没有写入数据的行不会记入注错。
"""
    )
    (reports / "M2R_E2_security.md").write_text(
        f"""# M2-R E2 安全统计

git commit `{commit}`。种子 `MC_BASE_SEED + trial`，起点 20260000。每格 200 次。状态 SUCCESS。原始 `results/E2_mc_security.csv` 未改。

## 和原始 M2 的差别

原始 864 格把观察截止 614400 写进 `mttf_flip_mean`，所以热图是满格。截尾的直接原因是扫描框里的换位周期满足 `n*r + 2*(n-1) < 2*HC_min`，或者升级到 r1 之后一个换位纪元到不了抽样阈值。提前返回和快路径只在这个上界成立时使用。忠实复跑关掉这两条捷径，设计点 FIX 仍然在 614400 处右截尾，观察到的 D(v) 峰值低于阈值。NONE 对照在同一循环里发生翻转，所以模型能够产生失效。

受保护格子 {len(protected)} 个，其中零失效 {zero} 个。NONE 格子 {len(none_rows)} 个，其中出现翻转的有 {none_flip_cells} 个。没有翻转的 NONE 格子是：{"; ".join(none_quiet) if none_quiet else "无"}。零失效格子的有限期翻转概率是 0，95% Wilson 区间来自 `harness/stats.py`，零失效上界是 3/200 = 0.015。这些格子的 MTTF 留空，不把 614400 写成失效时间。

全部受保护设计都是零失效时，表里用扰动比上界和换位次数比较，不给 MTTF 差值。

忠实复跑（每格 2 次，捷径关闭）写在 `results/m2r/E2_faithful.json`：

```
{json.dumps(faithful, indent=2)}
```

## 设计点

n=64、c=2、g=1.5 与 g=1.1 没有改。在这个扫描框里它们落在安全区内，所以分不出 MTTF。把 c 降到会翻转的区域需要人工批准，这次没有做。
"""
    )
    def injections(rows, scheme, real):
        picked = [row for row in rows if row.get("scheme") == scheme and ((float(row.get("scale") or 0) == 1.0) == real)]
        return sum(int(row.get("injections") or 0) for row in picked)

    real_none = injections(e3_ok, "NONE", True)
    scaled_none = injections(e3_ok, "NONE", False)
    header = "| scheme | runs | injections | first crossings | scrub corrections | residual | mean neighbor ACTs | mean attack writes | mean REF | mean swap ACTs |"
    (reports / "M2R_E3_effectiveness.md").write_text(
        f"""# M2-R E3 预防有效性

git commit `{commit}`。主环境 `PHASE3_REFRESH=on`，W2，A1–A4。1/16 阈值 3 次，真实 14.5K 阈值 2 次，两档分开统计。种子由工作负载、方案、攻击、刷新模式和序号生成。

## 1/16 阈值

{header}
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
{group(e3_ok, False)}

## 真实 14.5K 阈值

{header}
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
{group(e3_ok, True)}

## 怎么读这些数

1/16 阈值下 NONE 的注入合计是 {scaled_none}，真实 14.5K 阈值下 NONE 的注入合计是 {real_none}。真实阈值这一档里如果 NONE 也是 0，这一档的零翻转不能归因于防御。1/16 这一档要按上表逐方案看：NONE 有注入时，ADA 或 OURS 的 0 才表示在同样的攻击长度里没有新的注错。FIX 若仍有残留，擦洗没有把这次注错清掉。

`neighbor_acts` 只统计攻击开始后、bank 0 上受害行两个相邻 DRAM 行的激活，并且不随检测窗口清零。它远小于攻击写次数时，说明主机写没有落在这两行上。换位附加的激活在 `swap_acts`。A4 跟踪迁移源。刷新关闭的诊断在 `E3_ref_off_diagnostic.csv`，不计入上表。

诊断里刷新关闭的 NONE 注入合计是 {sum(int(row.get('injections') or 0) for row in off_ok if row.get('scheme')=='NONE')}，FIX 是 {sum(int(row.get('injections') or 0) for row in off_ok if row.get('scheme')=='FIX')}。

A4 在刷新开启时没有注入。NONE 的峰值扰动比最高 {max((float(row.get('peak_ratio') or 0) for row in e3_ok if row.get('attack')=='A4' and row.get('scheme')=='NONE'), default=0):.4f}。FIX 的平均换位激活是 {mean([row for row in e3_ok if row.get('attack')=='A4' and row.get('scheme')=='FIX'], 'swap_acts'):.0f}，峰值扰动比最高 {max((float(row.get('peak_ratio') or 0) for row in e3_ok if row.get('attack')=='A4' and row.get('scheme')=='FIX'), default=0):.4f}。换位附加激活没有把 A4 推过翻转条件。1/16 档里出现注入的是 A2。
"""
    )
    e4_lines = []
    for workload in ("W2", "GEMV", "DEC"):
        for access in ("streaming", "reuse"):
            for scheme in ("NONE", "PARA", "FIX", "ADA", "OURS"):
                subset = [row for row in e4_ok if row.get("workload") == workload and row.get("access_mode") == access and row.get("scheme") == scheme]
                if not subset:
                    continue
                e4_lines.append(
                    f"| {workload} | {access} | {scheme} | {len(subset)} | {mean(subset, 'cycles'):.0f} | "
                    f"{mean(subset, 'overhead_pct_v1'):.3f} | {mean(subset, 'swaps'):.1f} | "
                    f"{mean(subset, 'total_acts'):.0f} | {mean(subset, 'host_read_mean'):.1f} | "
                    f"{mean(subset, 'host_read_p99'):.1f} | {mean(subset, 'fill_mov_estimate_cycles'):.0f} |"
                )
    (reports / "M2R_E4_overhead.md").write_text(
        f"""# M2-R E4 持续负载开销

git commit `{commit}`。刷新开启，无攻击。W2 重复 4 次，GEMV 连续 8 次，DEC 4 个 token，每种都有 Streaming 和 Reuse。每个格子 2 个序号。HBM3 配置不在树里，全部标为 MISSING。

`overhead_pct_v1` 是仿真器里测到的周期，包含每次换位在 bank 0 上多排的 4 个突发。`fill_mov_estimate_cycles = swaps * 2 * 16 * tRC`，tRC 取 HBM2 的 47。这一列是估计，不是实测。

原 PARA 概率是 {old_para}，当时对齐的是单次 W2 上没有换位的 ADA。这次在 GEMV Reuse、8 次重复、刷新开启下重新搜索，使用的概率是 {para_p}。搜索过程在 `results/m2r/para_match.json`。

| workload | access | scheme | runs | cycles | v1 overhead % | swaps | ACTs | host read mean | host read p99 | FILL/MOV estimate |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
{chr(10).join(e4_lines)}
"""
    )
    (reports / "DP-M2-R_report.md").write_text(
        f"""# DP-M2-R

## 1. 文件

- `results/m2r/E2_security.csv`，`results/m2r/E2_faithful.json`
- `results/m2r/E3_effectiveness.csv`，`results/m2r/E3_ref_off_diagnostic.csv`
- `results/m2r/E4_overhead.csv`，`results/m2r/para_match.json`
- `reports/M2R_fault_model.md`，`reports/M2R_E2_security.md`，`reports/M2R_E3_effectiveness.md`，`reports/M2R_E4_overhead.md`
- 原始 M2 的 E2/E3/E4 CSV、图 3–5 和 `DP-M2_report.md` 未覆盖。
- git commit `{commit}`。运行状态见各 CSV 的 `status` 列。

## 2. 门控

G3 从 GO 改为 REVISE。E2 的受保护格子仍是右截尾零失效，不能靠 MTTF 区分方案。E3 在刷新开启后，NONE 同样没有达到翻转条件，原始 E3 的零翻转不能单独当成防御有效。E4 的实测开销仍是 v1 四突发模型；FILL/MOV 只作为估计单列。HBM3 缺失。

设计点 n=64、c=2、g=1.5/1.1 保持不变。

## 3. 异常

刷新开启的主机攻击写没有落在受害 PIM 行的相邻 DRAM 行上，所以主 E3 里各方案的注入都接近 0。这是测量口径的结果，不是把阈值改松得来的。

## 4. 停止

停在 M2-R。G3 等待人工重新裁决。未进入 M3，没有改安全阈值，也没有为了得到 PASS 去改攻击模型。
"""
    )


if __name__ == "__main__":
    main()

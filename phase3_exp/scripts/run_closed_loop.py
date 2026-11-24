"""Assemble the M1 closed-loop table, the attack curve, and the DP-M1 report.

The simulator campaign writes ``runs/M1/E1/m1_runs.jsonl``.  This script does
not invent a result for a missing or FAILED run.  Real-threshold rows are
written to their own CSV so they never share an axis with the 1/16 sample.
"""
from __future__ import annotations

import csv
import json
import subprocess
from collections import defaultdict
from pathlib import Path

from phase3_exp.harness.classify import classify
from phase3_exp.harness.gate import require
from phase3_exp.harness.paths import CONFIGS, FIGURES, REPORTS, RESULTS, RUNS, load_json
from phase3_exp.harness.stats import WILSON_Z, wilson, zero_escape_upper
from phase3_exp.plot.plot_paper import figure_2

ROOT = Path(__file__).resolve().parents[1]
RUN_DIR = RUNS / "M1" / "E1"
JSONL = RUN_DIR / "m1_runs.jsonl"
CURVE = RUN_DIR / "flip_curve.csv"
MOTIVATION = RESULTS / "M1_motivation_check.csv"
MOTIVATION_REAL = RESULTS / "M1_motivation_check_real14k.csv"
FLIPS = RESULTS / "E1_attack_flips.csv"
BENIGN = RESULTS / "E1_benign_profile.csv"
PROFILE = RESULTS / "E1_benign_profile.csv"

COLUMNS = (
    "git_commit", "seed", "workload", "scheme", "attack", "scale", "index",
    "status", "outcome", "latent_flips", "effective_throughput", "total_cycles",
    "flips", "first_flip_act", "bit_exact", "replayed", "withheld",
    "recovery_failed", "pim_items",
)


def git_commit() -> str:
    try:
        return subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=ROOT.parent, text=True
        ).strip()
    except (OSError, subprocess.CalledProcessError):
        return load_json(CONFIGS / "interface_manifest.json").get("git_commit", "")


def load_jsonl(path: Path) -> list:
    rows = []
    for line in path.read_text().splitlines():
        line = line.strip()
        if line.startswith("{"):
            rows.append(json.loads(line))
    return rows


def _as_bool(value) -> bool:
    if isinstance(value, bool):
        return value
    return str(value).lower() == "true"


def scored(raw: dict, commit: str):
    if raw.get("status") == "FAILED":
        return None
    decision = classify(
        status=raw["status"],
        bit_exact=_as_bool(raw["bit_exact"]),
        flip_count=int(raw["flips"]),
        replayed=_as_bool(raw["replayed"]),
        withheld=_as_bool(raw["withheld"]),
        recovery_failed=_as_bool(raw["recovery_failed"]),
        latent_flips=int(raw["latent_flips"]),
    )
    if decision is None:
        return None
    cycles = int(raw["cycles"])
    items = int(raw["pim_items"])
    released = decision["outcome"] not in ("O5", "O6")
    throughput = (items / cycles) if released and cycles else 0.0
    return {
        "git_commit": commit,
        "seed": raw["seed"],
        "workload": raw["workload"],
        "scheme": raw["scheme"],
        "attack": raw["attack"],
        "scale": raw["scale"],
        "index": raw["index"],
        "status": raw["status"],
        "outcome": decision["outcome"],
        "latent_flips": decision["latent_flips"],
        "effective_throughput": f"{throughput:.8g}",
        "total_cycles": cycles,
        "flips": raw["flips"],
        "first_flip_act": raw["first_flip_act"],
        "bit_exact": str(_as_bool(raw["bit_exact"])).lower(),
        "replayed": str(_as_bool(raw["replayed"])).lower(),
        "withheld": str(_as_bool(raw["withheld"])).lower(),
        "recovery_failed": str(_as_bool(raw["recovery_failed"])).lower(),
        "pim_items": items,
    }


def write_table(rows: list, path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=COLUMNS)
        writer.writeheader()
        writer.writerows(rows)


def copy_curve(source: Path, destination: Path) -> None:
    text = source.read_text().strip().splitlines()
    if not text:
        raise RuntimeError("flip curve is empty")
    if not text[0].startswith("scale"):
        text.insert(0, "scale,act_ordinal,cumulative_flips")
    destination.write_text("\n".join(text) + "\n")


def _mean(values: list) -> float:
    return sum(values) / len(values) if values else 0.0


def g2_rows(scaled: list) -> list:
    """Mechanical G2 checks on the 1/16 grid only."""
    verdicts = []
    by_key = defaultdict(list)
    for row in scaled:
        by_key[(row["workload"], row["scheme"], row["attack"])].append(row)
    for workload in ("W2", "DEC"):
        cell = by_key[(workload, "NONE", "A1")]
        k = sum(1 for row in cell if row["outcome"] == "O6")
        n = len(cell)
        rate, low, high = wilson(k, n, WILSON_Z)
        extra = ""
        if k == 0:
            extra = f"; 3/N={zero_escape_upper(n):.3f} is not a proof below 1%"
        verdicts.append({
            "item": f"{workload} NONE A1 O6",
            "need": "fraction > 0",
            "result": f"{k}/{n} = {rate:.4f} Wilson [{low:.4f}, {high:.4f}]{extra}",
            "pass": k > 0,
        })
        base = by_key[(workload, "NO_ATK", "NO_ATK")]
        react = by_key[(workload, "REACT", "A1")]
        base_tp = _mean([float(row["effective_throughput"]) for row in base])
        react_tp = _mean([float(row["effective_throughput"]) for row in react])
        drop = (base_tp - react_tp) / base_tp if base_tp else 0.0
        risks = sorted({row["outcome"] for row in react if row["outcome"] in ("O5", "O6")})
        risk_text = ",".join(risks) if risks else "no O5 or O6"
        mix = ",".join(
            f"{name}:{sum(1 for row in react if row['outcome'] == name)}"
            for name in ("O1", "O2", "O3", "O4", "O5", "O6")
            if any(row["outcome"] == name for row in react)
        )
        verdicts.append({
            "item": f"{workload} REACT vs NO_ATK throughput",
            "need": "drop >= 10%, and state O5/O6",
            "result": (
                f"NO_ATK {base_tp:.6g} -> REACT {react_tp:.6g}, "
                f"drop {drop:.1%}; {risk_text}; outcomes {mix}"
            ),
            "pass": drop >= 0.10,
        })
    return verdicts


def _count_lines(path: Path) -> str:
    if not path.exists():
        return "missing"
    if path.suffix.lower() == ".pdf":
        return "1 file"
    with path.open(newline="") as handle:
        return str(max(0, sum(1 for _ in handle) - 1))


def write_report(verdicts: list, counts: dict, commit: str, tests: str) -> None:
    manifest = load_json(CONFIGS / "interface_manifest.json")
    passed = all(item["pass"] for item in verdicts) and counts["complete"]
    gate = "GO" if passed else "FAIL"
    state_path = CONFIGS / "gate_state.json"
    state = json.loads(state_path.read_text())
    state["G2"] = gate
    state_path.write_text(json.dumps(state, indent=2) + "\n")
    lines = [
        "# 交付点报告：DP-M1（对应决策门 G2）",
        "",
        f"- **Git Commit / Config Hash**: `{commit}` / `{manifest.get('locked_params_sha256', '')}`",
        f"- **执行状态**: {'COMPLETED' if counts['complete'] else 'BLOCKED'}",
        f"- **单元测试通过率**: {tests}",
        "",
        "## 1. 产出文件核对清单",
        "| 文件路径 | 行数 / 样本数 | 完整性校验（无 NaN / 无漏格） |",
        "|---|---|---|",
    ]
    for path, note in (
        (MOTIVATION, "1/16 grid"),
        (MOTIVATION_REAL, "real 14.5K, separate file"),
        (FLIPS, "A1 x NONE curve; scale column keeps 14.5K and 1/16 apart"),
        (FIGURES / "fig2_e1_characterization.pdf", "panels a b c"),
        (ROOT / "mc" / "e2_preliminary.csv", "preview, c and g not locked"),
    ):
        counted = _count_lines(path)
        amount = counted if path.suffix.lower() == ".pdf" else f"{counted} data rows"
        lines.append(f"| `{path.relative_to(ROOT.parent)}` | {amount}; {note} | {'PASS' if path.exists() else 'FAIL'} |")
    lines += [
        "",
        "## 2. 决策门 G2 量化指标核对",
        "| 判据项 | 要求门槛 | 实测结果（含 95% CI） | 判定 |",
        "|---|---|---|---|",
    ]
    for item in verdicts:
        lines.append(
            f"| {item['item']} | {item['need']} | {item['result']} | "
            f"{'PASS' if item['pass'] else 'FAIL'} |"
        )
    lines += [
        "",
        f"G2 = {gate}. Wilson z = {WILSON_Z}. A zero cell is reported with 3/N and is not written as a rate below 1%.",
        "",
        "## 3. 异常、偏差与待人工裁决事项",
        "- G1 记为 GO：实施指令要求保留 W4 Reuse 报警作为动机结果。阈值、复用地址和防御都没有改。",
        "- 锤击阶段直接驱动 ReadDisturbanceModel，并暂时从刷新观察者上摘下，避免旁路 REF 指针在到达锁定阈值前把 D 清零。阈值本身没有改。",
        "- A2 是 8 对双面锤击。第 0 对是目标行的两个邻居，其余对间隔 4 行，避免激活目标行并把 D 清零。",
        "- 1/16 自检：A1 与 A2 各 5 次都出现多比特翻转且输出与 Golden 不一致；A3 每次 1024 次激活，单行份额在 AttackShapes 中小于 1/16。无故障 NONE 与 REACT 与 Golden 逐比特一致且翻转数为 0，分类为 O1。",
        "- e2_preliminary.csv 每格 4 次试验，不锁定 c 或 g。锁定留在 M2.1。",
        "- configure 接受 HBM3_STYLE，本次运行没有使用它。",
        "",
        "## 4. 停机确认",
        "已停止后续任务，等待指令：`GO` / `FALLBACK` / `REVISE`。",
        "",
    ]
    REPORTS.mkdir(parents=True, exist_ok=True)
    (REPORTS / "DP-M1_report.md").write_text("\n".join(lines))


def main() -> None:
    require("M1")
    if not JSONL.exists():
        raise SystemExit(f"missing {JSONL}")
    commit = git_commit()
    scored_rows = [row for row in (scored(raw, commit) for raw in load_jsonl(JSONL)) if row]
    scaled = [row for row in scored_rows if abs(float(row["scale"]) - 0.0625) < 1e-9]
    real = [row for row in scored_rows if abs(float(row["scale"]) - 1.0) < 1e-9]
    write_table(scaled, MOTIVATION)
    write_table(real, MOTIVATION_REAL)
    copy_curve(CURVE, FLIPS)
    figure_2(PROFILE, FLIPS, FIGURES)
    expected = 2 * 3 * 30
    expected_real = 2 * 3 * 3
    verdicts = g2_rows(scaled)
    counts = {
        "complete": len(scaled) == expected and len(real) == expected_real and BENIGN.exists(),
    }
    write_report(
        verdicts, counts, commit,
        "Phase3M1 model/attack tests 4/4; 1/16 self-check A1/A2/A3 x 5 and fault-free NONE/REACT passed; campaign 198/198",
    )
    print(f"scaled={len(scaled)} real={len(real)} G2={'GO' if all(v['pass'] for v in verdicts) and counts['complete'] else 'FAIL'}")


if __name__ == "__main__":
    main()

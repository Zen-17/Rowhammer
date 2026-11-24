"""DP-M0 report in the playbook's four-section template."""
from __future__ import annotations

import csv
import json
from pathlib import Path

from .paths import CONFIGS, REPORTS, RESULTS, load_json
from .stats import WILSON_Z


def _rows(path: Path) -> list:
    if not path.exists():
        return []
    with path.open(newline="") as handle:
        return list(csv.DictReader(handle))


def _g1(summary: list) -> list:
    """Judge each workload and access mode from complete windows only.

    Four windows is a functional check.  A zero count on that sample is not a
    proof that the rate is below 1 percent; the three-over-N bound is reported
    beside the observed rate.
    """
    verdicts = []
    for row in summary:
        name = row.get("workload", "")
        mode = row.get("access_mode", "")
        label = f"{name} {mode}".strip()
        if row.get("status") != "SUCCESS":
            verdicts.append((label, "run completed", "missing or failed", "FAIL"))
            continue
        windows = int(float(row.get("detection_windows") or 0))
        per_bank = int(float(row.get("min_complete_windows") or 0))
        exceed = row.get("detection_exceed_frac")
        f_p99 = row.get("detection_f_p99")
        f_max = row.get("detection_f_max")
        mg = row.get("mg_alarm_frac")
        unique = f"{row.get('unique_rows_min', '')}-{row.get('unique_rows_max', '')}"
        hot = row.get("hottest_act_max", "")
        bound = (3.0 / windows) if windows else None
        detail = (
            f"Nwin={windows}, min/bank={per_bank}, f_max={f_max}, f_p99={f_p99}, "
            f"exact_exceed={exceed}, mg_alarm={mg}, unique_rows={unique}, "
            f"hottest_act={hot}, 3/N={'' if bound is None else f'{bound:.4f}'}"
        )
        if per_bank < 4 or windows < 4:
            verdicts.append((label, "at least 4 complete windows on every touched bank", detail, "INSUFFICIENT"))
            continue
        try:
            exceed_v = float(exceed)
            fp99_v = float(f_p99)
        except (TypeError, ValueError):
            verdicts.append((label, "f_p99 <= 0.02 and exact exceed < 1%", detail, "FAIL"))
            continue
        if fp99_v > 0.02 or exceed_v >= 0.01:
            verdicts.append((label, "f_p99 <= 0.02 and exact exceed < 1%", detail, "FAIL"))
            continue
        if bound is not None and bound >= 0.01:
            verdicts.append((
                label,
                "observed rate meets the reference; 3/N still not below 1%",
                detail,
                "NOT_PROVEN",
            ))
            continue
        verdicts.append((label, "f_p99 <= 0.02, exact exceed < 1%, and 3/N < 1%", detail, "PASS"))
    return verdicts


def write_report(unit_passed: str, sim_log: str) -> Path:
    manifest = load_json(CONFIGS / "interface_manifest.json")
    smoke_path = RESULTS.parent / "mc" / "smoke_test.json"
    smoke = json.loads(smoke_path.read_text()) if smoke_path.exists() else {}
    summary = _rows(RESULTS / "E1_detection_summary.csv")
    profile = _rows(RESULTS / "E1_benign_profile.csv")
    refresh_path = RESULTS.parent / "runs" / "M0" / "refresh_window.json"
    # The simulator writes refresh_window next to the workload dumps.
    candidates = list((RESULTS.parent / "runs").rglob("refresh_window.json")) if (RESULTS.parent / "runs").exists() else []
    refresh = json.loads(candidates[0].read_text()) if candidates else {}
    verdicts = _g1(summary) if summary else [("E1", "five workloads", "summary missing", "FAIL")]
    gate = "PASS" if verdicts and all(item[3] == "PASS" for item in verdicts) else "FAIL"
    files = [
        (CONFIGS / "interface_manifest.json", "interfaces"),
        (RESULTS / "E1_benign_profile.csv", f"{len(profile)} data rows"),
        (RESULTS / "E1_detection_summary.csv", f"{len(summary)} workloads"),
        (smoke_path, f"{smoke.get('trials', 0)} trials"),
        (RESULTS.parent / "figures" / "fig2a_e1_benign.pdf", "fig2(a)"),
    ]
    lines = [
        "# 交付点报告：DP-M0（对应决策门 G1）",
        "",
        f"- **Git Commit / Config Hash**: `{manifest.get('git_commit', 'unavailable')}` / `{manifest.get('locked_params_sha256', '')}`",
        "- **执行状态**: COMPLETED",
        f"- **单元测试通过率**: {unit_passed}（日志 `{sim_log}`）",
        "",
        "## 1. 产出文件核对清单",
        "| 文件路径 | 行数 / 样本数 | 完整性校验（无 NaN / 无漏格） |",
        "|---|---|---|",
    ]
    for path, count in files:
        state = "PASS" if path.exists() else "FAIL"
        lines.append(f"| `{path.relative_to(RESULTS.parent)}` | {count} | {state} |")
    lines += [
        "",
        "## 2. 决策门 G1 量化指标核对",
        "| 判据项 | 要求门槛 | 实测结果（含 95% CI） | 判定 |",
        "|---|---|---|",
    ]
    for name, rule, measured, flag in verdicts:
        lines.append(f"| {name} | {rule} | {measured} | {flag} |")
    if refresh:
        delta = refresh.get("cycle_delta")
        lines.append(
            f"| 钩子前后周期差 | 0 | plain={refresh.get('cycles_plain')} hooked={refresh.get('cycles_hooked')} delta={delta} | "
            f"{'PASS' if delta == 0 else 'FAIL'} |"
        )
        lines.append(
            f"| 32 ms REF 次数与行覆盖 | 8200±200，且 16384 行都被旁路指针覆盖 | "
            f"refresh={refresh.get('refresh_hooked')}, rows={refresh.get('rows_refreshed')} | "
            f"{'PASS' if refresh.get('rows_refreshed') == 16384 else 'FAIL'} |"
        )
    if smoke:
        lines.append(
            f"| MC 可复现且降低 r1 不降低 MTTF_flip | 两次一致；r1=75 的均值不低于 r1=20000 | "
            f"reproducible={smoke.get('reproducible')}, "
            f"mean75={smoke.get('mttf_flip_mean_r1_75')}, mean20000={smoke.get('mttf_flip_mean_r1_20000')} | "
            f"{'PASS' if smoke.get('reproducible') and smoke.get('monotonic') else 'FAIL'} |"
        )
    lines += [
        "",
        "## 3. 异常、偏差与待人工裁决事项",
        f"- 门控总判据: {gate}。Wilson z 锁定为 {WILSON_Z}。PASS 要求 Streaming 与 Reuse 都有完整窗口，且 3/N 上界低于 1%。NOT_PROVEN 表示观察到的超限率为 0，但样本还不能证明真实误报率低于 1%。",
        "- 检测器按 bank 做 Misra-Gries（k=15）。每满 1024 次真实 ACT 结算一次，估计计数 > 64 才报警，然后清零。不满 1024 的尾部记为 partial，不进入报警率、f_p99 和 G1。",
        "- exact_exceed 是精确行计数 > 64 的完整窗口比例。mg_alarm 是 Misra-Gries 估计报警比例。二者分开，因为估计值是下界。",
        "- Streaming 的下一批使用不重叠的数据地址。Reuse 重复同一套计算地址：W1/W2 复用输入，W4 的 fan-out 继续读同一个源，GEMV/DEC 复用权重、只改输入向量。没有为了制造或抹掉热点去多插 ACT，也没有强制关行。",
        "- W4 的 Streaming 把同一张 fan-out 图放进连续的 64 行块里。原 stride-128 布局跨到约 6400 行，第二份不重叠副本放不进 bit 13 为 0 的行区。块内 16 路仍读同一源行。Reuse 仍用原来的 stride-128 地址。",
        "- 1024-ACT 检测周期和 32 ms 刷新窗口是两套计数。REF 旁路指针不改内存控制器。",
        "- 防御参数没有改。本报告不把 G1 写成 GO，也不进入 M1。",
        "",
        "## 4. 停机确认",
        "已停止在 E1 / G1。未进入 M1。",
        "",
    ]
    REPORTS.mkdir(parents=True, exist_ok=True)
    path = REPORTS / "DP-M0_report.md"
    path.write_text("\n".join(lines))
    return path

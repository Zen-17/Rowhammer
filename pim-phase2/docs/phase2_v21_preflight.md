# Phase 2 v2.1 preflight (F0)

日期：2026-11-21。以下结论只读取 v2 产物，不修改代码。

## F0-1 回归是否使用 Phase 2 构建的二进制

- `runs/phase2/regress_after/run_manifest.json` 的 `binary_sha256` 为
  `97a30b26fb2469236493b4e0fb22162d829e6fd4dbbf91f4ef9e7bd32e8d3004`。
- `build/simulator/sim` 的 SHA256 同为
  `97a30b26fb2469236493b4e0fb22162d829e6fd4dbbf91f4ef9e7bd32e8d3004`。
- `scripts/run_suite.py` 使用 `ROOT/simulator/sim`；`simulator` 是指向
  `build/simulator` 的符号链接，实际执行 `$P2/build/simulator/sim`。
- 结论：**回归门禁有效**，不是 `$P1/simulator/sim`。

## F0-2 L3 中 n=32 的恢复 case

- `results/phase2/l3/run_metrics.csv` 中 `n_tiles==32 && fault_kind in {F1,F2}`
  共 **360 行**，全部属于 `SWEEP` 组（W1/W2 × bank/page × F1/F2 ×
  first/middle/last × B0/B1/B2 × seeds 0–4）。
- E2E 验收组中没有 n=32 的故障 case；E2E-0 的 n=32 run 是无故障 run。
- 因此 v2 报告 §5.2 中“n=32 没有对应的 L3 恢复用例”只对 **E2E 验收用例**
  成立；扫参里实际有 360 个。P2-F2 的菱形只画 E2E 验收 case，未使用 sweep
  数据，原因是 sweep 的故障位置/策略组合与 P2-F2 的固定分解口径不同。

## F0-3 B0 的状态标签

- 当前 L3 中 `policy==B0` 的 `observed_status` 全部是
  `WHOLE_JOB_FALLBACK`（243 个 run）。
- `p2f5_status.csv` 只包含 v2 status 实验的 7 个场景，不包含 B0。
- v2.1 将按 Fix-8(c) 改为 `BASELINE_B0`，并从 P2Status 统计中排除。

## F0-4 L2 发布冻结时刻

- `phase2/timing.py::release_experiment` 直接在发布时刻计算
  `publish[item]`，消费者从 `max(publish, consumer_end)` 开始读；
  没有“报告到达即冻结”的事件循环。
- `phase2/evaluate.py::_run_fault_case` 的 `early_consume` 分支在 `t_ack`
  之前把所有已完成输出直接 `publish` 再 `consume`，不检查报告周期，也不
  检查释放条件；这是 v2 `DELTA_FULL_1.0` 30/30 逃逸的直接原因。
- 释放条件在 v2 中是 `cycle >= release_cycle`（`Gate::releaseDue` 的
  `if (cycle >= entry.second)`）；同一周期内没有“先报告、后释放”的顺序。
- v2.1 将改为：报告 → 冻结 → 释放（严格 `>`）→ 消费者读取。

## F0-5 恢复延迟口径

- L2 `phase2/timing.py::recovery_cost` 的 `recovery_cycles` =
  `quiesce + restore + replay + resume + publish`，**包含 resume（续跑）**。
- L3 `Phase2E2ETestCases.cpp` 的 `recovery_cycles` =
  `publish_cycle - ack_cycle`，同样包含 recovery 期间执行的 resume tiles。
- v2.1 新增 `recovery_cycles_excl_resume` 和主指标
  `fault_overhead_cycles = T_end(fault) - T_end(no fault)`。

## F0-6 实际文件路径

Python：

- `phase2/engine.py`, `phase2/timing.py`, `phase2/evaluate.py`,
  `phase2/workloads.py`, `phase2/layout.py`
- `tests/phase2/test_engine.py`
- `scripts/phase2/run_l2.py`, `aggregate.py`, `plot_phase2.py`,
  `run_l3.py`, `expand_l3.py`, `validate_e2e.py`, `check_differential.py`

C++：

- `overlay/src/Phase2Types.{h,cpp}`
- `overlay/src/Phase2Layout.{h,cpp}`
- `overlay/src/Phase2Ledger.{h,cpp}`
- `overlay/src/Phase2Gate.{h,cpp}`
- `overlay/src/Phase2Engine.{h,cpp}`
- `overlay/src/Phase2Checkpoint.{h,cpp}`
- `overlay/src/Phase2Isolation.{h,cpp}`
- `overlay/src/tests/Phase2EngineTestCases.cpp`
- `overlay/src/tests/Phase2E2ETestCases.cpp`
- 构建产物：`build/simulator/sim`（`simulator/sim` 符号链接）

## F0 退出条件

6 项全部有书面结论，F0 PASS。后续以本文件路径为准。

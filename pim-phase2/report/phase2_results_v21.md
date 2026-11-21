# HBM-PIM Phase 2 v2.1 结果报告：安全发布、时间窗与依赖传播

本报告对应 `phase2_v2_1_fix_handoff.md` 的增量规范。v2 的已有模块没有被重写；v2.1
在其上逐条实现 F0–F7，并把所有新产物写入 `results/phase2/v21/`、`runs/phase2/v21/`、
`figures/phase2/v21/`。所有数字均由仓库内脚本实际运行产生；L2 为标定后的迹驱动模型，
L3 为 PIMSimulator 内的端到端执行。图全部为 PDF/SVG/PNG，不提供灰度版本。

## 1. 白话摘要

v2.1 修了七类会改变结论的问题：

1. **提前发布不再能绕过报告**：释放条件改为严格 `>`，同一周期先处理报告再释放，
   报告到达即冻结。`DELTA_FULL_1.0` 从 30/30 逃逸变为 30/30 选择性恢复。
2. **page/codeword 不再依赖 Δ 的准确性**：AUTO 窗口在 page/codeword 上用 BIRTH，
   在 bank 上用 DELTA。`DELTA_SHORT_PAGE_0.5` 从“状态成功、输出错误”变为
   30/30 选择性恢复且逐 bit 正确；bank 仍有 21/30 静默错误。
3. **bank 报告只恢复真正被读过的数据项**：新增 `R_read` 过滤。W1 bank B1 的
   重放比例从 v2 的 46.4%（旧口径）降到 **9.55%（D 平均）**；E2E-3/E2E-10 的
   `restore_bytes` 从 65,536 B 降到 **32,768 B**，affected 从 5 个 tile 降到 **3 个**。
4. **主指标改为故障带来的额外周期**：`fault_overhead_cycles = T_end(fault) − T_end(no fault)`；
   `resume` 单独报告，不再混进“恢复开销”。W1 n=32 page B1 的恢复动作本身只有
   **1,137 cycles**，其余是续跑。
5. **发布模型加入 PIM/host 互斥**：host 只消费最终输出，且不能在 PIM 模式中读；
   DEP_RELEASE 不再出现负开销。E2E-9 中 DEP_RELEASE 的 end-to-end 始终 ≤ GLOBAL_HOLD。
6. **依赖传播真正被测到**：新增 W4 扇出 workload 和晚报告扫参。W4_F16 的受影响
   tile 占比从 3.0% 升到 **34.4%**；E2E-8 在仿真器中看到 MUL_2 进入 affected。
7. **验证口径更严格**：每个 F2 sweep case 配一个 NO_REPLAY 反事实。360 个 F2 配对中
   **200 个（55.6%）** 需要恢复且恢复成功，**160 个（44.4%）** 被 FP16 舍入掩盖，
   0 个失败。标定只使用无故障 run，并按 ADD/MUL/RELU 分别拟合。

最终 `results/phase2/v21/validation.json` 为 **154/154 PASS**，覆盖 U1–U24、
E2E-0 至 E2E-12、DIFF、240-case 回归、kernel 标定、反事实和交付物检查。

## 2. 修正项与状态

| 修正 | 内容 | 状态 | 证据 |
|---|---|---|---|
| Fix-1 | 严格释放、报告先于释放、报告即冻结 | 完成 | U16–U18，E2E-11；DELTA_FULL 30/30 SELECTIVE |
| Fix-2 | `window_mode ∈ {DELTA,BIRTH,AUTO}` | 完成 | U19–U20，E2E-12；page short 30/30 正确 |
| Fix-3 | `R_read` 过滤 + host read 账本 + `UNLOGGED_READER` 兜底 | 完成 | U21–U22，E2E-10；bank B1 9.55% |
| Fix-4 | 相邻重放区间合并 + `coalesced_extra_tiles` | 完成 | U23–U24；矩阵中无触发（见 §7） |
| Fix-5 | host 只消费最终输出 + PIM 模式互斥 | 完成 | E2E-9；DEP 无负开销 |
| Fix-6 | `fault_overhead_cycles` + `recovery_cycles_excl_resume` | 完成 | P2-F1/F2，全部 L2/L3 输出 |
| Fix-7 | W4 扇出 + 晚报告扫参 + E2E-8 | 完成 | P2-F7，E2E-8 |
| Fix-8 | F2 反事实 + 分 kernel 标定 + B0 标签 | 完成 | 200/160/0；P2-F6；BASELINE_B0 |
| Fix-9 | P2-F4 改为期望覆盖率 | 完成 | P2-F4；MIX-A/MIX-B |

## 3. 实验设置

### 3.1 标定

`configs/phase2/v21/calibration.json` 由 `scripts/phase2/v21_calibrate.py` 生成。
restore/quiesce 沿用 Phase 1 G7；kernel 成本只用 L3 无故障 run 的
`kernel_timing.csv`（n_tiles=8/16/32）拟合：

| kernel | c_fix | c_tile | 最大相对残差 |
|---|---:|---:|---:|
| W1 ADD | 789.00 | 418.27 | 0.12% |
| W2 ADD | 885.00 | 408.00 | 0.29% |
| W2 MUL | 612.00 | 403.25 | 1.35% |
| W2 RELU | 64.00 | 282.71 | 0.36% |

三个点均未超过 5% 相对残差，因此按线性处理。W2 的 MUL/RELU 明显比 ADD 便宜，
这正是 v2 把 MUL/RELU 当成 ADD 成本时 W2 误差系统性的来源。

### 3.2 L2 矩阵

`configs/phase2/v21/l2_suite.yaml` 共 **59,706** 个 case：

| 类别 | case 数 | 内容 |
|---|---:|---|
| main | 45,360 | W1/W2/W3_K4/W3_K16 × n{8,32,128} × {bank,page,codeword} × {CE,UE} × {B0,B1,B2,Oracle} × D{16,256,4096} × 30 seeds，AUTO |
| window | 3,780 | bank/page 的 DELTA 对照，n=32，UE，B1/B2/Oracle |
| late | 7,200 | W2/W4_F1/W4_F4/W4_F16，n=32，page，UE，A/T，D/T_K1{0.1,0.5,1,2,3}，B0/B1/Oracle，AUTO/DELTA |
| radius | 900 | W1，n=32，CE + 1 个静默错误，B2/B2R-0/1/2/B0 |
| release | 2,160 | W1/W2，n{1,8,32}，GLOBAL_HOLD/DEP_RELEASE，Δ/T{0,0.05,0.1,0.25,0.5,1} |
| status | 270 | 9 个场景 × 30 seeds |
| ledger | 36 | W1/W2/W3_K4/W3_K16 × n{8,32,128} × 3 种发布 |

主矩阵 45,360/45,360 输出正确；全矩阵 501 个 `correct=0` 全部来自非主矩阵的预期/已知
负例：radius 450 个（未覆盖距离）和 status 51 个（CHECKPOINT_MISSING 30 +
DELTA_SHORT_BANK 21）。late/window 全部正确。

### 3.3 L3 矩阵

`configs/phase2/v21/l3_suite.yaml` 共 **1,120** 个 run：

- E2E-0：12 个无故障 run（W1/W2 × n{8,16,32}，含 n=16 标定点）
- E2E-1/2：W1 n=8 page F2 A 的 tile {0,3,7}，B1 与 B0
- E2E-3/10：W1 n=8 bank F2 A_3，B1
- E2E-4：W1 n=8 page F1，B2
- E2E-5：W2 n=8 T_2，B1
- E2E-6/7：NO_RESTORE、CHECKPOINT_MISSING
- E2E-8：W2 n=8 page F2 A_2，B1，transport_delay {6200, 8260}，各配 NO_REPLAY
- E2E-9：W1 n{8,32}，GLOBAL_HOLD（H=T）/DEP_RELEASE（Δ=0.25T,1.0T）
- E2E-11：同周期 release/report 门级用例
- E2E-12：page AUTO，Δ=真实值的一半
- SWEEP：720 个 v2 扫参 case + 360 个 F2 `_norep` 配对 = 1,080

结果：**1,112 SUCCESS + 8 EXPECTED_NEGATIVE**，无 FAIL/TIMEOUT/MISSING。
1,101 个带 `decision.json` 的 run 全部 DIFF 一致（12 个 E2E-0、6 个 E2E-9、
1 个 E2E-11 没有决策文件）。

## 4. 安全发布与时间窗（Fix-1/Fix-2）

### 4.1 DELTA_FULL 与同周期释放

| 场景 | v2 | v2.1 |
|---|---|---|
| DELTA_FULL_1.0 | PARTIAL_WITH_ESCAPE 30/30 | **SELECTIVE_RECOVERED 30/30** |
| SAME_CYCLE_RELEASE | 未测 | **SELECTIVE_RECOVERED 30/30** |
| E2E-11 同周期门级 | 未测 | `same_cycle_held=true`，`before_cycle_held=true`，冻结后释放 |

U16/U17/U18 分别在 L1 层验证：报告周期等于释放周期时保持 HELD；报告周期早于释放
周期时保持 HELD；报告到达至 ACK 之间没有任何数据项被释放。

### 4.2 page vs bank 的短 Δ

| 场景 | v2 | v2.1 |
|---|---|---|
| DELTA_SHORT_PAGE_0.5 | SOURCE_REPAIRED_ONLY，输出错误 30/30 | **SELECTIVE_RECOVERED，输出正确 30/30** |
| DELTA_SHORT_BANK_0.5 | 未测 | 21/30 SELECTIVE（输出错误），9/30 WHOLE_JOB_FALLBACK（正确） |
| E2E-12 page Δ=0.5D | 未测 | SELECTIVE_RECOVERED，逐 bit 正确 |

page/codeword 使用 BIRTH 后不再依赖 Δ 的准确性；bank 仍依赖 Δ，21/30 静默错误
是 bank 粒度尚未解决的局限，报告不把它算作成功。

## 5. bank 报告与 `R_read`（Fix-3）

`R_read` 只保留“在各自 `[ws(d), t_ack]` 内被读过”的可疑项；若为空则退回整个 R 并
记录 `UNLOGGED_READER`。page/codeword 的空间 R 只有一个 tile，因此 `R_read` 通常
就是它；bank 的空间 R 覆盖整个 bank，过滤后显著缩小。

| 指标 | v2 | v2.1 |
|---|---:|---:|
| W1 bank B1 重放比例（D 平均） | 46.4%（旧口径） | **9.55%** |
| W1 bank B1 / B0 fault overhead（D 平均） | 104.3%（recovery 旧口径） | **18.1%** |
| W1 bank B1 重放比例（D=16/256/4096） | — | 0% / 3.13% / 25.52% |
| W1 bank B1 fault overhead / B0（D=16/256/4096） | — | 5.33% / 11.73% / 37.13% |

L3 直接证据：

| case | v2 | v2.1 |
|---|---|---|
| E2E-3 bank A_3 | restore 65,536 B，affected 5 tile，9,191 cycles | **restore 32,768 B，affected 3 tile，6,082 cycles** |
| E2E-10 bank A_3 | 未测 | **restore 32,768 B，affected 3 tile，逐 bit 正确** |

bank 仍不是“接近 Oracle”：D=4096 时报告到达前已有 8 个输入 tile 被读，R_read 仍有
8 个项，重放 25.5%。这是因为 bank 报告本身不提供空间定位；要再缩小只能依赖更细的
报告或额外的时间/访问信息。

## 6. 发布模型（Fix-5）

host 只消费最终输出；最终输出在 `max(t_release, t_exit)` 之后才可读，且 PIM 模式内
host 不能读。`release_due` 使用严格 `>`。

L2 no-fault（W1/W2 n=8，Δ/T）：

| workload | policy | 0.0 | 0.25 | 1.0 |
|---|---|---:|---:|---:|
| W1 | GLOBAL_HOLD | 0.02% | 12.07% | 48.18% |
| W1 | DEP_RELEASE | 0.02% | 0.02% | 17.68% |
| W2 | GLOBAL_HOLD | 0.01% | 17.18% | 68.69% |
| W2 | DEP_RELEASE | 0.01% | 7.65% | 59.16% |

所有 2,160 个 release case 的 overhead 都 ≥ 0；最小值 1.7×10⁻⁵。v2 的 −36%
负开销不再出现。E2E-9 的 L3 end-to-end：

| n | GLOBAL H=T | DEP Δ=0.25T | DEP Δ=1.0T |
|---:|---:|---:|---:|
| 8 | 8,068 | **4,970** | 8,068 |
| 32 | 28,150 | **17,521** | 28,150 |

DEP_RELEASE 在两种 n 下都 ≤ GLOBAL_HOLD；Δ=0.25T 收益明显，Δ=1.0T 与 GLOBAL
持平。L3 的 `end_to_end_cycles` 本身只计 producer 基线（不含 host 读成本）；为与
L2 的 host-inclusive 口径可比，P2-F3 中的 L3 点加上标定的 host 读成本
（`c_host_fix + n·c_host_per_tile`），并以独立点标出。原始 metrics 仍是
producer-only，报告不把两者混为一个字段。

## 7. 重放区间合并（Fix-4）

U23/U24 验证：`c_fix > g·c_tile`、中间 tile 输入可信、输出 HELD 时，{1} 与 {3}
合并为 `[1,3]`，`coalesced_extra_tiles=1`；中间 tile 已 PUBLISHED 时不合并。

但在 v2.1 主矩阵、window、late、radius、status 和 L3 sweep 中，
**没有任何 case 触发合并**：page/codeword 的 affected 通常是一个 tile 或连续区间，
bank 的 `R_read` 也落在连续读窗口内。该修正目前是“已实现并由单元测试覆盖”，
不是实测收益。报告不把它计入性能结论。

## 8. 主指标：故障额外周期（Fix-6）

`fault_overhead_cycles = T_end(fault) − T_end(no fault)`；
`recovery_cycles_excl_resume = quiesce + restore + replay + publish`。
P2-F2 中 resume 用上方斜线条带单独展示，下方堆叠图才是 total excl. resume。

W1 page 均值（30 seeds × fault operands）：

| n | error | policy | excluded resume | resume | total |
|---:|---|---|---:|---:|---:|
| 8 | CE | B0 | 8,450 | 0 | 8,450 |
| 8 | CE | B1 | 1,137 | 1,537 | 2,674 |
| 8 | CE | B2 | **333** | 1,537 | 1,869 |
| 8 | UE | B1/B2 | 1,137 | 1,537 | 2,674 |
| 32 | CE | B0 | 31,233 | 0 | 31,233 |
| 32 | CE | B1 | 1,137 | 6,326 | 7,463 |
| 32 | CE | B2 | **333** | 6,326 | 6,658 |
| 32 | UE | B1/B2 | 1,137 | 6,326 | 7,463 |

恢复动作本身与 n 基本无关（约 1,137 cycles），n=32 的 total 主要来自 resume。
B2 对 CE 的收益是“不重放被纠正的读者”：excluded resume 从 1,137 降到 333。
UE 下 B1/B2 相同，符合语义。

P2-F1 的 fault overhead 以同 key 的 B0 归一化，并按 D 分面。page/codeword 在
W1/W2/W3 均为 3%–9%；bank 在 D=4096 时 W1 为 37.1%、W2 为 28.2%。

## 9. 依赖传播（Fix-7）

### 9.1 W4 扇出与晚报告

W4 为 ADD → fan-out MUL → RELU，K2 的每个 T_i 被 F 个 U 读取。P2-F7 在
n=32、page、UE、B1、AUTO 下：

| workload/F | D/T=0.1 | 0.5 | 1.0 | 2.0 | 3.0 |
|---|---:|---:|---:|---:|---:|
| W2 (F=1 等价) | 1.56% | 1.56% | 2.60% | 3.12% | 3.12% |
| W4_F1 | 1.56% | 1.56% | 2.60% | 3.12% | 3.12% |
| W4_F4 | 2.97% | 2.97% | 7.05% | 9.29% | 9.38% |
| W4_F16 | 3.02% | 7.27% | 18.28% | **32.93%** | **34.38%** |

受影响占比随 D 增大而上升，F 越大上升越快。W4_F16 在 D/T=3 时约 34% 的 tile
操作进入恢复集；W2/W4_F1 只有约 3%。B0 的 fault overhead 均值约 50,755 cycles，
作为灰色参考线；B1 在低 D 时远低于 B0，但在 D/T=3 时 W4_F16 的 fault overhead
达到约 30,404 cycles，接近 B0 的量级。

### 9.2 E2E-8

W2 n=8，F2 注入 A_2，B1，transport_delay=6,200 / 8,260：

- D/T=0.445：affected={ADD_2, MUL_2}，输出逐 bit 正确，fault overhead 2,725 cycles；
- D/T=0.593：affected={ADD_2, MUL_2, RELU_2}，输出逐 bit 正确，fault overhead 3,203 cycles；
- 两个 NO_REPLAY 对照输出均错误。

这是“报告晚到时，下游 kernel 已经读过坏数据，必须把 MUL/RELU 读者纳入重放”的
仿真器直接证据。v2 的 1–3% page 重放比例只能证明空间定位，不能证明依赖传播；
v2.1 的依赖传播结论只基于 P2-F7 和 E2E-8。

## 10. 验证口径（Fix-8）

### 10.1 F2 反事实

360 个 F2 sweep case 各配一个 `_norep` run：

| 分类 | 数量 | 占比 | Wilson 95% CI |
|---|---:|---:|---:|
| RECOVERY_NEEDED_AND_DONE | 200 | 55.6% | [50.4%, 60.7%] |
| MASKED | 160 | 44.4% | [39.4%, 49.6%] |
| FAIL | 0 | 0% | — |

**44.4% 的 F2 注入被 FP16 舍入掩盖**：NO_REPLAY 时输出仍然逐 bit 正确。因此
“恢复后输出正确”不能单独证明恢复有效；v2.1 把 `_norep` 配对作为必测项。F1(CE)
的 case 单独列为“读路径已纠正”，不计入恢复正确率。

### 10.2 分 kernel 标定与 P2-F6

标定只使用 L3 无故障 run（n=8/16/32），验证集为全部正常恢复 case（W1 370、
W2 363 个 case，NO_REPLAY 排除）。P2-F6：

| workload | recovery excl. resume MAPE | fault overhead MAPE |
|---|---:|---:|
| W1 | 19.1% | 16.1% |
| W2 | 21.2% | **11.9%** |

v2 的 W2 验证 MAPE 22.43% 使用的是 ADD 成本；v2.1 分 kernel 后，W2 fault overhead
MAPE 降到 11.9%，但 recovery excl. resume 仍有 21.2%，说明报告/ACK 时刻与
resume 边界的建模仍有误差。

### 10.3 B0 标签

`policy==B0` 的 case 状态写 `BASELINE_B0`，不再参与 P2Status 统计；P2-F5 不含 B0。
E2E-2 的 B0 用例从 `WHOLE_JOB_FALLBACK` 改为 `BASELINE_B0`，但恢复语义不变。

## 11. 半径策略（Fix-9）

P2-F4 改为“重放成本 + 两种示意分布下的期望覆盖率”，不再画 0/100 的同义反复曲线：

| policy | 重放比例 | MIX-A 期望覆盖 | MIX-B 期望覆盖 |
|---|---:|---:|---:|
| B2 | 0% | 0% | 0% |
| B2R-0 | 5.3% | 20% | 10% |
| B2R-1 | 14.5% | 55% | 35% |
| B2R-2 | 22.0% | 59% | 50% |
| B0 | 100% | 61% | 60% |

MIX-A 为 same_row/±1/±2/far = 0.20/0.70/0.08/0.02，MIX-B 为
0.10/0.50/0.30/0.10；两者都是 **ASSUMED 示意分布**，不对应任何特定 DRAM 器件。
B2R-1 在近邻为主的 MIX-A 下以 14.5% 的重放拿到 55% 期望覆盖，是较均衡的点；
far 距离仍只能靠 B0。

## 12. P2-T2 新旧对照

| 指标 | v2 | v2.1 |
|---|---|---|
| DELTA_FULL_1.0 状态 | PARTIAL_WITH_ESCAPE 30/30 | SELECTIVE_RECOVERED 30/30 |
| DELTA_SHORT_0.5（page）状态/输出 | SOURCE_REPAIRED_ONLY，输出错误 | SELECTIVE_RECOVERED，正确 30/30 |
| DELTA_SHORT_0.5（bank）状态/输出 | 未测 | 21/30 SELECTIVE 输出错误，9/30 fallback 正确 |
| W1 bank B1 重放比例 | 46.4% | 9.55%（D 平均） |
| W1 bank B1 / B0 | 104.3%（recovery） | 18.1%（fault overhead） |
| E2E-3 restore_bytes / affected | 65,536 B / 5 tile | 32,768 B / 3 tile |
| DEP_RELEASE Δ=0 开销 | −36% | 0.01%（无负值） |
| W2 page UE 受影响占比 | 1.04%（D 平均） | 0.35% / 1.39% / 1.39% at D=16/256/4096 |
| W2 验证 MAPE | 22.43%（recovery） | 11.89%（fault overhead）/ 21.16%（recovery excl. resume） |
| F2 中 MASKED 比例 | 未统计 | 160/360 = 44.4% |
| B0 状态 | WHOLE_JOB_FALLBACK | BASELINE_B0 |
| L2 case 数 | 47,946（报告）/ 48,666（实际） | 59,706 |
| L3 run 数 | 745 | 1,120 |

v2 报告中的 47,946 与 736/736 是旧口径/旧产物；v2.1 不再沿用这两个数字。

## 13. 结论

1. **安全发布可以做到“报告到达即冻结”**：严格 `>` + 同周期报告优先，使
   `DELTA_FULL` 和同周期释放不再逃逸。E2E-11 与 U16–U18 给出 L1/L3 证据。
2. **page/codeword 用 BIRTH 是正确选择**：短 Δ 下 page 30/30 正确；bank 仍依赖 Δ，
   21/30 静默错误是明确的粒度局限，不应被“SELECTIVE_RECOVERED”标签掩盖。
3. **bank 报告的 `R_read` 过滤有效但不够**：W1 bank B1 从 46.4% 降到 9.55%，
   E2E-10 恢复到 32 KiB / 3 tile；D=4096 时仍有 25.5% 重放，bank 报告不能提供
   细粒度空间定位。
4. **恢复动作很便宜，续跑才是大头**：W1 n=32 B1 的 excl-resume 只有 1,137 cycles，
   resume 6,326 cycles。优化重点应放在 checkpoint/resume，而不是继续压缩
   restore/replay。
5. **CE 与 UE 分流有效**：B2 在 CE 下把 excl-resume 从 1,137 降到 333；UE 下
   B1/B2 相同，符合“UE 必须重放读者”的语义。
6. **依赖传播必须用晚报告证明**：P2-F7 显示 W4_F16 在 D/T=3 时 34.4% 的 tile
   操作受影响；E2E-8 在仿真器中看到 MUL_2/RELU_2 进入 affected。
7. **验证必须配反事实**：44.4% 的 F2 注入被 FP16 舍入掩盖；只看“恢复后正确”
   会高估恢复收益。
8. **发布策略不再有负开销**：DEP_RELEASE 在任何 Δ 下 ≥ 0，且 ≤ GLOBAL_HOLD；
   PIM 模式互斥是必须写进模型的约束。

## 14. 局限

- 结果来自模型内实现，不代表真实 HBM-PIM 硬件。
- 一次报告对应一个损坏位置的假设；多损坏位置会产生多条报告，或属于漏检，由半径
  策略处理。
- W4 的 K2 成本属于 ASSUMED（复用 MUL fit）；MIX-A/MIX-B 是示意分布。
- bank 粒度仍依赖 Δ 的准确性；`DELTA_SHORT_BANK` 的 21/30 静默错误就是证据。
- L2 的 DEP_RELEASE 与 E2E-9 的 end-to-end 口径不同（后者不含 host 读成本），
  图中用独立 L3 点标出。
- 只考虑单 channel；host 拷贝、互连和 OS 开销未建模。
- 原地重放依赖“无原地更新、无 buffer 复用”的假设；违反时降级为整 job。
- 重放区间合并在当前矩阵中没有触发，只有 U23/U24 的单元测试覆盖。
- 注入的是一次性故障，不涉及持续攻击或 Rowhammer 多轮累积。

## 15. 复现

```bash
cd /opt/data/data/workspace-dynamic/Rowhammer/pim-phase2
JOBS=$(nproc) bash scripts/phase2/reproduce_phase2.sh --version v21
```

v2.1 顺序：

1. `v21_calibrate.py`（读取 E2E-0 `kernel_timing.csv`）
2. `v21_run_l2.py`
3. `v21_expand_l3.py` + `v21_run_l3.py`
4. `v21_validate_e2e.py`
5. `v21_counterfactual.py`
6. `v21_f6.py`
7. `v21_aggregate.py` + `v21_plot.py`
8. `v21_build_validation.py`

主要产物：

- L2：`results/phase2/v21/l2/cases.csv`、`matrix_summary.json`
- L3：`runs/phase2/v21/l3/`、`results/phase2/v21/l3/run_metrics.csv`
- 图源：`results/phase2/v21/figure_data/`
- 图：`figures/phase2/v21/`（PDF/SVG/PNG，无灰度）
- 统计：`results/phase2/v21/statistics.csv`
- 验证：`results/phase2/v21/validation.json`
- 对照表：`figures/phase2/v21/p2t2_comparison.md`

图对应：P2-F1 粒度×策略，P2-F2 恢复分解（resume 分离），P2-F3 发布开销，
P2-F4 半径成本/期望覆盖，P2-F5 状态分布，P2-F6 L2/L3 验证，P2-F7 晚报告。
P2-T1 账本表，P2-T2 新旧对照表。

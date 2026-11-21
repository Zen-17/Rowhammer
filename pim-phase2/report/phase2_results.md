# HBM-PIM Phase 2 结果报告：计算侧依赖追踪与选择性恢复

本报告对应 Phase 2 实施规范 v2。所有数字均由仓库内脚本在 Linux 服务器上实际运行产生；
L2 为标定后的迹驱动模型，L3 为 PIMSimulator 内的端到端执行。图中标注 `model (L2)`
的结论不代表真实 HBM-PIM 硬件测量。

## 1. 摘要

Phase 2 建立了三层闭环：

- **L1**：C++ 与 Python 两份等价恢复引擎，使用同一组手工账本用例 U1–U15。
- **L2**：以 Phase 1 实测标定的时间/成本模型，对 W1/W2/W3 做 47,946 个 case 的扫参。
- **L3**：在 PIMSimulator 中真实执行“报告→排空→分析→恢复输入→选择性重放→续跑→发布→逐 bit 验证”，覆盖 E2E-0 至 E2E-7 与 720 个扫参 case。

关键结果：

| 项目 | 结果 |
|---|---|
| Phase 1 回归 | `regress_before` 240/240；`regress_after` 240/240；`REGRESSION=PASS` |
| L1 用例 | Python 16/16（U1–U15 + whole-job）；C++ 16/16；地址向量 3,000 行逐行一致 |
| L2 主矩阵 | 45,360/45,360 主矩阵 case 输出正确；全矩阵 47,946 行 |
| L2 选择性（UE, n=32, page） | W1：B1 重放 2.1%，恢复延迟为 B0 的 23.5%；W2：1.0%，38.9% |
| L3 验收 | E2E 断言 61/61；745 个 L3 run 全部对账通过；DIFF 736/736 |
| L3 扫参 | 720/720 SUCCESS；输出逐 bit 一致，恢复期间新报告 0 |
| L2→L3 外推 | W1 拟合 MAPE 16.29%，W2 留出验证 MAPE 22.43% |
| 最终断言 | `results/phase2/validation.json` 113/113 PASS |

## 2. 实现结构

### 2.1 账本与数据结构

- 账本单位是 **tile 操作**：一次 kernel 对一个 tile 的完整处理，覆盖 16 个 bank、128 个 32 B burst（4 KiB，正好一个 host page）。
- `DataRef=(operand, tile, version)`；初始数据 version=0，tile 操作写出的版本等于 `op_id`。
- 物理足迹统一为 `BurstKey=(channel, rank, bank, row, burst_col)`。page 报告解码为 128 个 BurstKey，codeword 报告为 1 个，bank 报告为整 bank。
- 账本校验“无原地更新、无 buffer 复用”；任一违反触发 `WHOLE_JOB_FALLBACK / IN_PLACE_UNSUPPORTED`。
- 引擎只读取账本、`Phase1PublicReport`、引擎配置、checkpoint 可用性和 gate 状态。`scripts/phase2/check_isolation.sh --require-all` 对 12 个引擎文件做禁用词 grep，结果 `ISOLATION=PASS`。

### 2.2 恢复策略

| 代号 | 语义 |
|---|---|
| B0 | Phase 1 G7 整 job：恢复全部初始输入并整体重执行 |
| B1 | 依赖闭包选择性恢复，CE/UE 同等处理 |
| B2 | B1 + CE 只修复源数据，不重放读者 |
| B2R-k | B2 + CE 时把报告中每个 (bank,row) 扩展到 ±k 行，无时间下界 |
| B3 | B2 + `DEP_RELEASE`（本报告 L2 的发布实验使用该策略） |
| Oracle | 评估器内部知道真实故障位置时的最小重放集，仅作下界 |

发布策略：`GLOBAL_HOLD(H)` 在 job 结束 + H 统一发布；`DEP_RELEASE(Δ)` 对每个数据项按“最晚祖先完成时间 + Δ”发布。报告队列一溢出立即停止发布并走整 job 降级。

### 2.3 新增文件

C++：`Phase2Types.{h,cpp}`、`Phase2Layout.{h,cpp}`、`Phase2Ledger.{h,cpp}`、`Phase2Engine.{h,cpp}`、`Phase2Gate.{h,cpp}`、`Phase2Isolation.{h,cpp}`、`Phase2Checkpoint.{h,cpp}`、`tests/Phase2EngineTestCases.cpp`、`tests/Phase2E2ETestCases.cpp`。

Python：`phase2/layout.py`、`engine.py`、`workloads.py`、`timing.py`、`evaluate.py`、`tests/phase2/*`、`scripts/phase2/*`。

Phase 1 已有文件只改动了白名单中的四个：`Phase1Feeder.{h,cpp}`（新增完成观察者）和 `tests/PIMKernel.{h,cpp}`（新增 `executeEltwiseTiles`）。`diff -rq` 验证无其他已有文件被修改。

## 3. 实验设置

### 3.1 标定

`configs/phase2/calibration.json` 由 `scripts/phase2/calibrate.py` 生成：

| 参数 | 值 | 来源 |
|---|---:|---|
| `c_tile` | 475.1429 | `results/g7/summary.csv` 正例按 n_tiles 中位数拟合 |
| `c_fix` | 328.8571 | 同上 |
| `c_restore_per_byte` | 0.064819 | `restore_cycles` vs `restore_bytes` |
| `c_restore_fix` | 26.0 | 同上 |
| `c_quiesce` | 41.0 | `fig4_quiescence.csv` ACTUAL_CE 中位数 |
| `c_host_fix` / `c_host_per_tile` | 20.1429 / 692.8571 | `fig6_cost.csv` EXT_OFF HOST 拟合 |

参考值核对：`c_tile≈475`、`c_fix≈329`、`0.0648 B/cycle`、`26`、`41`，四项均在 ±1 内。

### 3.2 L2 矩阵

- workload：W1、W2、W3(K=4)、W3(K=16)
- n_tiles：8、32、128
- granularity：bank、page、codeword
- error_class：CE、UE
- 故障位置：W1 取 {A,X}，W2 取 {A,Y,T}，W3 取输入 0；tile 由 `(workload, operand, seed)` 确定性均匀选择
- D：16、256、4096；L=0；policy：B0、B1、B2、Oracle；seeds 0–29
- 半径实验 900 case，发布实验 2,160 case，状态实验 210 case，账本 36 case

L2 主矩阵共 45,360 case（3 个 workload 的 operand 数不同：W1 2、W2 3、W3 1）。

### 3.3 L3 矩阵

- E2E-0：W1/W2、n=8/32、无故障，记账开/关、观察者开/关、full/tiles/segments
- E2E-1/2：W1 n=8、page、F2(UE)、A 的 tile {0,3,7}，B1 与 B0
- E2E-3：W1 bank、F2、B1
- E2E-4：W1 page、F1(CE)、B2
- E2E-5：W2 n=8、T 的 tile 2、F2、B1
- E2E-6/7：NO_RESTORE、CHECKPOINT_MISSING(A_3)
- 扫参：W1/W2 × n {8,32} × granularity {bank,page} × F1/F2 × tile {first,middle,last} × policy {B0,B1,B2} × seeds 0–4 = 720 case

## 4. L1 结果

Python `tests/phase2/test_engine.py`：16 passed（U1–U15 + `plan_whole_job` 确定性）。
C++ `Phase2Engine.*`：16/16 通过。两侧对同一组账本、报告和配置产出的 `decision.json`
在 L3 的 736 个有决策 run 上逐字节一致（DIFF）。

地址编码：C++ 导出 3,000 行 `(addr,ch,rank,bank,row,col)`，Python `Scheme8Codec` 逐行解码/重编码一致。

## 5. L2 结果

### 5.1 P2-F1 选择性

UE、n_tiles=32、page 报告（mean over 30 seeds × 3 D × fault operands）：

| workload | policy | 重放 tile 比例 | 恢复延迟（B0 归一化） |
|---|---|---:|---:|
| W1 | B0 | 100% | 1.000 |
| W1 | B1/B2 | 2.08% | 0.235 |
| W1 | Oracle | 2.08% | 0.235 |
| W2 | B0 | 100% | 1.000 |
| W2 | B1/B2 | 1.04% | 0.389 |
| W2 | Oracle | 1.04% | 0.389 |
| W3(K=4) | B0 | 100% | 1.000 |
| W3(K=4) | B1/B2 | 2.08% | 0.162 |

bank 粒度下 W1 的 B1 重放比例 46.4%，恢复延迟为 B0 的 104.3%：依赖闭包把同 bank 的
所有已完成生产者都纳入再生集，选择性重放被拆成多个 run，甚至略贵于整 job 重执行。
这与“bank 报告空间上覆盖所有 tile，只能靠时间窗缩小范围”的预期一致；Oracle 在同样
bank 报告下只需 1.4% tile、24.0% 延迟。codeword 与 page 在当前布局下空间选择性相同。

### 5.2 P2-F2 延迟分解

W1、page、mean over 30 seeds × fault operands（cycles）：

| n | error | policy | quiesce | restore | replay | resume | publish | total |
|---:|---|---|---:|---:|---:|---:|---:|---:|
| 8 | CE | B0 | 41 | 4274 | 4130 | 0 | 0 | 8445 |
| 8 | CE | B1 | 41 | 292 | 536 | 1396 | 0 | 2264 |
| 8 | CE | B2 | 41 | 292 | 0 | 1396 | 0 | 1728 |
| 8 | UE | B1/B2 | 41 | 292 | 536 | 1396 | 0 | 2264 |
| 32 | UE | B0 | 41 | 17018 | 15533 | 0 | 0 | 32592 |
| 32 | UE | B1/B2 | 41 | 292 | 536 | 6803 | 0 | 7671 |
| 32 | CE | B2 | 41 | 292 | 0 | 6803 | 0 | 7135 |

B2 对 CE 的收益来自“不重放被纠正的读者”；UE 下 B1/B2 相同。剩余的大头是续跑
（本次报告较早，job 尚未跑完），说明恢复动作本身不是主要成本。P2-F2 中的菱形是
可用的 L3 实测值（W1 n=8 的 B0 UE、B1 UE、B2 CE）；n=32 没有对应的 L3 恢复用例，
因此不画菱形，避免把模型值伪装成实测。

### 5.3 P2-F3 发布开销

GLOBAL_HOLD 的 L2 曲线复现了 P1-F 的量级：

- W1 n=1：Δ/T=0.25 时 13.25%，Δ/T=1.0 时 53.0%
- W1 n=8：10.66% / 42.61%；P1-F 实测 11.01% / 42.69%
- 取 n=1/8 均值：11.95% vs 参考 10.21%（+1.74 pp）；47.80% vs 43.96%（+3.84 pp）

两项都在 ±5 pp 内，因此没有调参贴合。DEP_RELEASE 在 Δ=0 时出现约 -36% 的“负开销”：
host 在 kernel 级 producer barrier 之前就开始逐 tile 读取，这是模型的早发布收益；该
行为来自 `DEP_RELEASE` 的定义，不代表硬件实测。

### 5.4 P2-F4 半径 Pareto

W1、n=32、CE 报告 + 1 个静默 CE 级错误，30 seeds：

| 距离类别 | B2 | B2R-0 | B2R-1 | B2R-2 | B0 |
|---|---:|---:|---:|---:|---:|
| same_row 覆盖率 | 0% | 100% | 100% | 100% | 100% |
| ±1 覆盖率 | 0% | 0% | 100% | 100% | 100% |
| ±2 覆盖率 | 0% | 0% | 0% | 100% | 100% |
| far 覆盖率 | 0% | 0% | 0% | 0% | 100% |
| same_row 重放比例 | 0% | 5.3% | 14.5% | 22.0% | 100% |

B2R-0 已经覆盖 same_row（整行扩展），B2R-1/2 覆盖相邻行；far 只能靠 B0。成本随半径
单调增加，形成可用的 Pareto 前沿。静默距离分布是 ASSUMED 参数，不是实测分布。

### 5.5 P2-F5 状态分布

30 seeds 下每个场景均为单一状态（100%）：

| 场景 | 状态 |
|---|---|
| NORMAL_UE | SELECTIVE_RECOVERED |
| CE | SOURCE_REPAIRED_ONLY |
| DELTA_SHORT_0.5 | SOURCE_REPAIRED_ONLY（最终输出错误） |
| DELTA_FULL_1.0 | PARTIAL_WITH_ESCAPE |
| CHECKPOINT_MISSING | OUTSIDE_WINDOW_NO_TRUSTED_INPUT |
| LEDGER_TRUNCATED | WHOLE_JOB_FALLBACK |
| REPORT_OVERFLOW | WHOLE_JOB_FALLBACK |

`DELTA_SHORT_0.5` 的窗口下界晚于首次坏读，读者没有被选为种子；源数据被修复，但已经
写出的输出仍然错误。`DELTA_FULL_1.0` 能恢复最终输出，但消费者在报告前已经读走旧版本，
因此状态是 `PARTIAL_WITH_ESCAPE`。

### 5.6 P2-T1 账本开销

每个 tile 条目估计 59 字节（8+4+2+4+16+1）加 8 字节/每个输入输出引用。代表性行：

| workload | n | entries | bytes | peak held GLOBAL | peak held DEP(Δ=0) |
|---|---:|---:|---:|---:|---:|
| W1 | 8 | 8 | 472 | 8 | 1 |
| W1 | 32 | 32 | 1,888 | 32 | 1 |
| W2 | 8 | 24 | 1,352 | 24 | 1 |
| W2 | 32 | 96 | 5,408 | 96 | 1 |
| W3(K=16) | 128 | 128 | 21,888 | 128 | 1 |

`DEP_RELEASE(Δ=0)` 把 peak held 降到 1 个数据项；`GLOBAL_HOLD` 需要持有整个 job 的全部
产出。Δ=0.25T 时 W1 n=32 的 peak 为 9，W2 n=32 为 25。

## 6. L3 结果

### 6.1 E2E-0 与钩子自检

| case | producer cycles | ledger entries | 输出 |
|---|---:|---:|---|
| W1 n=8 tiles | 4,130 | 8 | bit exact |
| W1 n=8 记账关 | 4,130 | 0 | bit exact |
| W1 n=8 observer 关 | 4,130 | 8（计划条目） | bit exact |
| W1 n=8 full | 4,130 | 8 | bit exact |
| W1 n=8 3 段 | 4,957 | 8 | bit exact |
| W1 n=32 | 14,171 | 32 | bit exact |
| W2 n=8 | 10,257 | 24 | bit exact |
| W2 n=32 | 36,540 | 96 | bit exact |

记账开/关、观察者开/关、full/tiles 的周期完全一致；3 段调用多付两次固定开销，但输出
逐 bit 相同。这与 §8.2/§8.3 的自检要求一致。

### 6.2 E2E-1 至 E2E-7

| 编号 | 观测 | 关键数字 |
|---|---|---|
| E2E-1 k=0 | SELECTIVE_RECOVERED，affected={ADD_0} | restore 4096 B，replay 1 tile，recovery 4,151 cycles |
| E2E-1 k=3 | affected={ADD_3} | recovery 3,015 cycles |
| E2E-1 k=7 | affected={ADD_7} | recovery 1,084 cycles |
| E2E-1 NO_REPLAY | 仅 tile k 与 reference 不一致 | mismatch_tiles=1，其余 tile 一致 |
| E2E-2 k=3 | WHOLE_JOB_FALLBACK | restore 65,536 B，replay 8 tiles，recovery 8,397 cycles |
| E2E-3 k=3 bank | affected 5 tiles | restore 65,536 B，recovery 9,191 cycles |
| E2E-4 k=3 CE | SOURCE_REPAIRED_ONLY | replay 0，restore 4,096 B，恢复后读 A_3 新 CE=0 |
| E2E-5 W2 T_2 | affected={ADD_2, MUL_2} | restore 0，replay 2 tiles，recovery 6,590 cycles |
| E2E-6 NO_RESTORE | 重放期间新 UE 报告 1 条 | 不发布，`recovery_failed=true` |
| E2E-7 checkpoint 缺 A_3 | OUTSIDE_WINDOW_NO_TRUSTED_INPUT | replay 0，B_3 POISONED，host 读被拒绝 |

E2E-2 的 B0 恢复 65,536 B（A+X 共 16 个 tile）并整体重执行；E2E-3 的 bank 报告因为
扫描全部 tile 而恢复全部 16 个输入，恢复代价与 B0 相同但重放更碎，最终仍逐 bit 正确。
E2E-6 证明“恢复输入”不可省略：跳过恢复写后，重放重新读到坏 A_3 并产生新 UE 报告，
输出不发布。E2E-7 证明没有可信输入时不重放、不发布，依赖输出进入 POISONED。

### 6.3 扫参与差分

720 个 sweep case 全部 SUCCESS：`published=true`、`output_bit_exact=true`、
`reports_during_recovery=0`，没有 `NO_TRUSTED_INPUT`。加上 25 个验收 case，L3 共
739 SUCCESS + 6 EXPECTED_NEGATIVE，无 FAIL/TIMEOUT/MISSING。

DIFF：736 个带 `decision.json` 的 run 全部由 Python 引擎基于 `ledger.csv +
reports.csv + params + decision_inputs.json` 重算，并与 C++ `decision.json` 逐字节一致。
9 个 E2E-0 无故障 run 没有报告，因此没有决策文件。

## 7. L2 预测 vs L3 实测（P2-F6）

只用 W1 的 L3 无故障 kernel 时间拟合 `c_tile`/`c_fix`：

- `c_tile = 418.375`，`c_fix = 783.0`（W1 n=8 4,130；n=32 14,171）
- W1 拟合集 MAPE = 16.29%
- W2 留出验证 MAPE = 22.43%

W2 的误差是系统性的：模型把 MUL/RELU 的每 tile 成本当成 ADD 的成本，而 L3 实测的
MUL/RELU kernel 明显更便宜（W2 n=8 总周期 10,257，低于 3×4,130=12,390）。该偏差
没有用 W2 数据回调参数，而是作为模型局限保留；这也正是 Phase 2 规范要求 L3 修正
MUL/RELU 成本的原因。

## 8. 讨论

1. **page/codeword 已经足够**：在当前布局下，page 与 codeword 报告都精确落到一个 tile；
   B1/B2 的重放比例约 1–2%，远低于 B0。bank 报告空间上不可区分，B1 的选择性主要来自
   时间窗，甚至可能因 run 碎片化而比 B0 更贵。
2. **CE 与 UE 分流有效**：B2 在 CE 下把“修复源数据”和“重放读者”分开，W1 n=8 的恢复
   从 2,264 降到 1,728 cycles；UE 下 B1 与 B2 相同，符合“UE 必须重放读者”的语义。
3. **半径扩展是可调的保守性**：B2R-0/1/2 用 5.3%/14.5%/22.0% 的重放代价覆盖
   same_row/±1/±2；far 只能靠 B0。该权衡适合作为后续策略参数。
4. **发布策略影响稳态开销**：GLOBAL_HOLD 的模型与 P1-F 在 ±5 pp 内一致；
   DEP_RELEASE 可以通过早发布隐藏部分 kernel 固定开销，但代价是消费者可能读到尚未
   被报告覆盖的版本；这正是 `PARTIAL_WITH_ESCAPE` 需要单独度量的原因。
5. **降级路径是闭环的一部分**：checkpoint 缺失、账本截断、报告溢出、依赖信息不完整
   都有明确状态和可执行行为，不是静默跳过。

## 9. 局限

- 结果来自模型内实现，不代表真实 HBM-PIM 硬件。
- 注入的是一次性故障，不涉及持续攻击或 Rowhammer 多轮累积。
- 漏检位置/距离分布是 ASSUMED，仅作为参数扫描。
- L2 的时间/成本模型由 Phase 1 标定；MUL/RELU 成本在 L3 修正前属于 ASSUMED，W2
  验证 MAPE 22.43% 反映了这一点。
- 只考虑单 channel；host 拷贝、互连和 OS 开销未建模。
- 原地重放依赖“无原地更新、无 buffer 复用”的假设；违反时降级为整 job。
- Δ 上界依赖报告队列不溢出；溢出时立即停止发布并降级。
- L2 的 DEP_RELEASE 负开销来自早于 kernel 级 barrier 的发布模型，应视为上界收益。
- bank 粒度报告在当前布局下空间上不可区分，选择性只来自时间窗。

## 10. 复现

```bash
cd "$P2"     # /opt/data/data/workspace-dynamic/Rowhammer/pim-phase2
JOBS=$(nproc) bash scripts/phase2/reproduce_phase2.sh
```

`$P2` 是 Phase 2 根目录，`$P1` 是共享的 `../pim-phase1`。脚本会先调用
`scripts/phase2/build_phase2.sh`，把 `$P1/simulator` 复制到
`$P2/build/simulator`，覆盖 `$P2/overlay/src` 后构建；之后按 Stage 0→7
顺序执行，并在 `logs/phase2/` 留下每步日志。最终检查：

```bash
.venv/bin/python scripts/phase2/build_validation.py
cat results/phase2/validation.json
```

## 11. 交付物索引

- 引擎：`simulator/src/Phase2*`、`phase2/*.py`
- L1 测试：`simulator/src/tests/Phase2EngineTestCases.cpp`、`tests/phase2/test_engine.py`
- L2：`configs/phase2/l2_suite.yaml`、`results/phase2/l2/cases.csv`、`results/phase2/figure_data/`
- L3：`configs/phase2/l3_suite.yaml`、`configs/generated/phase2/l3-manifest.json`、`runs/phase2/l3/`
- 图：`figures/phase2/`（PDF/SVG/PNG/gray PNG、captions、plot_manifest）
- 统计：`results/phase2/statistics.csv`
- 验证：`results/phase2/validation.json`
- 源码差异：`environment/phase2_source.patch`
- 决策记录：`docs/phase2_decisions.md`；状态：`docs/phase2_status.md`

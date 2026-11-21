# HBM-PIM Phase 2 修正交接文档 v2.1（增量规范）

> **给执行 Agent 的说明**
>
> - 这是对 Phase 2 v2 实现的**增量修正**。v2 规范（`phase2_agent_implementation_plan.md`）中未在本文提到的内容**全部保持不变**。
> - v2 已经跑通：L1 16/16、L3 745 个 run、DIFF 736/736、回归 PASS。**不要重写已有模块**，只按本文逐条修改。
> - 本文按 F0 → F7 的顺序执行。每个阶段结束后停下来，用 §9 的模板汇报。
> - 遇到本文没覆盖的情况：选更保守的一方，记入 `docs/phase2_decisions.md`（编号从 `V21-01` 开始），然后继续。
> - 同一问题尝试 3 次失败：在 `docs/phase2_status.md` 标 `BLOCKED`，附命令、日志和最小复现，然后跳到下一个不依赖它的修正项。

---

## 0. 背景：为什么要修（用一段话理解）

v2 的结果证明了三件事：
- 能把错误定位到单个 tile；
- 只重做这个 tile 就能修好输出；
- 救不了的情况会明确降级，不会静默跳过。

但还有几处问题会改变结论：
1. 提前发布在 Δ 设置正确时仍会让输出被读走（安全漏洞）；
2. 提前发布的模型允许 host 在 PIM 计算期间读输出，而仿真器里做不到（收益被高估）；
3. “恢复延迟”把本来就要做的剩余工作也算了进去（比较不公平）；
4. 报告来得太快，下游操作还没开始，依赖传播几乎没被测到；
5. bank 粒度下可疑范围过大，导致比整体重来还贵；
6. Δ 设短时出现“状态看似成功、输出实际错误”的静默失败；
7. 验证口径有漏洞：CE 的 case 无论恢复与否都逐 bit 正确；按 kernel 类型的标定也不干净。

---

## 1. 路径与目录约定

- `$P2` = Phase 2 根目录（v2 中为 `pim-phase2`），`$P1` = 共享的 Phase 1 仓库（`$P2/../pim-phase1`）。以下路径默认相对于 `$P2`。
- C++ 源文件以 F0 记录的实际位置为准（v2 报告中写的是 `overlay/src/`，构建产物在 `build/simulator/sim`）。
- **v2.1 的产物全部写入新的版本目录，不覆盖 v2：**

| 类别 | v2.1 位置 |
|---|---|
| L2 结果 | `results/phase2/v21/l2/` |
| L3 运行与结果 | `runs/phase2/v21/l3/`、`results/phase2/v21/l3/` |
| 图源数据 | `results/phase2/v21/figure_data/` |
| 图 | `figures/phase2/v21/` |
| 统计与断言 | `results/phase2/v21/statistics.csv`、`results/phase2/v21/validation.json` |
| 报告 | `report/phase2_results_v21.md` |
| 配置 | `configs/phase2/v21/l2_suite.yaml`、`configs/phase2/v21/l3_suite.yaml`、`configs/phase2/v21/calibration.json` |
| 日志 | `logs/phase2/v21/` |

- 不得新增依赖；不得写绝对路径（沿用 v2 的规定）。

---

## 2. F0：预检（只读诊断，不改代码）

逐项检查，结果写入 `docs/phase2_v21_preflight.md`：

| 编号 | 检查内容 | 怎么查 | 通过标准 |
|---|---|---|---|
| F0-1 | `regress_after` 用的是不是 Phase 2 构建出的二进制 | 查 `runs/phase2/regress_after/run_manifest.json` 和 runner 日志里实际执行的 `sim` 路径，并比对它与 `build/simulator/sim` 的 SHA256 | 是 `$P2/build/simulator/sim`。如果是 `$P1/simulator/sim`，标记“回归门禁无效”，F2 结束时用正确的二进制重跑 |
| F0-2 | L3 中 n=32 的恢复 case 是否存在 | 在 `results/phase2/l3/summary.csv` 中筛选 `n_tiles==32 && fault_kind in {F1,F2}` | 写出实际行数。如果 >0，说明 v2 报告 §5.2 中“n=32 没有对应 L3 恢复用例”的说法有误，查明画 P2-F2 时为什么过滤掉了这部分数据 |
| F0-3 | B0 在结果里的状态标签 | 查 L3 中 `policy==B0` 的 `status` 列，以及 `p2f5_status.csv` 是否包含 B0 | 记录现状（v2 中 E2E-2 显示为 `WHOLE_JOB_FALLBACK`） |
| F0-4 | L2 中“发布冻结”发生在什么时刻 | 读 `phase2/timing.py` 和 `phase2/evaluate.py` 中 DEP_RELEASE 与报告处理的代码 | 写明发布是在报告到达时冻结，还是在 ACK 时冻结；同一周期内是先发布还是先处理报告；释放条件用的是 `>=` 还是 `>` |
| F0-5 | 恢复延迟的统计口径 | 读 L2/L3 中 `recovery_cycles` 的计算代码 | 写明是否包含 resume（续跑） |
| F0-6 | C++ 与 Python 的实际文件路径 | `find . -name 'Phase2*' -o -path './phase2/*.py'` | 列出全部路径，后续以此为准 |

退出条件：6 项全部有书面结论。

---

## 3. 修正项（逐条执行）

每条修正项都按同一结构写：问题（白话）→ 必须实现的改动 → 新增测试 → 受影响的图。
**C++ 引擎与 Python 引擎必须同步修改**，改完后 DIFF 测试必须继续 100% 一致。

### Fix-1：提前发布的安全漏洞（最高优先级）

**问题**：v2 的 `DELTA_FULL_1.0` 场景中，Δ 已经设成真实上界，消费者却在 30/30 个 seed 中都在报告到达前读走了旧版本。

**必须实现**（三条同时满足）：
1. **释放条件用严格大于**：数据项 d 在周期 t 可以释放，当且仅当 `t > t_last_ancestor_complete(d) + Δ`。
2. **同周期先处理报告**：每个周期内的固定顺序为：①投递报告并交给 gate → ②gate 执行冻结 → ③处理释放 → ④处理消费者读取。L2 的事件排序和 L3 的循环顺序都必须遵守。
3. **报告到达即冻结**：gate 收到任何报告的那个周期，立刻停止一切释放（所有 HELD 保持 HELD），**不等 ACK**。冻结持续到本次恢复完成。

**Δ 的定义写进代码注释和报告**：Δ 必须覆盖“某次读检测到错误”到“gate 收到该报告”之间的最大间隔。L3 中 `Δ = transport_delay + capacity * service_interval + 1`；L2 中 `Δ_true = D`（单故障、无排队）。

**新增测试**（Python 与 C++ 都要有）：
- U16：报告周期 = 释放周期 → 该数据项保持 HELD；
- U17：报告周期 = 释放周期 − 1 → 保持 HELD；
- U18：报告在 ACK 之前到达 → 报告到达到 ACK 之间，没有任何数据项被释放。

**预期**：L2 状态实验中，`DELTA_FULL_1.0` 变为 `SELECTIVE_RECOVERED` 30/30（如果不是，记录实际结果并排查，不要调参贴合）。

**受影响的图**：P2-F3、P2-F5。

### Fix-2：时间窗下界（消除静默失败）

**问题**：`DELTA_SHORT_0.5` 中，Δ 设得比真实延迟短，真正读到坏数据的操作落在时间窗之外，结果是输出错误、状态却是 `SOURCE_REPAIRED_ONLY`。

**必须实现**：引擎配置新增 `window_mode ∈ {DELTA, BIRTH, AUTO}`，默认为 `AUTO`。对每个可疑数据项 d，单独计算时间窗下界 `ws(d)`：
- `DELTA`：`ws(d) = max(birth(d), report_cycle − Δ)`（防下溢）；
- `BIRTH`：`ws(d) = birth(d)`；
- `AUTO`：page 和 codeword 粒度用 BIRTH，bank 粒度用 DELTA。

`birth(d)`：INITIAL 数据为 0；恢复写写回的数据为该次恢复写的完成周期；op 产出的数据为该 op 的 `t_complete`。时间窗上界仍为 `t_ack`。

**理由（写进报告）**：page/codeword 粒度下，每个 tile 在一个 kernel 内通常只被读一次，用 BIRTH 几乎不增加成本，却完全不依赖 Δ 的准确性。bank 粒度覆盖范围太大，仍需要 Δ 来收窄。

**新增测试**：
- U19：page 粒度 + AUTO 时，读者的读取发生在 `report_cycle − Δ` 之前，仍被选为种子；
- U20：bank 粒度 + AUTO 时，行为与 v2 相同（DELTA）。

**新增状态实验场景**：`DELTA_SHORT_0.5` 分别在 page 和 bank 粒度下运行。预期：page → `SELECTIVE_RECOVERED` 且输出正确；bank → 如实记录结果（可能仍会失败，这正是 bank 粒度的局限）。

**受影响的图**：P2-F5。在 W4 上额外比较 DELTA 与 BIRTH 的重放成本（见 Fix-7）。

### Fix-3：收紧可疑数据项（修 bank 粒度）

**问题**：v2 把报告区域内的所有数据项都当作可疑。bank 区域覆盖全部 tile，于是恢复了全部 64 KiB 输入，并重算了所有已完成的生产者（E2E-3 中 affected = 5 个 tile、restore 65,536 B，耗时 9,191 周期，比 B0 的 8,397 还长）。

**原理**：报告一定是由某次读产生的，所以坏数据只可能位于“时间窗内被读过”的数据项中。

**必须实现**：把 v2 §5.3 步骤 4 中的 `R` 换成：
```text
R_read = { d ∈ R : 存在一次对 d 的读（op 的输入读，或账本中记录的 host 读），
                   且读时间区间 [t_issue, t_complete] 与 [ws(d), t_ack] 有交集 }
```
- 步骤 3（种子）、步骤 4（恢复/再生）、步骤 6（外部输入判定）中的 `R` 全部改为 `R_read`。
- 半径扩展部分 `R_k` 保持 v2 的定义不变（它本来就是为没有被检测到的静默错误准备的）。
- **安全兜底**：如果 `R_read` 为空（报告来自账本没有记录的读，例如未记账的 host 读），退回 v2 的规则（使用整个 R），`fallback_reasons` 中加入 `UNLOGGED_READER`。
- host 对输出的读取必须记入账本，作为一种读事件：`reader = HOST`，时间为读完成周期。
- **假设（写进报告）**：一次报告对应一个损坏位置；多个损坏位置会产生多条报告，或者属于漏检，由半径策略处理。

**新增测试**：
- U21：bank 报告，时间窗内只有 A_2 被读过 → `R_read = {A_2}`，restore = {A_2}，affected 只含 A_2 的读者；
- U22：`R_read` 为空 → 退回整个 R，并带 `UNLOGGED_READER`。

**新增 L3 用例 E2E-10**：W1 n=8，bank 粒度，F2 注入 A_3，B1。断言：
- 输出逐 bit 一致；
- `restore_bytes < 65536`；
- 记录 affected 的大小。

**预期**：L2 中 bank 粒度下 B1 的重放比例从 46.4% 明显下降，接近 Oracle（1.4%）。如果没有下降，排查原因并记录，不要调参。

**受影响的图**：P2-F1、P2-F2。

### Fix-4：合并重放区间

**问题**：每个连续的重放区间都要单独付一次 kernel 固定开销（L3 实测约 783 周期），区间太碎时总成本反而更高。

**必须实现**：同一个 kernel 内，相邻两个重放区间之间相隔 `g` 个 tile。当以下三个条件同时满足时合并成一个区间，中间的 tile 一起重放：
- `g * c_tile(kernel) < c_fix(kernel)`；
- 中间 tile 的所有输入都可信（不在 `R_read ∪ R_k` 中）；
- 中间 tile 的输出状态都是 HELD（不是 PUBLISHED 或 CONSUMED）。

合并引入的额外 tile 记入新指标 `coalesced_extra_tiles`，**不计入 affected**。`decision.json` 新增 `replay_runs` 字段，内容为合并后的区间列表 `[kernel, tile_begin, tile_count]`。

**新增测试**：
- U23：区间 {1} 和 {3}，c_fix > c_tile → 合并为 [1, 3]，`coalesced_extra_tiles = 1`；
- U24：中间 tile 2 的输出已 PUBLISHED → 不合并。

**受影响的图**：P2-F1、P2-F2。

### Fix-5：发布模型加入 PIM 模式互斥

**问题**：L2 假设 host 能在 PIM kernel 运行期间逐 tile 读取输出，因此算出了 −36% 的开销和 peak held = 1。在仿真器里，rank 处于 HAB_PIM 模式时 host 访问无法完成（Phase 1 G7 的 NO_STATE_RESET 对照证明了这一点）。

**必须实现（L2）**：
- host 只消费**最终输出**（W1 的 B、W2 的 O、W4 的 O），中间数据不被 host 消费；
- 最终输出 d 的实际可读时间：
  ```text
  t_readable(d) = max( t_release(d), t_exit(最后一个 kernel) )
  ```
  其中 `t_release` 由发布策略决定（GLOBAL_HOLD 或 DEP_RELEASE），`t_exit` 是最后一个 kernel 退出 PIM 模式的周期；
- host 读按 tile 串行，每个 tile 的成本取 calibration 中的 `c_host_per_tile`，固定开销取 `c_host_fix`；
- `peak_held_items` 按“已产出但尚未可读”的数据项数统计。

**预期**：DEP_RELEASE 的开销在任何 Δ 下都 ≥ 0，并且 ≤ GLOBAL_HOLD；收益来自让扣留时间与 kernel 尾部重叠。出现负值即为 bug。

**新增 L3 用例 E2E-9**：W1，n_tiles ∈ {8, 32}，release ∈ {GLOBAL_HOLD, DEP_RELEASE}，Δ ∈ {0.25T, 1.0T}（T 取该 n 下无故障 kernel 的周期数），无故障，host 通过 gate 逐 tile 读取。记录 `end_to_end_cycles` 和 `peak_held_items`。断言：
- 输出逐 bit 一致；
- 在 L3 中 DEP_RELEASE 的 `end_to_end_cycles` ≤ GLOBAL_HOLD。

**受影响的图**：P2-F3、P2-T1。

### Fix-6：主指标改为“故障带来的额外周期”

**问题**：v2 的 `recovery_cycles` 包含 resume（续跑），而续跑的工作在无故障时本来就要做；B0 的续跑为 0，因为整体重跑已经把它包含了。所以 B1 与 B0 的比值不公平。E2E-1 中 k=0/3/7 的恢复耗时 4,151/3,015/1,084，主要差别就是续跑工作量不同。

**必须实现**：
- 新增主指标：
  ```text
  fault_overhead_cycles = T_end(有故障, policy) − T_end(无故障, 同 workload/n_tiles/seed)
  T_end = job 开始到所有最终输出都可读（PUBLISHED）的周期
  ```
  L2 中直接计算；L3 中每个有故障的 case 都要配对一个无故障 run（相同的 workload、n_tiles、seed、release 策略；可复用 E2E-0 的结果）。
- 保留 v2 的分解字段（quiesce/restore/replay/resume/publish），另新增 `recovery_cycles_excl_resume`。
- 所有汇总表**按 D 分组**报告，不再跨 D 求平均。
- 新增字段 `reader_partial_at_ack`（bool）：读者 op 在 ACK 时尚未完成、因而进入续跑集而不是重放集。L2 和 L3 都要输出。

**受影响的图**：P2-F1（改用 `fault_overhead_cycles`）、P2-F2（resume 单独标注，并从 total 中剔除）。

### Fix-7：让依赖传播真正被测到

**问题**：
- 报告延迟（最大 4,096 周期）远小于 kernel 时长（n=32 时约 15k 周期），报告到达时下游 kernel 往往还没开始，前向闭包几乎总是空的；
- v2 的 W3 只有扇入（一个输出读多个输入），没有扇出，所以影响范围不会变宽。

结果是 W2 的重放比例只有 1.04%，W3(K=4) 与 W1 完全相同。

**必须实现**：

(a) **新工作负载 W4（仅 L2，带扇出）**：三个 kernel，tile 数为 n：
```text
K1: T_i = A_i + X_i                      (i = 0..n-1)
K2: U_j = f(T_j, T_{j+1}, ..., T_{j+F-1}) (下标取 mod n；每个 T_i 被 F 个 U 读取)
K3: O_j = relu(U_j)
```
- F ∈ {1, 4, 16}；
- K2 的单 tile 成本 = `c_tile(MUL)`，属于 ASSUMED，写进 captions；
- W4 也必须满足“无原地更新、无 buffer 复用”。

(b) **晚报告扫参（L2）**：
- workload ∈ {W2, W4(F=1), W4(F=4), W4(F=16)}；n_tiles = 32；error_class = UE；granularity = page；
- 故障位置：A（输入）和 T（中间数据）；
- `D / T_K1 ∈ {0.1, 0.5, 1, 2, 3}`，其中 `T_K1 = c_fix(ADD) + n * c_tile(ADD)`；
- policy ∈ {B0, B1, Oracle}；window_mode ∈ {AUTO, DELTA}；seeds 0..29；
- 输出：受影响 op 占比、`fault_overhead_cycles`、`reader_partial_at_ack`。

**预期现象**（用于自查，不得调参贴合）：受影响占比随 D 增大而上升；F 越大上升越快；存在某个 D，使 B1 与 B0 的 `fault_overhead_cycles` 曲线交叉或接近。

(c) **L3 晚报告用例 E2E-8**：W2，n_tiles = 8，F2 注入 A_2，page 粒度，B1，`transport_delay ∈ {T_ADD, 2*T_ADD}`（T_ADD 为 E2E-0 中实测的 ADD kernel 周期），Δ 按 Fix-1 的公式同步设置。断言：
- `affected` 中包含 MUL_2（报告到达时 MUL kernel 已经执行过 MUL_2）；
- 输出逐 bit 一致；
- NO_REPLAY 负面对照下输出不一致；
- DIFF 一致。

这是依赖传播在仿真器里的直接证据。

**受影响的图**：新增 P2-F7。

### Fix-8：验证口径

(a) **按故障类型分开统计正确率**：
- F1(CE) 的 case 单独列为“CE 已在读路径纠正，正确是平凡的”，不计入恢复正确率；
- 对 L3 扫参中的每个 F2 case，新增一个配对的 `negative_control=NO_REPLAY` run（目录名后缀 `_norep`），并按下表分类：

| 分类 | 判定 |
|---|---|
| `RECOVERY_NEEDED_AND_DONE` | NO_REPLAY 时输出错误，且正常恢复后逐 bit 正确 |
| `MASKED` | NO_REPLAY 时输出也正确（FP16 舍入掩盖了错误） |
| `FAIL` | 正常恢复后输出仍然错误 |

  写入 `results/phase2/v21/l3/f2_counterfactual.csv`，并在报告中给出三类的计数和 Wilson 置信区间。

(b) **按 kernel 类型标定，并干净地划分标定集与验证集**：
- **标定集只用无故障 run**：在 L3 中新增 n_tiles = 16 的无故障 W1、W2 run（加上已有的 8 和 32，共 3 个点）。每个 run 记录每个 kernel 的开始/结束周期（新增 `kernel_timing.csv`，列为 `kernel_idx, kernel_type, t_start, t_end, n_tiles`）。
- 对 ADD、MUL、RELU 分别用 3 个点做最小二乘拟合，得到 `c_fix_k, c_tile_k`，同时报告残差。如果 3 个点明显不共线（相对残差 > 5%），在报告中说明 kernel 时间对 n 不是线性的。
- restore 成本仍沿用 G7 的标定（这是独立来源）。
- **验证集**：L3 中全部有故障的恢复 case（W1 和 W2），比较 L2 预测与 L3 实测的 `recovery_cycles_excl_resume` 和 `fault_overhead_cycles`，分别报告 MAPE。
- 写入 `configs/phase2/v21/calibration.json`，每个参数都注明来源。

(c) **B0 标签**：`policy == B0` 的 case，`status` 字段写 `BASELINE_B0`，不参与 P2Status 统计；P2-F5 中排除 B0。

**受影响的图**：P2-F6，以及报告中的验证表。

### Fix-9：P2-F4 改为期望覆盖率

**问题**：v2 中半径的定义恰好对应距离类别，所以覆盖率只有 0% 和 100%，属于构造出来的结果。

**必须实现**：
- 成本部分：保留各策略（B2、B2R-0/1/2、B0）的重放比例，改为表格 + 柱状图（n_tiles = 32）。
- 覆盖率部分：在两种**示意性**距离分布下计算期望覆盖率：

| 分布 | same_row | ±1 | ±2 | far |
|---|---:|---:|---:|---:|
| MIX-A（近邻为主） | 0.20 | 0.70 | 0.08 | 0.02 |
| MIX-B（远距较多） | 0.10 | 0.50 | 0.30 | 0.10 |

  期望覆盖率 = Σ（分布权重 × 该距离类别下的覆盖率）。
- caption 中写明：这两种分布是 ASSUMED 示意分布，不是实测，也不对应任何特定 DRAM 器件。

**受影响的图**：P2-F4。

---

## 4. 新增与修改的测试汇总

**L1**（Python 与 C++ 各一份，原有 U1–U15 必须继续通过）：

| 编号 | 对应修正 | 断言摘要 |
|---|---|---|
| U16 | Fix-1 | 报告周期 = 释放周期 → HELD |
| U17 | Fix-1 | 报告周期 = 释放周期 − 1 → HELD |
| U18 | Fix-1 | 报告到 ACK 之间没有任何释放 |
| U19 | Fix-2 | page + AUTO：早于 report−Δ 的读者仍被选为种子 |
| U20 | Fix-2 | bank + AUTO：与 v2 行为一致 |
| U21 | Fix-3 | bank 报告：`R_read` 只含时间窗内被读过的数据项 |
| U22 | Fix-3 | `R_read` 为空 → 退回整个 R，并带 `UNLOGGED_READER` |
| U23 | Fix-4 | 满足条件的区间被合并，`coalesced_extra_tiles` 正确 |
| U24 | Fix-4 | 中间 tile 已发布时不合并 |

**L3 新增用例**：

| 编号 | 对应修正 | 断言摘要 |
|---|---|---|
| E2E-8 | Fix-7 | 晚报告：MUL_2 ∈ affected；逐 bit 一致；NO_REPLAY 时输出错误；DIFF 一致 |
| E2E-9 | Fix-5 | 发布策略：逐 bit 一致；DEP_RELEASE 的 end_to_end ≤ GLOBAL_HOLD |
| E2E-10 | Fix-3 | bank 粒度：逐 bit 一致；`restore_bytes < 65536` |
| E2E-11 | Fix-1 | 构造报告周期与某数据项释放周期相同：该数据项未被 host 读取 |
| E2E-12 | Fix-2 | page 粒度，Δ 配置为真实值的一半：状态为 SELECTIVE_RECOVERED，逐 bit 一致 |

v2 的 E2E-0 至 E2E-7 必须在 v2.1 代码下重新运行并全部通过。由于 Fix-3/Fix-4 的修改，E2E-3 的 restore_bytes 和 affected 可能变化，按新行为更新断言，**并在报告中列出新旧值对比**。

**DIFF**：v2.1 的全部 L3 run 都要通过差分测试。`decision.json` 中新增的字段（`R_read`、`replay_runs`、`coalesced_extra_tiles`、`window_mode`、每个数据项的 `ws`）同样参与比对。

---

## 5. 实验矩阵变化

**L2**（`configs/phase2/v21/l2_suite.yaml`）：
- 主矩阵沿用 v2，但所有输出新增 `fault_overhead_cycles`、`recovery_cycles_excl_resume`、`reader_partial_at_ack`、`coalesced_extra_tiles`，并新增 `window_mode` 维度（AUTO 为主，DELTA 为对照，仅在 bank 和 page 粒度下比较）；
- 新增晚报告扫参（Fix-7(b)）；
- 发布实验按 Fix-5 重跑；
- 状态实验加入 Fix-1、Fix-2 的场景；
- 半径实验按 Fix-9 改为输出期望覆盖率。

**L3**（`configs/phase2/v21/l3_suite.yaml`）：
- v2 的 720 个扫参 case 重跑；
- 每个 F2 case 配一个 `_norep` run；
- 新增 n_tiles = 16 的无故障标定 run；
- E2E-8 至 E2E-12；
- 每个有故障的 case 都配对一个无故障 run，用于计算 `fault_overhead_cycles`（可复用）。

---

## 6. 图表修改

图表样式完全沿用 v2 §11.2，输出到 `figures/phase2/v21/`。

| 编号 | v2.1 内容 | 相对 v2 的变化 |
|---|---|---|
| P2-F1 | 报告粒度 × 策略：(a) 受影响 tile 占比，(b) `fault_overhead_cycles`（相对 B0 归一化）；按 D 分面或分组 | 指标改为额外周期；bank 粒度反映 Fix-3/Fix-4 后的结果；Oracle 保留 |
| P2-F2 | 恢复分解堆叠柱：resume 用浅色斜线单独标出，并加注“无故障时本来就要做的工作”；total 不含 resume | 口径修正；n=32 加上 L3 菱形点（按 F0-2 的核查结果） |
| P2-F3 | 稳态开销随 Δ/T 变化：GLOBAL_HOLD vs DEP_RELEASE（含模式互斥）；叠加 E2E-9 的 L3 实测点 | 不再出现负开销 |
| P2-F4 | (a) 各策略的重放比例，(b) MIX-A/MIX-B 下的期望覆盖率 | 去掉同义反复的 0/100 图 |
| P2-F5 | 状态分布，含 DELTA_SHORT（page/bank）、DELTA_FULL、同周期场景；不含 B0 | 场景按 Fix-1/Fix-2 更新 |
| P2-F6 | L2 预测 vs L3 实测：标定只用无故障 run，验证集为全部恢复 case；分别给出 `recovery_cycles_excl_resume` 和 `fault_overhead_cycles` 的 MAPE | 标定集与验证集的划分方式改变 |
| **P2-F7（新增）** | 晚报告实验：(a) 受影响 op 占比 vs D/T_K1，(b) `fault_overhead_cycles` vs D/T_K1；每条曲线对应一个 workload/F，B0 用灰色水平参考线；叠加 E2E-8 的 L3 点 | 新增，用于证明依赖追踪的作用 |
| P2-T1 | 账本开销表：peak held 按 Fix-5 的口径重新计算 | 修正 DEP 列 |
| **P2-T2（新增）** | v2 → v2.1 关键数字对照表（见 §8） | 新增 |

---

## 7. 执行顺序与退出条件

| 阶段 | 内容 | 退出条件 |
|---|---|---|
| F0 | 预检（§2） | 6 项都有书面结论 |
| F1 | Python 引擎与 L2：Fix-1、2、3、4、6，以及 U16–U24 | Python 端 U1–U24 全部通过；L2 状态实验中 DELTA_FULL_1.0 为 SELECTIVE 30/30（否则记录并排查） |
| F2 | C++ 引擎同步修改，C++ 端 U16–U24 | C++ 端 U1–U24 全部通过；Phase 1 回归在**正确的二进制**上 PASS（如果 F0-1 判定无效，在这里补跑） |
| F3 | L2：Fix-5、Fix-7(a)(b)、Fix-9，全部 L2 扫参 | L2 全部产出；发布实验中 DEP_RELEASE 的开销无负值 |
| F4 | L3：E2E-0 至 E2E-12、n=16 标定 run、`kernel_timing.csv` | 全部断言通过；DIFF 100% 一致 |
| F5 | L3 扫参 + `_norep` 配对 + 无故障配对 | 对账结果无 FAIL/TIMEOUT/MISSING；生成 `f2_counterfactual.csv` |
| F6 | Fix-8(b) 标定与验证、P2-F6 | 两项 MAPE 均已报告 |
| F7 | 统计、图表、报告 | §8 中的所有交付物齐全 |

**降级**：F4/F5 被阻塞时，F3 的 L2 结果和图照常完成；报告中标注缺失的 L3 项和阻塞原因。E2E-8（依赖传播的仿真器证据）和 E2E-11（同周期安全）优先级最高，被阻塞时要单独说明。

---

## 8. 报告要求（`report/phase2_results_v21.md`）

1. 开头写一段白话摘要：v2.1 修了什么，结论有哪些变化。
2. **P2-T2 新旧对照表**，至少包含以下行（新值由运行得出）：

| 指标 | v2 | v2.1 |
|---|---|---|
| DELTA_FULL_1.0 的状态 | PARTIAL_WITH_ESCAPE 30/30 | |
| DELTA_SHORT_0.5（page）的状态与输出 | SOURCE_REPAIRED_ONLY，输出错误 | |
| bank 粒度 W1 B1 的重放比例 | 46.4% | |
| bank 粒度 W1 B1 / B0 | 104.3%（恢复延迟） | （改用 fault_overhead） |
| E2E-3 的 restore_bytes / affected | 65,536 B / 5 tile | |
| DEP_RELEASE 在 Δ=0 时的开销 | −36% | |
| W2 page UE 的受影响占比（按 D 分列） | 1.04%（跨 D 平均） | |
| W2 留出验证的 MAPE | 22.43% | |
| F2 case 中 `MASKED` 的比例 | 未统计 | |

3. 每条结论都要注明来源是 L2（模型）还是 L3（仿真器）。
4. “依赖追踪”的论述只能基于 P2-F7 和 E2E-8；不得再用 v2 中 page 粒度下 1–3% 的重放比例来论证依赖追踪（那部分证明的是空间定位）。
5. 局限部分在 v2 基础上补充：
   - 一次报告对应一个损坏位置的假设；
   - W4 的成本属于 ASSUMED；
   - MIX-A/MIX-B 是示意分布；
   - bank 粒度仍依赖 Δ 的准确性。

**交付物**：§1 表中的全部 v2.1 目录；更新后的 `environment/phase2_source.patch`；`results/phase2/v21/validation.json`（覆盖 U1–U24、E2E-0 至 E2E-12、DIFF、回归和 counterfactual 的全部断言，格式与 v2 相同）；更新 `scripts/phase2/reproduce_phase2.sh`，新增 `--version v21` 参数。

---

## 9. 每个阶段的汇报模板

```text
阶段 Fx 结果：PASS / BLOCKED
改动文件：<相对 $P2 的路径；C++ 与 Python 分开列>
自检命令与结果：<命令> -> <关键输出>
关键数字：<例如 U1–U24 24/24；DELTA_FULL_1.0 SELECTIVE 30/30；E2E-10 restore 4096 B>
与 v2 不同的数字：<旧值 -> 新值>
新增决策：docs/phase2_decisions.md V21-xx
未解决问题：<无 / 阻塞点 + 日志路径>
```

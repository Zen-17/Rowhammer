# HBM-PIM Phase 2（计算侧依赖追踪与选择性恢复）工程实施规范 v2

> **使用说明**：本规范面向编码执行 Agent。所有架构决策、数据结构、边界规则、验收断言、实验矩阵和图表规范均已固化。**严格按第 10 节的阶段顺序执行；不要自行发明启发式；不要改动白名单以外的 Phase 1 代码。**
>
> 本版目标是**逻辑闭环**，不是细节完美：从“报告到达”到“输出重新发布并逐 bit 验证”的每一步都必须有实现、有测试、有数据；参数精细化留给后续版本。
>
> 实验在 Linux 服务器上运行。本文档可以放在任意位置；**文中所有路径都相对于 Phase 1 仓库根目录 `$P1`**（即同时包含 `simulator/Sconstruct`、`scripts/run_suite.py`、`README.md` 的目录）。

---

## 0. 路径与运行环境约定（服务器）

1. **确定 `$P1`**：Stage 0 第一步执行
   ```bash
   export P1="$(cd <仓库所在目录>/pim-phase1 && pwd)"
   test -f "$P1/simulator/Sconstruct" && test -f "$P1/scripts/run_suite.py" && echo P1_OK
   ```
   之后所有命令都在 `cd "$P1"` 下执行。
2. **代码中禁止写绝对路径**。Python 脚本一律使用 `ROOT = Path(__file__).resolve().parents[N]`（与 Phase 1 的 `scripts/run_g7.py` 相同的写法）；C++ fixture 只从环境变量 `PHASE2_CONFIG`、`PHASE2_OUT` 和 params 文件获取路径。写入 CSV/JSON 的路径一律用相对于 `$P1` 的相对路径。
3. **Phase 1 manifest 内含旧机器绝对路径**：`configs/generated/*.json` 中的 `device_ini`、`system_ini`、`output_dir`、`out` 字段写死了 `/opt/data/data/workspace-dynamic/Rowhammer/pim-phase1`。如果 `$P1` 与之不同，**不要覆盖原文件**，而是用 Stage 0 新写的 `scripts/phase2/relocate_manifest.py` 生成重定位副本，写到 `configs/generated/phase2/` 下（只替换路径前缀，`run_id` 和所有科学参数保持不变）。
4. **工具链**：构建命令为 `cd simulator && scons NO_STORAGE=0 NO_LIBRARY=1 -j"$(nproc)"`，产物是 `simulator/sim`（gtest 可执行文件，需在 `simulator/` 目录下以 `./sim --gtest_filter=...` 运行）。Python 统一使用 `.venv/bin/python`。**不得新增依赖**，只能使用 `environment/requirements.lock.txt` 中已有的包（numpy、pandas、matplotlib、PyYAML、pytest）。
5. **并行度**：所有 runner 接受 `--jobs`，默认值为 `$(nproc)`；每个仿真 case 是一个独立进程、独立输出目录，并支持 `--resume`（跳过已存在且 `exit_code==0` 的目录）。
6. **文件格式**：新建的 shell 脚本必须是 LF 换行并带 `#!/usr/bin/env bash` 和 `set -euo pipefail`。
7. **一键复现入口**：最终交付 `scripts/phase2/reproduce_phase2.sh`，按 Stage 0→7 的顺序调用全部命令。

### 0.1 Phase 2 新增文件位置（全部相对 `$P1`）

| 类别 | 位置 |
|---|---|
| C++ 核心模块 | `simulator/src/Phase2Types.h`、`Phase2Layout.{h,cpp}`、`Phase2Ledger.{h,cpp}`、`Phase2Engine.{h,cpp}`、`Phase2Gate.{h,cpp}`、`Phase2Isolation.{h,cpp}`、`Phase2Checkpoint.{h,cpp}` |
| C++ 测试 | `simulator/src/tests/Phase2EngineTestCases.cpp`（单元测试），`simulator/src/tests/Phase2E2ETestCases.cpp`（端到端 fixture） |
| Python 参考引擎与评估器 | `phase2/__init__.py`、`phase2/layout.py`、`phase2/engine.py`、`phase2/workloads.py`、`phase2/timing.py`、`phase2/evaluate.py` |
| Python 测试 | `tests/phase2/test_*.py` |
| 脚本 | `scripts/phase2/*.py`、`scripts/phase2/*.sh` |
| 配置 | `configs/phase2/l2_suite.yaml`、`configs/phase2/l3_suite.yaml`、`configs/phase2/calibration.json`（自动生成）、`configs/generated/phase2/`（自动生成） |
| 运行目录 | `runs/phase2/stage0/`、`runs/phase2/regress_before/`、`runs/phase2/regress_after/`、`runs/phase2/l3/<case_id>/` |
| 结果 | `results/phase2/l2/`、`results/phase2/l3/`、`results/phase2/figure_data/`、`results/phase2/statistics.csv`、`results/phase2/validation.json` |
| 图 | `figures/phase2/` |
| 文档 | `docs/phase2_status.md`、`docs/phase2_decisions.md`、`docs/phase2_mount_points.md`、`report/phase2_results.md` |
| 日志 | `logs/phase2/<stage>_<step>.log` |

`simulator/Sconstruct` 会自动 glob `src/*.cpp` 和 `src/tests/*.cpp`，因此新增的 C++ 文件不需要修改构建脚本。

---

## 1. 执行铁律（Hard Constraints）

1. **Phase 1 零破坏**：
   - Phase 2 的全部逻辑放在新文件中，Phase 1 的 fixture 不感知 Phase 2。
   - 对 Phase 1 已有文件**只允许**以下两处“纯新增”修改（不改动任何已有函数体、默认参数或行为）：
     - `simulator/src/tests/PIMKernel.{h,cpp}`：新增 `executeEltwiseTiles(...)`（见 §8.2）；
     - `simulator/src/Phase1Feeder.{h,cpp}`：新增一个默认为空的只读完成观察者 `setCompletionObserver(...)`（见 §8.3）。
   - 硬门禁：Stage 0 跑出的 `regress_before` 与 Stage 4 跑出的 `regress_after` 必须逐 case 一致（`metrics.json` 中除墙钟/UTC 外的所有键，以及 `output.bin`、`reports.csv` 的 SHA256）。
2. **盲测隔离（防真值泄露）**：恢复引擎（C++ 的 `Phase2Layout/Ledger/Engine/Gate/Isolation`，Python 的 `phase2/layout.py`、`phase2/engine.py`）只能读取：账本、`Phase1PublicReport`、引擎配置、checkpoint 是否可用、输出门禁状态。**严禁**读取或引用 `truth.csv`、`reference.bin`、注错参数、期望输出数组。由 `scripts/phase2/check_isolation.sh` 用 grep 检查，上述文件中不得出现 `truth|reference|inject|golden|expected` 这些词（注释里也不行）。
3. **禁止伪造结果**：所有 CSV/JSON 中的数值都必须由实际运行产生；禁止硬编码 pass；禁止为了通过测试而修改测试、断言或阈值；失败的 case 如实写入 `failure_cases.csv`。
4. **停止规则**：同一个问题尝试 3 次仍失败时，在 `docs/phase2_status.md` 中记录 `BLOCKED`（附命令、日志路径、最小复现），然后按 §10 的降级路线继续推进，**不要无限重试，也不要静默跳过**。
5. **歧义规则**：遇到本文档未覆盖的情况，选择更保守（恢复范围更大、发布更晚）的一方，写入 `docs/phase2_decisions.md`（格式：编号 / 情境 / 选择 / 理由 / 影响的文件），然后继续推进。
6. **先测再走**：每个 Stage 结束时运行该 Stage 的自检命令，更新 `docs/phase2_status.md`，并按 §12 的模板汇报后再进入下一个 Stage。
7. **命名隔离**：Phase 1 已在 `namespace DRAMSim` 中定义了 `enum class RecoveryStatus`。Phase 2 的所有 C++ 符号必须放在 `namespace DRAMSim::phase2` 中；测试文件里的辅助函数放在匿名 namespace 中，并加 `p2` 前缀，避免与 Phase 1 测试文件中的同名静态辅助函数冲突。

---

## 2. Phase 1 事实表（已核实，Stage 1 只需逐条核对，不需要重新勘察）

| 主题 | 事实 | 出处 |
|---|---|---|
| 报告接口 | `Phase1Reports::tick(cycle)` 每周期最多投递一条；`reports()` 返回 `std::vector<Phase1PublicReport>`，字段为 `report_seq, report_cycle, error_class("CE"/"UE"), region_type("bank"/"page"/"codeword"), region_id`；另有 `overflow()`、`dropped()` | `simulator/src/Phase1Reports.{h,cpp}` |
| region_id 编码 | bank：`channel*num_banks + actual_bank`；page：`hostAddress/4096`；codeword：`(((ch*num_banks+bank)*num_rows+row)*burst_columns+burst_col)*4+cw` | `Phase1Reports.cpp::onDetected` |
| 报告队列构造 | `Phase1Reports(codec, num_channels, num_banks, granularity, transport_delay, service_interval, capacity, deliver_ce, deliver_ue)` | `Phase1Reports.h` |
| ECC 配置 | `bank.setPhase1(Phase1Profile::LOCAL_VISIBLE, &reports)`；LOCAL_VISIBLE 下 **CE 在读路径上被纠正**，PIM 拿到的是正确数据（P1-C output_changed=0）；UE 则交付未纠正数据 | `tests/Phase1G7TestCases.cpp` L430–433，`report/phase1_results.md` |
| 注错 | `Bank::injectRawDataBit(row, burstColumn, bitInBurst)`；F1（CE）= bit 15；F2（UE）= 同一 codeword 的 bit 0 和 bit 1 | `tests/Phase1G5TestCases.cpp` L323–327 |
| 暂停/排空 | `Phase1PauseController`：`notifyReport(report_cycle)`、`tick(cycle)`、`acked()`、`ackCycle()`、`request(cycle, reason)`；驱动循环可参考 `runFeederLoop` | `Phase1Pause.h`，`tests/Phase1G7TestCases.cpp` L181–210 |
| Feeder | `Phase1FeederMemory::freezeChannel/unfreezeChannel/clearPending/isIdle/pendingCount` | `Phase1Feeder.h` |
| 状态复位 | 中途暂停时 rank 处于 `HAB_PIM`，必须先 `PIMRank::resetState()` 回到 `SB`，host 恢复写才能完成（G7 的 NO_STATE_RESET 对照会超时） | `tests/Phase1G7TestCases.cpp` L480–495，`docs/g7_recovery.md` §5 |
| 恢复写 | `feeder->MultiChannelMemorySystem::addTransaction(true, addr, &burst)`，随后 `addBarrier(0)` 并调用 `runDirectWrites` | `tests/Phase1G7TestCases.cpp` L497–515 |
| W1 工作负载 | `preloadNoReplacement(&A,0,0)`、`(&X,128,0)`；`executeEltwise(dim, ALL_BANK, ADD, 0, 256, 128)`，其中 `dim = n_tiles*128` 个 burst，输出位于 row 256 | `tests/Phase1G7TestCases.cpp` L439–460 |
| tile 布局 | tile i 使用列 `c=8i..8i+7`（`addrGenSafe` 会循环把列溢出折算到 row），在 even/odd 两组 bank 上通过 ALL_BANK 广播访问全部 16 个 bank；**tile i 恰好等于 1 个 4 KiB host page**（Scheme8：page 号 = row*4 + col/8） | `tests/PIMKernel.cpp::computeAddOrMul`，`tests/KernelAddrGen.cpp` |
| 广播的 bank 身份 | 真实 bank 编号 = `pb*2 + packet->bank`；广播命令的 `packet->bank` 不是真实 bank | `docs/source_audit.md` §2 |
| 支持的 kernel | `executeEltwise` 支持 ADD、MUL、RELU；每个 kernel 内部包含 parkIn→SB→HAB→CRF 编程→HAB_PIM→计算→退出模式，固定开销不可省 | `tests/PIMKernel.cpp` L499–526 |
| 输出门禁 | `Phase1Runtime` 只跟踪**一个整体输出版本**；收到任何报告就锁住整个输出；处于 PUBLISHED 时拒绝恢复（`ALREADY_COMMITTED`） | `Phase1Runtime.h`，`docs/g5_runtime.md` |
| Checkpoint | `Phase1CheckpointStore` 只保存 W1 的 A/X 整体副本 | `Phase1Checkpoint.h` |
| Fixture 约定 | Phase 1 fixture 读取 `PHASE1_CONFIG/PHASE1_OUT`；runner 在 `cwd=simulator/` 下运行 `./sim --gtest_filter=<Fixture>.RunCase` | `scripts/run_g7.py` L283–313 |
| 标定数据 | `results/g7/summary.csv`（列 `restore_cycles, reexec_cycles, recovery_cycles, ack_cycle, old_report_cycle`）；`results/figure_data/fig4_quiescence.csv`；`results/figure_data/fig6_cost.csv`（列 `hold_cycles, overhead_fraction, end_to_end_cycles`） | — |
| 标定参考值 | n_tiles=1/8：restore 557/4274，reexec 804/4130，ACK→publish 1361/8404 cycles；quiesce 41；P1-F 中 H=0.25T 时开销 10.21%，H=1T 时 43.96% | `docs/g7_recovery.md`，`README.md` |

如果核对时发现与事实不符，记入 `docs/phase2_mount_points.md` 的“差异”一节，以代码为准，并在 decisions 中说明影响。

---

## 3. 总体架构：三层闭环

```text
L1  引擎（C++ 与 Python 两份等价实现）+ 合成账本单元测试
     │  差分测试保证两份实现给出相同决策
L2  Python 迹驱动评估器（时间/成本模型由 Phase 1 实测标定）→ 全部扫参与主图
     │  预测值 vs 实测值
L3  仿真器端到端：在 PIMSimulator 中真实执行“报告→排空→分析→恢复输入→选择性重放→续跑→发布→逐 bit 验证”
```

- **为什么要两份引擎**：Python 版是规范参考实现（易测、供 L2 大规模扫参使用）；C++ 版在仿真器中实时运行。L3 每个 run 会导出账本和决策，`tests/phase2/test_differential.py` 用 Python 引擎重算并要求结果完全一致。这与 Phase 1 用独立 Python 参考交叉验证 ECC 的做法相同。
- **降级路线**：如果 L3 被阻塞（§1 第 4 条），L1 + L2 仍然要完成全部图表，图中标注 “model (L2)”，并在报告中写明 L3 的阻塞点。L3 至少需要完成 E2E-1/E2E-2 才能声称“仿真器验证”。

---

## 4. 核心概念

### 4.1 账本单位：tile 操作（不是 DRAM 命令）

- **tile 操作** = 一个 kernel 对一个 tile 的完整处理（W1 中为该 tile 的 BANK_TO_GRF → ADD → GRF_TO_BANK，覆盖 even/odd 两组 bank）。
- 选这个粒度的原因：ALL_BANK 广播一条命令就访问 16 个 bank；GRF 依赖在内存区域层面不可见。tile 操作每次进入时都从内存重新加载 GRF，不跨条目携带 GRF/SRF 状态，因此**可以独立重做（幂等）**。
- 数据项 `DataItem = (operand, tile)`；每个 operand 有固定的 base row（布局表见 §4.5），不同 operand 不复用 buffer，也没有原地更新（由账本校验，见 §5.6）。
- 版本：初始预加载的数据版本为 `0`（INITIAL，由 checkpoint 保护）；tile 操作写出的数据版本 = 该操作的 `op_id`。

### 4.2 物理足迹（统一的空间比较基础）

- 足迹的最细单位是 `BurstKey = (channel, rank, bank, row, burst_col)`。
- `tile_footprint(operand, i)`：bank 取 0..15，`row = base_row(operand) + (8*i)//32`，`burst_col ∈ [(8*i)%32, (8*i)%32+7]`，共 128 个 BurstKey。
- `report_footprint(report)`：
  - bank：该 bank 的全部 row/col；
  - page：对 `[id*4096, (id+1)*4096)` 中每个 32 B 地址做 Scheme8 解码，得到 128 个 BurstKey；
  - codeword：按 §2 的公式反解出单个 BurstKey。
- 重叠判定 = 两个 BurstKey 集合的交集非空。**不得**使用“bank_id 不同就返回 false”这类捷径（page 报告不带 bank）。
- Python 的 Scheme8 编解码按 `docs/source_audit.md` §3 实现，并且必须与 C++ 的 `Phase1Address` 交叉验证：Stage 4 写一个 gtest 导出 ≥1000 个随机地址的 `(addr, ch, rank, bank, row, col)` 到 `results/phase2/l1/address_vectors.csv`，再由 pytest 逐行比对。
- 预期现象（不是 bug）：在 W1/W2 布局下，page 与 codeword 粒度的空间选择性相同（都是 1 个 tile）；bank 粒度在空间上覆盖所有 tile，只能依靠时间窗缩小范围。

### 4.3 输出生命周期（Phase 2 自己维护，按数据项）

`HELD → PUBLISHED → CONSUMED`；另有终态 `POISONED`（不可恢复）。
- Phase 2 fixture **不把报告交给** `Phase1Runtime::onReport`（它会锁住整个输出），而是交给 `phase2::Gate`。
- B0 基线完全复用 Phase 1 G7 的整 kernel 路径，作为对照。

### 4.4 发布策略

- `GLOBAL_HOLD(H)`（Phase 1 语义）：job 的全部输出在 `job_end + H` 时一起发布，安全条件为 `H ≥ Δ`。
- `DEP_RELEASE(Δ)`（Phase 2 新增）：数据项 d 的发布时间 = d 的所有祖先操作（含生产者）中最晚的 `t_complete` + Δ。理由：祖先的每一次读如果读到坏数据，必然在该次读时被检测，报告最晚在 Δ 后到达。
- 一旦报告队列 `overflow()==true`，立即停止一切发布（所有 HELD 保持 HELD），并让下一次分析走 WHOLE_JOB_FALLBACK。
- Δ 的默认值为 `transport_delay + capacity*service_interval + 1`（单 channel、单报告源情况下是保守上界），由配置注入，引擎不得自行推导。

### 4.5 工作负载

| 名称 | 定义 | 层级 |
|---|---|---|
| W1 | `B = A + X`；行布局 A=0，X=128，B=256 | L2 + L3 |
| W2 | `T = A + X`；`U = T * Y`；`O = relu(U)`；行布局 A=0，X=128，T=256，Y=384，U=512，O=640；三个 kernel 依次执行，前一个 kernel 完全空闲后才下发下一个 | L2 + L3 |
| W3 | 合成扇入 DAG：输出 tile j 依赖 K 个输入 tile（K ∈ {4,16}），用来展示依赖扩散变宽；成本沿用 W1 的单 tile 成本（ASSUMED） | 仅 L2 |

n_tiles 的上限受 128 行/operand 约束（≤512）；L3 使用 {8, 32}，L2 使用 {8, 32, 128}。

---

## 5. 数据结构与确定性算法（C++ 与 Python 字段一一对应）

### 5.1 类型

```cpp
namespace DRAMSim { namespace phase2 {

enum class Policy { B1_SELECTIVE, B2_CLASS_AWARE, B2R_RADIUS };
enum class ReleasePolicy { GLOBAL_HOLD, DEP_RELEASE };

enum class P2Status {           // 优先级从高到低，见 §5.7
    OUTSIDE_WINDOW_NO_TRUSTED_INPUT,
    WHOLE_JOB_FALLBACK,
    PARTIAL_WITH_ESCAPE,
    SELECTIVE_RECOVERED,
    SOURCE_REPAIRED_ONLY
};

enum class Lifecycle { HELD, PUBLISHED, CONSUMED, POISONED };

struct DataRef { uint16_t operand; uint32_t tile; uint64_t version; };  // version 0 = INITIAL

struct OpEntry {
    uint64_t op_id;          // 全局单调递增，即执行序
    uint32_t job_id;         // 一个工作负载实例 = 一个 job（W2 的三个 kernel 同属一个 job）
    uint16_t kernel_idx;     // job 内第几个 kernel
    uint32_t tile;
    std::vector<DataRef> inputs;
    std::vector<DataRef> outputs;
    uint64_t t_issue;        // 该 tile 第一个描述符被 feeder 提交的周期
    uint64_t t_complete;     // 该 tile 最后一个描述符完成的周期；未完成为 UINT64_MAX
    bool completed;
    bool dep_complete = true;
    bool is_replay = false;
};

struct EngineConfig {
    Policy policy; unsigned radius_rows;   // 仅 B2R_RADIUS 使用
    uint64_t delta_cycles; uint64_t max_log_entries;
};

struct Decision {                          // 必须能完整序列化为 JSON（键排序）
    P2Status status;
    uint64_t window_start, window_end;
    std::vector<DataRef> suspect_items;     // R（含半径扩展后的 R_k，分开列出）
    std::vector<uint64_t> seeds, regenerate, affected;   // 升序 op_id
    std::vector<DataRef> restore_list;      // 需从 checkpoint 恢复的 INITIAL 数据
    std::vector<DataRef> escaped_outputs;   // 已 CONSUMED 的受影响输出
    std::vector<uint64_t> remaining;        // 未完成/未开始、需续跑的 op
    std::vector<std::string> fallback_reasons;
};

struct P2Metrics {
    P2Status status; uint64_t total_logged_ops, ledger_bytes_est, peak_held_items;
    uint64_t affected_ops, replayed_tiles, replay_runs, restore_bytes, isolation_bytes;
    uint64_t quiesce_cycles, restore_cycles, replay_cycles, resume_cycles, publish_cycles;
    uint64_t recovery_cycles;      // ACK → 受影响输出全部重新发布
    uint64_t end_to_end_cycles;    // job 开始 → 全部输出发布
    uint64_t escaped_outputs, reports_during_recovery;
};
}}
```

`ledger_bytes_est` 按 ASSUMED 编码计算：`8 + 4 + 2 + 4 + 16 + 1 + 8*(|inputs|+|outputs|)` 字节/条。

### 5.2 记账 `record_issue / record_complete`

1. 发起 tile 操作时创建 `OpEntry`（分配 `op_id`），输入版本取各数据项的当前版本。
2. 由 feeder 完成观察者（§8.3）把描述符的 `(row, col, is_write)` 映射到 `(kernel, tile)`，更新 `t_issue`（首个）和 `t_complete`（最后一个）；全部描述符完成后置 `completed=true`，并把输出数据项的当前版本设为 `op_id`、Gate 状态设为 `HELD`。
3. 条目数超过 `max_log_entries` 时淘汰最旧条目，并置 `ledger_truncated=true`。
4. 更新 `total_logged_ops`、`ledger_bytes_est`、`peak_held_items`。

### 5.3 分析入口 `analyze(report, t_ack, config, ledger, gate, checkpoint)`

只在暂停 ACK 之后调用，此时没有在途事务。

**步骤 1：空间。** `F = report_footprint(report)`；`R = {d : tile_footprint(d) ∩ F ≠ ∅}`。若策略为 B2R_RADIUS 且 `error_class=="CE"`，则 `F_k` = F 中出现过的每个 (bank, row) 扩展到 `row±radius_rows` 的整行；`R_k = {d : footprint(d) ∩ F_k ≠ ∅} \ R`。

**步骤 2：时间窗。**
```cpp
window_start = (report.report_cycle > delta) ? report.report_cycle - delta : 0;
window_end   = t_ack;   // 注意：不是 report_cycle；报告到 ACK 之间的在途读同样可疑
```

**步骤 3：种子（按策略）。** `reads_in(o, S, [a,b])` 表示 o 的输入含 S 中的数据项，且 `[o.t_issue, o.t_complete] ∩ [a,b] ≠ ∅`。
- B1_SELECTIVE：`Seeds = {o completed : reads_in(o, R, [ws, we])}`（CE 与 UE 同等处理）。
- B2_CLASS_AWARE：UE 同 B1；CE 时 `Seeds = ∅`（CE 已在读路径纠正，只需修复源数据）。
- B2R_RADIUS：UE 同 B1；CE 时 `Seeds = {o : reads_in(o, R_k, [0, we])}`（邻域内的静默错误没有检测时间上界，因此时间窗下界取 0）。

**步骤 4：再生集（溯源，而不是直接判越界）。** 对每个 `d ∈ R ∪ R_k`：当前版本为 INITIAL 时，checkpoint 中有 d 就把 d 加入 `restore_list`，没有则记 `NO_TRUSTED_INPUT`；当前版本由某个 op 产出时，把该生产者加入 `Regenerate`。

**步骤 5：前向闭包。** `Affected = closure(Seeds ∪ Regenerate)`：只要已完成的 v 读取了 Affected 中某个 op 产出的版本，就把 v 加入。BFS 按 op_id 升序遍历，保证确定性。

**步骤 6：外部输入与回溯。** Affected 中各 op 的输入里，版本不是由 Affected 产出的，称为外部输入 e：
- e ∉ R ∪ R_k（不在可疑区）：可信，重放时直接读内存，不恢复；
- e ∈ R ∪ R_k 且为 INITIAL：已在步骤 4 处理（恢复，或判 `NO_TRUSTED_INPUT`）；
- e ∈ R ∪ R_k 且由 q ∉ Affected 产出：把 q 加入 Affected，回到步骤 5。

**步骤 7：降级检查。** 满足以下任一条件即为 WHOLE_JOB_FALLBACK，原因写入 `fallback_reasons`：`ledger_truncated`；Affected 中存在 `dep_complete==false`；报告队列 `overflow`；§5.6 的原地更新校验失败。此时 Affected = 所涉 job 的全部已完成 op，`restore_list` = 该 job 的全部 INITIAL 输入。

**步骤 8：续跑集。** `remaining` = 所有未完成（部分执行）或尚未开始的 op。它们**不进入 Affected**，在重放完成后重新下发，自然会读到修复后的数据。

**步骤 9：输出检查。** 受影响输出 = Affected 产出的最终输出项 ∪ R 中的最终输出项。其中状态为 CONSUMED 的写入 `escaped_outputs`；**仍然执行恢复**，只是状态记为 PARTIAL_WITH_ESCAPE。

### 5.4 执行 `execute(decision)`（L3 中由 fixture 调用 C++ 实现；L2 中由成本模型计算）

1. 如果状态是 NO_TRUSTED_INPUT：不重放；依赖缺失输入的输出标记为 POISONED（host 读被拒绝）；记录事件后结束。
2. `clearPending()` → `unfreezeChannel(0)` → `PIMRank::resetState()`（与 G7 相同）。
3. `isolation.isolate(F)`，累加 `isolation_bytes`。
4. 对 `restore_list` 发 host 恢复写（每个 tile 128 个 burst），然后 `runDirectWrites`。
5. 对 Affected 按 kernel 分组、按 tile 合并成连续区间（run），每个 run 调用一次 `executeEltwiseTiles`，并将对应条目标记为 `is_replay=true`。
6. 按原顺序重新下发 `remaining`（同样合并成连续区间）。
7. 原子发布：受影响输出在全部 run 完成后一次性进入可发布状态（再按发布策略释放）；未受影响的输出不被扣留。
8. 记录 `P2Metrics`；重放期间如果收到新报告，计入 `reports_during_recovery`，v1 中视为该 case 失败（正例中必须为 0）。

### 5.5 存储侧接口（MVP）

```cpp
class SourceIsolationPolicy {
public:
    virtual ~SourceIsolationPolicy() = default;
    virtual uint64_t isolate(const std::vector<BurstKey>& footprint) = 0;  // 返回隔离/搬运字节数
    virtual bool is_isolated(const BurstKey& key) const = 0;
};
class SimpleRegionIsolationPolicy : public SourceIsolationPolicy { /* 只做逻辑标记，字节数 = 足迹 burst 数 × 32 */ };
```
v1 不做重映射。源数据由 §5.4 第 4 步的合法写（重新编码 ECC）或重放写覆盖修复；本研究注入的是一次性故障。

### 5.6 原地重放的安全条件（替代“新版本槽位”）

v1 重放直接写回原地址，前提是以下条件成立，由账本校验：
- (a) 任何 op 的输入数据项与输出数据项不重合（无原地更新）；
- (b) 每个数据项在一个 job 内至多被写一次（无 buffer 复用）。

任一条件被违反 → WHOLE_JOB_FALLBACK，原因 `IN_PLACE_UNSUPPORTED`。在 (a)(b) 成立时，受影响输出的所有已完成读者都在 Affected 中，未完成的读者在 remaining 中，因此原地覆写不会丢失任何仍需要的数据。

### 5.7 状态优先级

`NO_TRUSTED_INPUT` > `WHOLE_JOB_FALLBACK` > `PARTIAL_WITH_ESCAPE` > `SELECTIVE_RECOVERED`（Affected 非空）> `SOURCE_REPAIRED_ONLY`（Affected 为空）。

---

## 6. 策略与基线

| 代号 | 含义 | 实现位置 |
|---|---|---|
| B0 | Phase 1 G7：整 job 恢复全部输入并整体重执行，整个输出被锁 | L2 成本模型；L3 中复用 G7 流程 |
| B1 | 依赖闭包选择性恢复，CE/UE 同等处理 | 引擎 |
| B2 | B1 + 按错误类别区分（CE 只修复源数据） | 引擎 |
| B2R-k | B2 + CE 时对邻域 ±k 行做无时间下界的扩展 | 引擎 |
| B3 | B2 + DEP_RELEASE 发布策略 | 引擎 + Gate |
| Oracle | 已知真实故障位置下的最小重放集，**只能出现在评估器中**，作为下界 | `phase2/evaluate.py` |

---

## 7. L2 迹驱动评估器（`phase2/timing.py`、`phase2/evaluate.py`）

### 7.1 标定（`scripts/phase2/calibrate.py` → `configs/phase2/calibration.json`）

从 Phase 1 结果中按 n_tiles 取正例的中位数，做两点线性拟合：
- `c_tile, c_fix`：由 `reexec_cycles(n) = c_fix + n*c_tile` 拟合（参考值约 475 / 329）；
- `c_restore_per_byte, c_restore_fix`：由 `restore_cycles` 对 `restore_bytes` 拟合（参考值约 0.0648 / 26）；
- `c_quiesce`：取 quiesce 中位数（参考值 41）。

每个参数都要记录来源文件、列名、样本数和拟合残差。MUL/RELU 沿用 ADD 的单 tile 成本，标记为 ASSUMED，待 L3 实测修正。

### 7.2 时间模型

- kernel 执行时间 = `c_fix + n*c_tile`。op (k,i)：`t_issue = kernel_start + c_fix/2 + i*c_tile`，`t_complete = t_issue + c_tile`（固定开销对半分摊，ASSUMED）。
- 注错：输入类故障在 job 开始前注入；中间数据类故障在其生产者完成后注入。检测时间 = 首个读者的 `t_issue + c_tile/2`；`t_report = t_detect + D`；`t_ack = t_report + L + c_quiesce`。
- `t_issue ≤ t_ack` 且 `t_complete ≤ t_ack` 的 op 视为已完成；`t_issue ≤ t_ack < t_complete` 的视为部分执行，放入 remaining。
- 恢复耗时 = `c_quiesce + c_restore_fix + restore_bytes*c_restore_per_byte + Σ_run (c_fix + tiles*c_tile)`；续跑耗时同理按 run 计算。
- B0 = 恢复整个 job 的全部 INITIAL 输入，并从头整体重执行（G7 语义）。
- 正确性判定（仅评估器内部使用）：最终输出正确，当且仅当所有真实读到坏数据的 op（UE、漏检故障）及其下游都被重放，且坏的源数据已被修复。

### 7.3 扫参矩阵（`configs/phase2/l2_suite.yaml`）

- workload ∈ {W1, W2, W3(K=4), W3(K=16)}；n_tiles ∈ {8, 32, 128}；
- granularity ∈ {bank, page, codeword}；error_class ∈ {CE, UE}；
- 故障位置：W1 取 {A, X}，W2 取 {A, Y, T}，tile 均匀随机；
- D ∈ {16, 256, 4096}；L = 0；policy ∈ {B0, B1, B2, Oracle}；seeds 0..29。
- 半径实验：CE 报告 + 一个静默漏检错误，漏检位置相对报告行的距离类别为 {same_row, ±1, ±2, far}（分布为 ASSUMED，以参数形式扫描）；policy ∈ {B2, B2R-0, B2R-1, B2R-2, B0}。
- 发布实验（无故障）：Δ/T_job ∈ {0, 0.05, 0.1, 0.25, 0.5, 1.0}；release ∈ {GLOBAL_HOLD(H=Δ), DEP_RELEASE(Δ)}；消费者为 host 按 tile 读取，每个 tile 一次。
- 状态分布实验，场景包括：正常 UE；CE；Δ 配置不足（Δ_cfg/Δ_true ∈ {0.5, 1.0}）导致消费早于报告；某 tile 的 checkpoint 缺失；`max_log_entries` 过小；报告队列溢出。

### 7.4 标定自检

用 `GLOBAL_HOLD` 复现 P1-F 中 H=0.25T 与 H=1T 的开销（10.21% / 43.96%）。消费者模型以 `docs/g8_paper_matrix.md` 为准。误差超过 ±5 个百分点时**不要调参去贴合**，把差异写入报告的“模型局限”。

---

## 8. L3 仿真器端到端

### 8.1 Fixture（`simulator/src/tests/Phase2E2ETestCases.cpp`，`Phase2E2EFixture.RunCase`）

- 结构参照 `Phase1G7Fixture.RunCase`；读取 `PHASE2_CONFIG`（params.ini）和 `PHASE2_OUT`；`runFeederLoop`、`runDirectWrites` 复制到匿名 namespace 并改名为 `p2RunFeederLoop`、`p2RunDirectWrites`。
- 主要参数：`workload, n_tiles, seed, granularity, transport_delay, service_interval, capacity, handler_delay, fault_kind(F0/F1/F2), fault_operand, fault_tile, fault_bank, policy(B0/B1/B2/B2R), radius_rows, release, delta_cycles, negative_control(NONE/NO_REPLAY/NO_RESTORE/CHECKPOINT_MISSING), watchdog_cycles, device_ini, system_ini`。
- 每个 run 的输出目录包含：`params.ini, metrics.json, reports.csv, ledger.csv, decision.json, output.bin, reference.bin, validation.json, stdout.log, stderr.log, exit_code`。其中 `reference.bin` 只由 fixture 末尾的评估代码写出和比较，引擎不得接触。
- `ledger.csv` 的列：`op_id,job_id,kernel_idx,tile,t_issue,t_complete,completed,dep_complete,is_replay,inputs,outputs`，inputs/outputs 编码为 `operand:tile:version;...`；另输出 `layout.csv`（`operand,base_row`），供差分测试使用。

### 8.2 新增 `PIMKernel::executeEltwiseTiles(KernelType k, int tile_begin, int tile_count, int in0_row, int res_row, int in1_row)`

- 与 `executeEltwise` 的流程完全相同，区别只有两点：CRF 程序使用 `num_jump = tile_count-1`；第 i 次循环的列为 `c = 8*(tile_begin+i)`（`addrGenSafe` 会循环折算到 row）。
- 只新增代码，不修改 `executeEltwise`。
- 自检：在 n_tiles=8 时，`executeEltwiseTiles(ADD, 0, 8, …)` 与 `executeEltwise` 输出逐 bit 相同、周期相同；分 3 段调用后输出也逐 bit 相同。

### 8.3 新增 `Phase1FeederMemory::setCompletionObserver(std::function<void(const P1CompletionEvent&)>)`

- 事件字段：`{sequence, is_write, row, column, tag, submit_cycle, complete_cycle}`；默认不设置；只读回调，不改变任何调度。
- 自检：同一个 W1 case 在设置/不设置观察者时，`metrics.json` 和 `output.bin` 完全一致。

### 8.4 L3 用例（Stage 5 的硬性验收）

| 编号 | 场景 | 必须通过的断言 |
|---|---|---|
| E2E-0 | W1 与 W2 无故障，n_tiles ∈ {8, 32}，启用 Phase 2 记账 | 输出与 reference 逐 bit 一致；账本条目数 = 总 tile 操作数；记账开/关时周期相同 |
| E2E-1 | W1 n_tiles=8，page 粒度，F2(UE) 注入 A 的 tile k ∈ {0, 3, 7}，policy B1 | 状态为 SELECTIVE_RECOVERED；`affected == {ADD_k}`；`restore_list == {A_k}`（`restore_bytes == 4096`）；最终输出逐 bit 一致；**负面对照 NO_REPLAY：tile k 与 reference 不一致，其余 tile 一致** |
| E2E-2 | 同 E2E-1，policy B0 | 输出逐 bit 一致；记录 B0 的各项周期，供对比 |
| E2E-3 | W1，bank 粒度，F2，B1 | `affected ⊇ {ADD_k}`；输出逐 bit 一致；记录 affected 大小 |
| E2E-4 | W1，F1(CE)，B2 | 状态为 SOURCE_REPAIRED_ONLY；`replayed_tiles == 0`；`restore_bytes == 4096`；输出逐 bit 一致；恢复后再读 A_k 为 CLEAN（无新 CE） |
| E2E-5 | W2 n_tiles=8，ADD kernel 完成后向 T 的 tile k 注入 F2，B1 | `ADD_k ∈ affected`（溯源）；`MUL_k ∈ affected`；状态不是 NO_TRUSTED_INPUT；输出逐 bit 一致；负面对照 NO_REPLAY 时输出不一致 |
| E2E-6 | 同 E2E-1，负面对照 NO_RESTORE | 重放重新读到坏的 A_k，`reports_during_recovery ≥ 1`，case 被判为恢复失败且输出不发布（证明恢复输入这一步不可省略） |
| E2E-7 | 同 E2E-1，负面对照 CHECKPOINT_MISSING(A_k) | 状态为 OUTSIDE_WINDOW_NO_TRUSTED_INPUT；`replayed_tiles == 0`；B_k 为 POISONED，host 读被拒绝 |
| DIFF | 对 `runs/phase2/l3/` 下的全部 run 执行 `tests/phase2/test_differential.py` | Python 引擎根据 `ledger.csv + reports.csv + params` 重算出的 `decision.json` 与 C++ 输出完全一致 |

负面对照中，“输出不一致”必须是实际比较得到的结果，不能是推断。

### 8.5 L3 扫参（`configs/phase2/l3_suite.yaml` → `scripts/phase2/expand_l3.py` → `scripts/phase2/run_l3.py`）

W1、W2 × n_tiles {8, 32} × granularity {bank, page} × fault_kind {F1, F2} × fault_tile {0, 中间, 末尾} × policy {B0, B1, B2} × seeds 0..4。runner 支持 `--jobs` 和 `--resume`，并按 Phase 1 的方式对账（SUCCESS / EXPECTED_NEGATIVE / FAIL / TIMEOUT / MISSING）。

---

## 9. 验收测试（L1）

Python（`tests/phase2/test_engine.py`）与 C++（`Phase2EngineTestCases.cpp`）实现同一组用例，账本均为手工构造：

| 编号 | 场景 | 断言 |
|---|---|---|
| U1 | W1，4 个 tile，UE，page 报告覆盖 A_1 | SELECTIVE；affected = {ADD_1}；restore = {A_1}（X_1 不在可疑区，直接读内存） |
| U2 | W2，4 个 tile，UE 在 A_0 | affected = {ADD_0, MUL_0, RELU_0}，且按 op_id 升序 |
| U3 | W2，UE 在 T_2，由 MUL_2 检测到 | Regenerate = {ADD_2}；affected ⊇ {ADD_2, MUL_2}；restore = ∅（A_2/X_2 不在可疑区）；不是 NO_TRUSTED_INPUT |
| U4 | CE 在 A_1，分别用 B2 与 B1 | B2：SOURCE_REPAIRED_ONLY，affected = ∅，restore = {A_1}；B1：affected = {ADD_1} |
| U5 | MUL_0.dep_complete = false | WHOLE_JOB_FALLBACK；affected = 该 job 全部已完成 op |
| U6 | `ledger_truncated = true` | WHOLE_JOB_FALLBACK |
| U7 | 报告队列 overflow | WHOLE_JOB_FALLBACK |
| U8 | B_1 已 CONSUMED | PARTIAL_WITH_ESCAPE；escaped = {B_1}；affected 仍为 {ADD_1} |
| U9 | checkpoint 缺少 A_1 | NO_TRUSTED_INPUT；affected 不被执行 |
| U10 | 读时间早于 ws 的 op；读时间位于 report_cycle 与 t_ack 之间的 op | 前者被排除；后者被包含 |
| U11 | bank 粒度报告 | 种子只受时间窗限制，结果与手算一致 |
| U12 | CE + B2R-1 | 相邻行数据项的读者被纳入，且不受时间下界约束 |
| U13 | `report_cycle < Δ` | `window_start == 0`，无下溢 |
| U14 | 存在原地更新的账本 | WHOLE_JOB_FALLBACK，原因 IN_PLACE_UNSUPPORTED |
| U15 | 同一输入运行两次 | `decision.json` 字节级一致 |

---

## 10. 分阶段执行计划（每个 Stage 结束都要停下汇报）

**Stage 0：环境与基线（不改任何已有文件）**
1. 设置 `$P1`，记录 `uname -a`、gcc/scons/python 版本，写入 `runs/phase2/stage0/env.txt`。
2. 编写 `scripts/phase2/relocate_manifest.py`，生成 `configs/generated/phase2/regression-manifest.json`（以 `regression_g13-manifest.json` 为源，把 run 目录改到 `runs/phase2/regress_before`）。
3. 构建 `simulator/sim`；运行 `./sim --gtest_filter='Phase1Ecc.*:Phase1Storage.*:Phase1Runtime.*:Phase1Checkpoint.*:PIMKernelFixture.*'`，全部通过（把实际通过数写入 env.txt，后续以它为准；`PIMBenchFixture.add` 的已知性能失败不在此过滤器内）。
4. 用 `scripts/run_suite.py --manifest configs/generated/phase2/regression-manifest.json --results-dir results/phase2/regress_before --runs-dir runs/phase2/regress_before --jobs $(nproc)` 跑 240 个 case。
5. 快照：把 `simulator/src/` 复制到 `runs/phase2/stage0/src_snapshot/`，并生成 `phase1_files.sha256`。
6. 运行 `calibrate.py` 生成 `calibration.json`，与 §2 的参考值核对（±1 cycle）。

退出条件：240/240 为 SUCCESS 或 EXPECTED_NEGATIVE；calibration 核对通过。

**Stage 1：核对事实表（只读）** → 输出 `docs/phase2_mount_points.md`，逐条标注“一致”或“差异”。

**Stage 2：Python 参考引擎与 L1 测试** → `phase2/layout.py`、`phase2/engine.py`、`tests/phase2/test_engine.py`。退出条件：U1–U15 全部通过，`check_isolation.sh` 通过。

**Stage 3：L2 评估器与主图初稿** → `phase2/workloads.py`、`timing.py`、`evaluate.py`，`scripts/phase2/run_l2.py`、`aggregate.py`、`plot_phase2.py`。退出条件：§11 中所有可由 L2 产出的 figure_data 和图生成成功；§7.4 的标定自检结果已记录。

**Stage 4：C++ 引擎与 Phase 1 新增钩子** → §0.1 列出的 C++ 模块、`Phase2EngineTestCases.cpp`、地址向量导出，以及 §8.2、§8.3 的新增接口。退出条件：
- C++ 版 U1–U15 全部通过，地址向量比对通过，§8.2/§8.3 的自检通过；
- 用 `regress_after` 重跑 240 个 case，`scripts/phase2/compare_runs.py --before runs/phase2/regress_before --after runs/phase2/regress_after` 输出 `REGRESSION=PASS`；
- 用 `diff -ruN runs/phase2/stage0/src_snapshot simulator/src` 生成 `environment/phase2_source.patch`，确认被改动的已有文件只有白名单中的那几个。

**Stage 5：L3 端到端** → `Phase2E2ETestCases.cpp`、E2E-0 至 E2E-7、DIFF。退出条件：§8.4 全部通过。

**Stage 6：L3 扫参与模型验证** → 运行 §8.5；用 W1 的 L3 数据重新拟合成本参数，在 W2 上做验证（禁止在同一批数据上拟合后又拿它验证）；输出 P2-F6，在报告中写明 MAPE。

**Stage 7：统计、终版图表与报告** → `results/phase2/statistics.csv`、`figures/phase2/*`、`report/phase2_results.md`、`reproduce_phase2.sh`。

**降级**：Stage 4/5 进入 BLOCKED 时，跳过 Stage 6 中依赖 L3 的部分，Stage 7 照常完成，图中只标注 “model (L2)”，报告中写明阻塞点和已完成的 L3 子集。

---

## 11. 实验结果呈现规范（论文级）

### 11.1 图表清单

| 编号 | 内容 | 形式 | 数据文件（`results/phase2/figure_data/`） |
|---|---|---|---|
| P2-F1 | 报告粒度 × 策略对选择性的影响：(a) 重放 tile 占 job 的比例，(b) 恢复延迟（相对 B0 归一化）；UE，n_tiles=32，分面为 W1 / W2 / W3 | 双栏，分组柱 + 95% CI，Oracle 用黑色短横线标出 | `p2f1_selectivity.csv` |
| P2-F2 | 恢复延迟分解：quiesce / restore / replay / resume / publish；B0 vs B1 vs B2；CE 与 UE 两组，n_tiles ∈ {8, 32}；L3 实测值用菱形点叠加 | 单栏，堆叠柱 | `p2f2_breakdown.csv` |
| P2-F3 | 无故障稳态开销随 Δ/T_job 的变化：GLOBAL_HOLD vs DEP_RELEASE，W1 与 W2；叠加 Phase 1 P1-F 的实测点作为参照 | 单栏，折线 + CI 阴影 | `p2f3_release.csv` |
| P2-F4 | 漏检覆盖率与重放成本的 Pareto 图：每种漏检距离类别一条曲线，曲线上的点为 B2、B2R-0/1/2、B0 | 单栏，散点连线；比例用 Wilson CI | `p2f4_radius.csv` |
| P2-F5 | 各场景下 P2Status 的分布 | 单栏，100% 横向堆叠柱 | `p2f5_status.csv` |
| P2-F6 | L2 预测 vs L3 实测的恢复周期，带 y=x 线，标注 MAPE；W1 为拟合集（空心），W2 为验证集（实心） | 单栏，散点 | `p2f6_validation.csv` |
| P2-T1 | 账本开销表：每个 job 的条目数、估计字节数、GLOBAL_HOLD 与 DEP_RELEASE 下的 peak_held_items | LaTeX booktabs 表 + Markdown 表 | `p2t1_ledger.csv` |

每个 figure_data CSV 每行对应一个 case/seed（原始粒度），聚合由绘图脚本完成。必须包含 `source` 列（`L2` / `L3`）和 `seed` 列；字段名在 `figures/phase2/captions.md` 中逐列定义。

### 11.2 样式（`scripts/phase2/p2style.py`，所有图共用）

- 复制 `scripts/plot_paper.py` 中的 rcParams：DejaVu Sans，字号 8–9 pt，`pdf.fonttype=42`，去掉上/右边框，300 dpi。
- 尺寸：单栏 3.5 × 2.4 in，双栏 7.16 × 2.6 in；不使用 `tight_layout` 之外的手工缩放。
- 策略颜色在所有图中固定（Okabe–Ito）：B0 `#666666`、B1 `#0072B2`、B2 `#009E73`、B3 `#D55E00`、B2R 系列 `#CC79A7` 由浅到深、Oracle 黑色虚线。同时给每种策略配不同的 hatch 或 marker，保证灰度可读。
- 图内不写标题（标题放进 caption）；多面板在左上角标粗体 (a)(b)；图例放在绘图区上方或空白处，不加边框。
- 坐标轴标签带单位，例如 “Recovery latency (DRAM cycles)”、“Replayed tiles (% of job)”；柱状图 y 轴从 0 开始；只有数据跨度大于 20 倍时才用对数轴，并在标签中注明。
- 误差：连续量用 95% bootstrap（10000 次，seed=20261115），比例用 Wilson 95%；每个点 n ≥ 10（L2 为 30，L3 为 5 × fault_tile 组合），并在 caption 中写出 n。
- L2 与 L3 数据必须在视觉上可区分（L2 用柱或线，L3 用菱形点），不得把两者混在同一根柱里。
- 输出：`figures/phase2/<name>.pdf`、`.svg`、`.png`（300 dpi）以及 `<name>_gray.png`；`figures/phase2/captions.md`（每张图注明：数据来源、样本量、过滤规则、ASSUMED 项）；`figures/phase2/plot_manifest.json`（输入文件 SHA256、行数、过滤条件、输出文件 SHA256）。
- 绘图脚本只读取 `results/phase2/figure_data/*.csv`，不重跑仿真。

### 11.3 统计表

`results/phase2/statistics.csv` 的列：`figure,metric,group,config,n,mean,median,min,max,ci_low,ci_high,ci_method,numerator,denominator`。

---

## 12. 每个 Stage 的汇报模板

```text
Stage N 结果：PASS / BLOCKED
新增/修改文件：<相对 $P1 的路径列表；修改的已有文件单独列出>
自检命令与结果：<命令> -> <关键输出/计数>
关键数字：<例如 U1–U15 15/15；REGRESSION=PASS；E2E-1 affected={ADD_3}>
新增决策：docs/phase2_decisions.md #<编号>
未解决问题：<无 / 阻塞点 + 日志路径>
```

---

## 13. 交付清单与必须写明的局限

**交付物**：§0.1 中的全部文件；`environment/phase2_source.patch`；`results/phase2/validation.json`（列出全部断言，格式为 name/pass/expected/actual）；`report/phase2_results.md`（方法、机制、实验设置、P2-F1 至 F6 与 T1、讨论、局限）；`scripts/phase2/reproduce_phase2.sh`。

**报告中必须写明的局限**：
- 结果来自模型内实现，不代表真实 HBM-PIM 硬件；
- 注入的是一次性故障，不涉及持续攻击；
- 漏检位置分布是 ASSUMED；
- L2 的时间/成本模型由 Phase 1 标定，MUL/RELU 成本经 L3 修正之前属于 ASSUMED；
- 只考虑单 channel；
- 原地重放依赖“无原地更新、无 buffer 复用”的假设；
- Δ 上界依赖报告队列不溢出，溢出时降级；
- host 拷贝、互连和 OS 开销未建模；
- bank 粒度报告在当前布局下空间上不可区分，选择性只来自时间窗。

# G10 补充实验预注册协议（P1-G / P1-H）

- 预注册日期：2026-11-16（UTC）
- 依据：`PHASE1_SUPPLEMENT_HANDOFF.md` §4、§5
- 适用范围：HBM-PIM Phase 1 补充实验 G10–G16
- 状态：本文件在 G10 冻结；G14 之后不得修改。必须偏离时，在 `docs/g16_supplement_results.md` 的“偏离预注册”一节逐条说明原因。
- 本文件 SHA256 由 `docs/STATUS.md` 在 G10 完成时记录。

本协议只登记检验对象、指标定义、分类规则、矩阵和预注册假设，不包含任何实验结果。文中“预期/假设”不是测量结果。

## 0. 范围限制与证据标签

1. 不实现 Phase 2 的依赖追踪或选择性恢复。
2. P1-G 的 F3M/F4S 是按 Copy-on-Flip 威胁模型（攻击者了解 ECC 函数）**人为构造的位模式**，不是 Rowhammer 发生概率模型。
3. scrub 是理想化、零时序成本的模型假设（**ASSUMED**）；codeword 粒度报告也是新增的假设接口（**ASSUMED**），用于给出“更细粒度能区分什么”的上界。
4. 不修改、不重跑、不覆盖已有 paper 矩阵结果。

证据标签沿用原手册 §0.1：DOCUMENTED / UPSTREAM / ASSUMED / SIMULATED / UNSUPPORTED。P1-G 与 P1-H 的新接口和新增行为属于 ASSUMED；实际运行数据属于 SIMULATED。

## 1. 冻结边界

以下已有产物只读，G10–G16 均不得修改或覆盖：

```text
runs/paper/                     results/statistics.csv、statistics.json
results/g1 … results/g8/        results/figure_data/fig1…fig6*.csv
results/run_metrics.csv、summary.csv、failure_cases.csv、validation_summary.json
figures/fig01 … fig06*、figS1*、figures/gray/、figures/plot_manifest.json
configs/suite_paper.yaml、configs/generated/paper*、configs/generated/paper/
artifact/                       environment/source_g0.patch … source_g8.patch
```

例外：`figures/captions.md`、`report/phase1_results.md`、`report/phase1_results.tex`、`docs/STATUS.md`、`docs/implementation_log.md`、`environment/source.patch` 允许追加或按补充手册 §8 修改。

补充实验产物统一写入 `configs/generated/supplement*`、`runs/supplement/`、`runs/regression_g10/`、`results/supplement/`、`results/regression_g10/`、`figures/fig07*`、`figures/fig08*`、`figures/figS2*` 和 `docs/g1*_supplement_*`。

## 2. P1-G：漏检损坏下的控制有效性

### 2.1 位模式（G11 验证）

所有位号都是 `Bank::injectRawDataBit(row, burstColumn, bitInBurst)` 的 `bitInBurst`（0–255）。该函数内部 `codeword = bitInBurst/64`、`data_bit = bitInBurst%64`。下列模式都在 codeword 0 内，即 FP16 lane 0 的低位。

SECDED 位置映射（原手册 §3.3）：数据位 j 放在 1..71 中跳过 1,2,4,8,16,32,64 后的第 j 个位置。因此数据位 0→位置 3，1→5，4→9，10→15，14→20，15→21。

| 名称 | 翻转的数据位 | 推导 | 预期解码结果 |
|---|---|---|---|
| F1 | {15} | 单比特 | CORRECTED，数据恢复原值，产生 CE |
| F3M（miscorrect） | {0,1,4} | 3⊕5⊕9=15，整体校验=1 | **CORRECTED**，解码器把位置 15（数据位 10）也翻转，返回数据 = 原值 ⊕ 位{0,1,4,10}；**产生 CE，但数据错误** |
| F4S（silent） | {0,1,4,10} | 3⊕5⊕9⊕15=0，整体校验=0 | **CLEAN**，返回数据 = 原值 ⊕ 位{0,1,4,10}；**不产生任何报告** |
| S4 第二位 | 在 {15} 已翻转时再翻 {14} | 20⊕21=1，整体校验=0 | UNCORRECTABLE（UE） |
| cw1 单比特 | bitInBurst=79 | codeword 1 的数据位 15 | CORRECTED |

F3M 和 F4S 交给消费者的坏数据完全相同（都是原值 ⊕ 位{0,1,4,10}），区别只在是否有报告。这是本组实验的核心对照，不得更改位模式。

### 2.2 G11 ECC 验证

1. 在 `tests/test_ecc_reference.py` 中新增 `test_escape_patterns`：用该文件已有的**独立 Python 参考实现**（不要逐行翻译 C++），对数据字全 0、全 1、`0x5555…`、`0xAAAA…` 和 4 个固定随机 64 位值，逐一验证上表每一行的状态和返回数据。
2. 在 `simulator/src/tests/Phase1EccTestCases.cpp` 中新增 `TEST(Phase1Ecc, EscapePatterns)`，用 C++ `Phase1Ecc::encodeParity/decode` 验证同样的内容。
3. 运行：

```bash
.venv/bin/python -m pytest tests/test_ecc_reference.py -q
cd simulator && ./sim --gtest_filter='Phase1Ecc.*'
```

任何一行与预期不符：停止，记录实际 syndrome/状态，报告 BLOCKED。不得修改 `Phase1Ecc.cpp`，也不得自行换位模式。

### 2.3 G12 fixture

复制 `simulator/src/tests/Phase1G8P1CTestCases.cpp` 为 `simulator/src/tests/Phase1G10EscapeTestCases.cpp`，测试名改为 `TEST(Phase1G10EscapeFixture, RunCase)`。上游 `Sconstruct` 会自动收集 `src/tests/*.cpp`。主流程（preload → 注入 → `executeEltwise` ADD → `runProducer` → hold → consumer）保持与 P1-C 完全一致，只做以下改动。

新增参数（params.ini）：

| key | 取值 | 说明 |
|---|---|---|
| `fault_kind` | `F3M`、`F4S`、`F1W_F4S` | 替换原来的 F0/F1/F2 检查 |
| `escape_placement` | `NONE`、`SAME_BANK`、`DIFF_BANK` | F3M/F4S 必须为 NONE；F1W_F4S 必须为 SAME_BANK 或 DIFF_BANK |
| `target_burst` | 由 `expand_suite` 的 `target_burst_for(seed, n_tiles)` 给出 | 与 P1-C 相同 |

注入规则（全部在 preload 完成后、`executeEltwise` 之前执行，与 P1-C 相同）：

- F3M：在 `target_burst` 翻转位 {0,1,4}。`corrupt_burst = witness_burst = target_burst`。
- F4S：在 `target_burst` 翻转位 {0,1,4,10}。`corrupt_burst = target_burst`，无 witness。
- F1W_F4S：在 `witness_burst = target_burst` 翻转位 {15}（会被纠正的 CE 见证）；再按确定性规则选 `corrupt_burst`：从 `b = target_burst+1` 开始逐个递增（到 `dim` 后回到 0），取第一个满足条件的 b。SAME_BANK 的条件是 `codec.decode(aAddress + b*32).bank == witness 的 bank`，DIFF_BANK 的条件是 `!=`。在 `corrupt_burst` 翻转位 {0,1,4,10}。C++ 不得随机抽样。

每次 `injectRawDataBit` 调用都写一行 INJECTION 到 `truth.csv`（沿用 P1-C 的列，`bit_positions` 用 `|` 连接，`target_access_id` 写 `A_burst_<b>`）。

评估器（只在运行结束后执行，只读，不经过 runtime，不调用 `Bank::read`，不产生 ECC 报告）：

1. `corruption_propagates`：取 `inputA[corrupt_burst]`，对其 cw0 的 64 位做 ⊕ 位{0,1,4,10} 得到 A′；对 F3M 额外断言 `Phase1Ecc::decode(被翻转后的原始字, 原 parity).data == A′`，对 F4S 断言 `decode` 状态为 CLEAN 且 data == A′（不成立则 `INVALID_CASE`）。计算 B′ = A′ + X（与 `expectedAdd` 相同的 FP16 运算）；PIM consumer 再算 C′ = 2·B′。若 B′（HOST）或 C′（PIM）与无故障期望逐字节相同，则 `corruption_propagates=false`（FP16 舍入掩蔽）。
2. `held_output_corrupt`：仅当输出**未发布**时计算。B 区起始地址为 `codec.encode({0,0,0,256,0})`；用 `codec.decode(bAddress + corrupt_burst*32)` 得到 tuple，从对应 Bank 的 `phase1Storage().readRawData(key)` 读出 4 个 codeword，拼成 32 B 与 `expectedB[corrupt_burst]` 比较：不同为 true，相同为 false，任一 key 不存在为 null。
3. `report_region_ids`：从 `reports.csv` 读出的全部 region_id 列表；`corrupt_bank`、`witness_bank`；`report_covers_corrupt_bank` = 是否有某个 bank 粒度 region_id 对应 corrupt_bank（region_id 与 bank 的换算必须照抄 `Phase1Reports.cpp` 的实现，不要猜）。

结果分类 `escape_outcome`（按顺序判定，命中即停）：

| 顺序 | 条件 | 类别 |
|---|---|---|
| 1 | timeout | `TIMEOUT` |
| 2 | 评估器断言失败 | `INVALID_CASE` |
| 3 | `corruption_propagates == false` | `MASKED` |
| 4 | `consumed && output_changed` | `CORRUPT_CONSUMED` |
| 5 | `!published && lock_cycle != 0 && held_output_corrupt == true` | `PREVENTED` |
| 6 | 其他任何情况 | `UNCLASSIFIED`（contract 失败，需调查） |

原有的 `outcome_class`（TIMELY_CONTROL / LATE_CONTROL / NO_REPORT …）照常计算和输出，用于和 Fig. 3 对照。

`contract_pass` 条件（全部成立）：

- 不超时；`escape_outcome ∈ {PREVENTED, CORRUPT_CONSUMED, MASKED}`；`internal_ue == 0`；
- F4S：`delivered_reports == 0` 且 `detected_events == 0`；
- F3M：`detected_events >= 1`，且 truth 中该 burst 的解码状态为 CORRECTED；
- F1W_F4S：`detected_events >= 1`（来自 witness），corrupt_burst 不产生检测事件；
- runtime 对象不持有 truth、golden、FaultSchedule 的引用（沿用 P1-C 的隔离写法，不要新增引用）。

`metrics.json` 新增字段：`fault_kind, escape_placement, witness_burst, corrupt_burst, witness_bank, corrupt_bank, corruption_propagates, held_output_corrupt, escape_outcome, report_region_ids, report_covers_corrupt_bank`。原 P1-C 字段全部保留。

### 2.4 P1-G 矩阵

| 维度 | 取值 |
|---|---|
| profile / 粒度 | LOCAL_VISIBLE / bank |
| n_tiles | 1, 8 |
| consumer | HOST, PIM |
| seed | 0..29（`target_burst_for`，与 P1-C 相同） |
| fault 配置 | F3M/NONE、F4S/NONE、F1W_F4S/SAME_BANK、F1W_F4S/DIFF_BANK |
| D/T_ref | 0, 0.5, 1 |
| H/T_ref | 0, 0.25, 1 |
| 其他 | L=8，I=1，Q=64，watchdog=400000；T_ref 取 `results/g8/calibration.json` 的 `p1c_t_ref`；`D=0 if r==0 else ceil(r*T_ref)`，H 同理 |

总数 2×2×30×4×3×3 = **4320**。smoke：n_tiles=1、HOST、seed 0..2、4 种 fault 配置、D/T∈{0,1}、H/T=0，共 **24**。

### 2.5 P1-G 预注册假设（检验对象，不是结果）

- H-G1：F3M 的 PREVENTED 格局与 Fig. 3 同一格子的 TIMELY 比例一致；迟到的格子变成 CORRUPT_CONSUMED。
- H-G2：F4S 在所有 D/H 下都是 CORRUPT_CONSUMED 或 MASKED，PREVENTED=0，因为没有报告。
- H-G3：F1W_F4S 的 SAME_BANK 与 DIFF_BANK 结果相同，因为当前 runtime 收到任何报告就锁住整个 kernel 输出；DIFF_BANK 的 `report_covers_corrupt_bank` 全为 false。这说明如果将来只按报告区域做选择性恢复，DIFF_BANK 的损坏会被漏掉。

结果与假设不符时如实报告，不调参数。

## 3. P1-H：报告序列能区分什么

### 3.1 C++ 改动（默认关闭，默认行为必须逐位不变）

1. **scrub 开关（ASSUMED）**：在 `Bank` 中增加 `void setPhase1Scrub(bool enabled)` 和计数器 `uint64_t phase1ScrubWrites() const`，成员默认 `false`。在 `Bank::read` 受保护路径中，当 `result.status == Phase1EccStatus::CORRECTED` 且开关为 true 时，调用 `phase1Storage_.writeLegal(key, result.data)` 并计数。不产生额外总线事务，不增加周期（ASSUMED，理想化零成本）。开关为 false 时不得执行任何新代码路径。
2. **codeword 粒度（ASSUMED）**：在 `Phase1Reports` 中让 granularity 额外接受 `"codeword"`，此时 `region_type="codeword"`，`region_id` 为 `(channel, actual_bank, row, burst_column, codeword_index)` 的确定性线性编码。bank/page 的行为不变。

   G13 固定的编码公式（ASSUMED，仅用于本模型报告接口）：

   ```text
   region_id = (((channel * num_banks + actual_bank) * num_rows + row)
                * burst_columns + burst_column) * 4 + codeword_index
   ```

   其中 `num_banks` 是每 channel 的 bank 数，`num_rows`、`burst_columns` 来自 `Phase1Address` 的固定几何；`codeword_index ∈ 0..3`。该公式只定义模型的公开报告标识，不代表真实 HBM 报告格式，也不提供位位置或 syndrome 信息。

改完立即执行 §4.2 的回归对比，必须 `REGRESSION=PASS`。

### 3.2 P1-H fixture

新建 `simulator/src/tests/Phase1G11SequenceTestCases.cpp`，`TEST(Phase1G11SequenceFixture, RunCase)`。只用主机读写路径，复用 `Phase1G4TestCases.cpp` 里的 host write/read 写法（`memory.addTransaction(true/false, address, payload)`，约第 140–170 行），先读懂再复用，不要凭函数名猜。

参数：`scenario`、`granularity ∈ {bank,page,codeword}`、`scrub ∈ {0,1}`、`seed`、`bank_p`、`bank_r`、`rounds=4`、`transport_delay_cycles=0`、`service_interval_cycles=1`、`report_queue_capacity=64`、`watchdog_cycles=200000`。profile 固定 LOCAL_VISIBLE。

三个存储位置（地址用 `codec.encode(Phase1AddrTuple{0,0,bank,row,col})`，并做 decode round-trip 断言）：

- P = (bank_p, row 0, col 0)
- Q = (bank_p, row 1, col 0)，与 P 同 bank、不同 row
- R = (bank_r, row 0, col 0)，不同 bank

流程：

1. 主机合法写入 P、Q、R，数据用确定性模式（例如 `0x0123456789ABCDEF ^ (seed * 0x9E3779B97F4A7C15)` 依次填 4 个 codeword），运行到 idle。
2. 对 `k = 0 .. rounds-1`：先执行该轮预定的注入；再按 **P、Q、R 的固定顺序**各主机读一次；推进模拟直到 idle 且报告队列清空（`reports.pendingCount()==0`）。
3. 每轮读取次数在所有场景中相同，避免读取次数本身泄露场景信息。

场景表（`round: burst bitInBurst`）：

| 场景 | 对应论文中的情形 | 注入 | scrub |
|---|---|---|---|
| S1_SINGLE | 一个持续错误位 | r0: P 15 | 0, 1 |
| S2_DIFF_CW | 不同 codeword 的独立错误（同 burst） | r0: P 15；r1: P 79 | 0, 1 |
| S3_DIFF_BURST_SAME_BANK | 不同 codeword 的独立错误（同 bank 不同 row） | r0: P 15；r1: Q 15 | 0, 1 |
| S4_SAME_CW_TWO_BITS | **同一 codeword 内两个不同易翻转位（危险情形）** | r0: P 15；r1: P 14 | 0, 1 |
| S5_DIFF_BANK | 不同 bank | r0: P 15；r1: R 15 | 0, 1 |
| S6_SAME_BIT_RECUR | 同一位反复翻转 | r0: P 15；r1: P 15 | **仅 1** |

S6 只在 scrub=1 时运行：scrub 已把第一次翻转修好，第二次注入才代表“再次翻转”；scrub=0 时 XOR 注入会把位翻回去，没有意义。S6 在 r1 注入前必须断言 P 的原始 cw0 数据位 15 已等于初始值，否则 `INVALID_CASE`。

输出：`reports.csv`（公开字段）、`truth.csv`（每次注入一行 INJECTION，每次检测一行 DETECTION）、`metrics.json`（`scenario, granularity, scrub, seed, bank_p, bank_r, rounds, detected_events, delivered_reports, internal_ce, internal_ue, scrub_writes, report_signature, contract_pass, timeout`）。`report_signature` 是按 `report_seq` 排序后的 `error_class:region_id` 用 `;` 连接的字符串，不含 report_cycle。

contract_pass：不超时；每轮注入实际发生；`delivered_reports == detected_events`（Q=64 时不应丢报告）；每条报告都对应 truth 中一次真实检测。

### 3.3 P1-H 矩阵

seed 0..9；`bank_p`、`bank_r` 在 `expand_supplement.py` 中用 `numpy.random.Generator(PCG64(seed))` 生成：`bank_p = rng.integers(0,16)`，`bank_r = (bank_p + 1 + rng.integers(0,15)) % 16`。每个 seed：3 粒度 ×（scrub=0 的 5 个场景 + scrub=1 的 6 个场景）= 33；总数 **330**。smoke：seed 0 的全部 33 个。

### 3.4 P1-H 分析定义（G15 实现）

- **不可区分**：同一 seed、同一 (granularity, scrub) 下，两个场景的 `report_signature` 完全相同。对每个场景对统计 10 个 seed 中不可区分的个数 k/10。另输出一列 `identical_with_timing`（签名中加入相对于第一份报告的 report_cycle 差值），只放 CSV，不进主图。
- **CoF 式规则**：`cof_fire` = 存在某个 region_id 收到 ≥2 份 CE。按 (scenario, granularity, scrub) 统计 fire 次数 / 10。
- `ue_seen` = 是否出现 UE 报告。
- 危险真值：只有 S4 属于“同一 codeword 内积累多个易翻转位”。

### 3.5 P1-H 预注册假设（检验对象，不是结果）

- H-H1：scrub=1、bank 粒度下，S2、S3、S4、S6 两两不可区分。
- H-H2：scrub=1、codeword 粒度下，S4 与 S6 仍不可区分（区分它们需要位位置或 syndrome，威胁模型假设接口不提供）。
- H-H3：scrub=0 时 S4 出现 UE，因而可区分，但此时错误已不可纠正。
- H-H4：CoF 式规则在 S2/S3/S4（scrub=1）上同样触发，不能把危险情形与良性情形分开。

## 4. G10 回归基线口径

### 4.1 回归选择

从 `configs/generated/paper-manifest.json` 选择：

- 全部 P1-A（144）；
- P1-C 中 `seed==0` 的全部 LOCAL_VISIBLE 主矩阵 case（60）；
- P1-E 中 `kind=="positive"` 且 `seed==0` 的 case（2）；
- P1-F Cost 中 `seed==0` 的 case（16）；
- 全部 P1-F Robustness（18）。

共 240 cases。只把每个 case 的 `params.output_dir` 和 `case.out` 改到 `runs/regression_g10/<run_id>`，其余字段不动。

### 4.2 比较口径

对每个 case 比较新运行与 `runs/paper/<run_id>/`：

- `metrics.json`：忽略键 `wall_seconds`、`paired_ext_off_wall_seconds`、`wall_overhead_fraction`，以及任何以 `_utc` 结尾的键，其余逐键相等；
- `output.bin`、`reports.csv` 的 SHA256 相等（文件不存在时两边都必须不存在）。

输出 `results/regression_g10/compare.csv`（run_id, group, field, paper_value, new_value）和一行汇总 `REGRESSION=PASS` 或 `REGRESSION=FAIL n_diff=<k>`。

### 4.3 里程碑要求

- G10：改代码之前跑一次，必须 0 差异（证明比较脚本有效）。
- G13：scrub/codeword 改动之后必须 0 差异（默认关闭路径隔离）。
- G16：收尾再跑一次，必须 0 差异。

### 4.4 冻结哈希

G10 开始时在 `results/regression_g10/frozen_sha256.txt` 记录冻结清单内全部现存文件的 SHA256；G16 用同一路径集合复核。工作区实际不存在 `environment/source_g0.patch`（G0 没有源码改动），记录中如实注明，不用空文件或伪造哈希补位。

## 5. 统计与图表口径（G15，不在 G10 实现）

- Wilson 95% CI 与 bootstrap 约定照抄原手册 §9.1：bootstrap 10,000 次、固定统计 seed=20261115；Wilson z=1.96。
- P1-G 的 PREVENTED 与 CORRUPT_CONSUMED 比例以 `n − MASKED` 为分母，同时列出 MASKED 和总数 n；n′=0 时不得画成 0%。
- Fig. 7 取 n_tiles=8、HOST consumer；F3M、F1W_F4S SAME_BANK、F1W_F4S DIFF_BANK、F4S 四面板；x=H/T_ref，y=D/T_ref（从上到下递增），颜色=CORRUPT_CONSUMED 比例（分母 n−MASKED），色条 0–1，白→深红。
- Fig. S2 为 4 行（n_tiles × consumer）× 4 列（fault 配置），格式同 Fig. 7。
- Fig. 8 为 2 行（scrub=0、1）× 3 列（bank、page、codeword）的场景对矩阵；颜色=不可区分 seed 比例；S4 行标签标星；scrub=0 的 S6 行列斜线填充并标 `N/A`，不得涂成 0。
- 图只读 `results/supplement/figure_data/*.csv`；图注写数据源、过滤、样本数、分母、失败处理和模型假设；图例不得遮挡数据；灰度副本可读。
- 失败与超时单列，不得删除；manifest 对账：总数 = SUCCESS + FAIL + TIMEOUT + MISSING。

## 6. 不允许的“修复”

- 更换位模式让结果更好看；
- 把 MASKED 并入 PREVENTED；
- 只报告 n_tiles=8 而隐藏其他配置；
- 让 runtime 读取 `corrupt_burst` 或 truth；
- 修改冻结清单中的任何文件；
- 删除负例、放宽比较、隐藏失败样本、把未执行命令写成已完成。

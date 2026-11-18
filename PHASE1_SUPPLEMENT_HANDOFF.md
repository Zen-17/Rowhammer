# HBM-PIM Phase 1 补充实验交接手册（G10–G16）

版本：1.0；设计日期：2026-11-16。执行对象：DeepSeek Flash 或其他代码模型。文档语言为中文，代码、数据字段和图中文字使用英文。

本手册是 `pim-phase1/HANDOFF.md`（下称“原手册”）的增补。原手册的全部约束继续有效，包括 §0 的命题区分、§0.1 证据标签、§3.3 ECC 规范、§6 数据隔离、§9 统计与作图规范、§12 禁止的“修复”。本手册与原手册冲突时，以本手册为准；本手册未提到的，按原手册执行。

**本手册是实验设计，不包含任何结果。文中“假设/预期”只是预注册的检验对象，不得写成测量结果，也不得为了让结果符合预期而调整参数、位位置或判定规则。**

---

## 0. 为什么要补这两组实验

Phase 1 已完成 G0–G9（2602 cases，见 `pim-phase1/report/phase1_results.md`）。审阅发现两个证据缺口：

**缺口 A：没有证明控制真的拦住了错误值。** P1-C 的 1800 个 LOCAL_VISIBLE case 全部 `output_changed=0`，因为注入的是单比特 CE，ECC 在运算前已经纠正。所以“TIMELY_CONTROL”只证明版本门控来得及关上，从未出现“门里关着的确实是一个错误结果”的 case。论文威胁模型（`HBM_jintao_main.pdf` §2）关心的是 ECC-aware Rowhammer 造成**未被纠正的损坏**，需要构造“ECC 漏检 + 报告”的场景，证明及时控制能阻止错误结果被消费，迟到时则不能。

**缺口 B：没有涉及风险域识别（论文标题前半部分）。** 论文 §1 Motivation 原文：*“Two reports for one region could reflect a recurrent fault at one bit, independent errors in different codewords, or distinct susceptible bits in one codeword.”* Phase 1 每个 case 只有一次故障、一份报告，没有任何“同一区域多份报告”的场景，无法说明粗粒度报告能区分什么、不能区分什么。

对应新增两组实验：

| 组 | 名称 | 回答的问题 |
|---|---|---|
| P1-G | Escape efficacy | 存在 ECC 漏检损坏时，及时控制能否阻止错误结果被消费？无报告时又如何？ |
| P1-H | Report-sequence ambiguity | 在 bank / page / codeword 三种报告粒度、有无 scrub 两种存储策略下，论文列出的几种故障情形产生的公开报告序列能否区分？ |

范围限制（必须写进报告）：

1. 不实现 Phase 2 的依赖追踪或选择性恢复。
2. F3M/F4S 是按 CoF 威胁模型（攻击者了解 ECC 函数）**人为构造的位模式**，不是 Rowhammer 发生概率模型。
3. scrub 是理想化、零时序成本的模型假设（ASSUMED）；codeword 粒度报告也是新增的假设接口（ASSUMED），用于给出“更细粒度能区分什么”的上界。
4. 不修改、不重跑、不覆盖已有的 paper 矩阵结果。

---

## 1. 冻结清单与工作目录

工作目录：`pim-phase1/`（与原手册相同）。

**以下已有产物只读，任何关卡都不得修改或覆盖：**

```text
runs/paper/                     results/statistics.csv、statistics.json
results/g1 … results/g8/        results/figure_data/fig1…fig6*.csv
results/run_metrics.csv、summary.csv、failure_cases.csv、validation_summary.json
figures/fig01 … fig06*、figS1*、figures/gray/、figures/plot_manifest.json
configs/suite_paper.yaml、configs/generated/paper*、configs/generated/paper/
artifact/                       environment/source_g0.patch … source_g8.patch
```

例外：`figures/captions.md`、`report/phase1_results.md`、`report/phase1_results.tex`、`docs/STATUS.md`、`docs/implementation_log.md`、`environment/source.patch` 允许**追加或按 §8 修改**。

**新产物统一放在：**

```text
configs/suite_supplement.yaml
configs/generated/supplement-manifest.json、configs/generated/supplement/
configs/generated/regression_g10-manifest.json
runs/supplement/<run_id>/       runs/regression_g10/<run_id>/
results/supplement/             # run_metrics/summary/validation/statistics_supplement.*
results/supplement/figure_data/ # fig7_*.csv、fig8_*.csv
results/regression_g10/
figures/fig07_escape_efficacy.*、fig08_report_ambiguity.*、figS2_escape_by_config.*
figures/plot_manifest_supplement.json
docs/g10_supplement_protocol.md、docs/g16_supplement_results.md
environment/source_g16.patch
```

---

## 2. 关卡顺序（严格按顺序，每次只做一关）

| 关卡 | 内容 | 完成证据 |
|---|---|---|
| G10 | 环境检查、回归基线、预注册协议文档 | `docs/g10_supplement_protocol.md`；回归基线 manifest |
| G11 | ECC 位模式验证（Python + C++） | 新增测试通过，日志 |
| G12 | P1-G fixture + smoke | 24 个 smoke case 全部 contract_pass |
| G13 | scrub 开关、codeword 粒度、P1-H fixture + smoke；**回归检查** | P1-H smoke 通过；回归对比 0 差异 |
| G14 | 展开并运行补充矩阵 | manifest 对账：成功 + 失败 + 超时 = 预期总数 |
| G15 | 聚合、统计、作图 | figure_data CSV、统计表、3 张图、plot manifest |
| G16 | 改报告/图注/tex、最终回归、补丁快照、STATUS | §8 列出的全部修改；最终回归 0 差异 |

每关结束后更新 `docs/STATUS.md` 和 `docs/implementation_log.md`，写明：改了哪些文件、执行了哪些命令（含退出码）、哪些检查通过或失败、产物位置、下一关唯一任务。失败时修当前关，不跳关，不改预期。

---

## 3. G10：环境、回归基线、预注册

### 3.1 环境

```bash
cd pim-phase1
.venv/bin/python -c "import numpy, pandas, yaml, matplotlib, jsonschema, pytest; print('ok')"
bash scripts/build.sh
```

如果 `.venv/bin/python` 不可用（例如指向不存在的 `/usr/bin/python3`），按原手册 §11.1 重建 venv，并用 `environment/requirements.lock.txt` 安装相同版本：`.venv/bin/python -m pip install -r environment/requirements.lock.txt`。版本无法完全一致时，把实际版本写进 `implementation_log.md`，不要静默换版本。

### 3.2 回归基线

目的：证明后续对 C++ 和脚本的改动在默认配置下**不改变**任何已有实验的行为。

新建 `scripts/make_regression_manifest.py`：读取 `configs/generated/paper-manifest.json`，选出以下 case，**把每个 case 的 `params.output_dir` 和 `case.out` 改到 `runs/regression_g10/<run_id>`**（其余字段不动），写出 `configs/generated/regression_g10-manifest.json`：

- 全部 P1-A（144）；
- P1-C 中 `seed==0` 的全部 LOCAL_VISIBLE 主矩阵 case（60）；
- P1-E 中 `kind=="positive"` 且 `seed==0` 的 case（2）；
- P1-F Cost 中 `seed==0` 的 case（16）；
- 全部 P1-F Robustness（18）。

修改 `scripts/run_suite.py`：增加 `--results-dir` 参数，**默认值保持 `results/g8` 不变**；把原来写 `RESULTS` 的地方改用该参数。不要改其他默认行为。

新建 `scripts/compare_regression.py`：对 regression manifest 里每个 case，比较 `runs/regression_g10/<id>/` 与 `runs/paper/<id>/` 的
- `metrics.json`：忽略键 `wall_seconds`、`paired_ext_off_wall_seconds`、`wall_overhead_fraction`，以及任何以 `_utc` 结尾的键，其余逐键相等；
- `output.bin`、`reports.csv` 的 SHA256 相等（文件不存在时两边都必须不存在）。

输出 `results/regression_g10/compare.csv`（run_id, group, field, paper_value, new_value）和一行汇总 `REGRESSION=PASS` 或 `REGRESSION=FAIL n_diff=<k>`。

G10 只需在**改代码之前**跑一次，确认对比脚本在原二进制上得到 0 差异（证明脚本本身没问题）：

```bash
.venv/bin/python scripts/make_regression_manifest.py
.venv/bin/python scripts/run_suite.py --manifest configs/generated/regression_g10-manifest.json \
    --results-dir results/regression_g10 --jobs 8
.venv/bin/python scripts/compare_regression.py
```

### 3.3 预注册协议

把本手册 §4、§5 中的指标定义、结果分类、矩阵和“预注册假设”原样整理进 `docs/g10_supplement_protocol.md`，写明日期和该文件的 SHA256（记入 STATUS）。G14 之后不得修改该文件；若必须偏离，在 `docs/g16_supplement_results.md` 的“偏离预注册”一节逐条说明原因。

---

## 4. P1-G：漏检损坏下的控制有效性

### 4.1 位模式（G11 必须先验证）

所有位号都是 `Bank::injectRawDataBit(row, burstColumn, bitInBurst)` 的 `bitInBurst`（0–255）。该函数内部 `codeword = bitInBurst/64`、`data_bit = bitInBurst%64`。下列模式都在 **codeword 0** 内，即 FP16 lane 0 的低位。

SECDED 位置映射（原手册 §3.3）：数据位 j 放在 1..71 中跳过 1,2,4,8,16,32,64 后的第 j 个位置。因此数据位 0→位置 3，1→5，4→9，10→15，14→20，15→21。

| 名称 | 翻转的数据位 | 推导 | 预期解码结果 |
|---|---|---|---|
| F1 | {15} | 单比特 | CORRECTED，数据恢复原值，产生 CE |
| F3M（miscorrect） | {0,1,4} | 3⊕5⊕9=15，整体校验=1 | **CORRECTED**，解码器把位置 15（数据位 10）也翻转，返回数据 = 原值 ⊕ 位{0,1,4,10}；**产生 CE，但数据错误** |
| F4S（silent） | {0,1,4,10} | 3⊕5⊕9⊕15=0，整体校验=0 | **CLEAN**，返回数据 = 原值 ⊕ 位{0,1,4,10}；**不产生任何报告** |
| S4 第二位 | 在 {15} 已翻转时再翻 {14} | 20⊕21=1，整体校验=0 | UNCORRECTABLE（UE） |
| cw1 单比特 | bitInBurst=79 | codeword 1 的数据位 15 | CORRECTED |

注意：F3M 和 F4S 交给消费者的坏数据**完全相同**（都是原值 ⊕ 位{0,1,4,10}），区别只在是否有报告。这是本组实验的核心对照，不要更改位模式。

### 4.2 G11：ECC 验证

1. 在 `tests/test_ecc_reference.py` 中新增 `test_escape_patterns`：用该文件已有的**独立 Python 参考实现**（不要逐行翻译 C++），对数据字全 0、全 1、`0x5555…`、`0xAAAA…` 和 4 个固定随机 64 位值，逐一验证上表每一行的状态和返回数据。
2. 在 `simulator/src/tests/Phase1EccTestCases.cpp` 中新增 `TEST(Phase1Ecc, EscapePatterns)`，用 C++ `Phase1Ecc::encodeParity/decode` 验证同样的内容。
3. 运行：

```bash
.venv/bin/python -m pytest tests/test_ecc_reference.py -q
cd simulator && ./sim --gtest_filter='Phase1Ecc.*'
```

**任何一行与预期不符：停止，记录实际 syndrome/状态，报告 BLOCKED。不得修改 `Phase1Ecc.cpp`，也不得自行换位模式。**

### 4.3 G12：fixture

复制 `simulator/src/tests/Phase1G8P1CTestCases.cpp` 为 `simulator/src/tests/Phase1G10EscapeTestCases.cpp`，测试名改为 `TEST(Phase1G10EscapeFixture, RunCase)`。上游 `Sconstruct` 会自动收集 `src/tests/*.cpp`。主流程（preload → 注入 → `executeEltwise` ADD → `runProducer` → hold → consumer）保持与 P1-C 完全一致，只做以下改动。

**新增参数（params.ini）：**

| key | 取值 | 说明 |
|---|---|---|
| `fault_kind` | `F3M`、`F4S`、`F1W_F4S` | 替换原来的 F0/F1/F2 检查 |
| `escape_placement` | `NONE`、`SAME_BANK`、`DIFF_BANK` | F3M/F4S 必须为 NONE；F1W_F4S 必须为 SAME_BANK 或 DIFF_BANK |
| `target_burst` | 由 `expand_suite` 的 `target_burst_for(seed, n_tiles)` 给出 | 与 P1-C 相同 |

**注入规则（全部在 preload 完成后、`executeEltwise` 之前执行，与 P1-C 相同）：**

- F3M：在 `target_burst` 翻转位 {0,1,4}。`corrupt_burst = witness_burst = target_burst`。
- F4S：在 `target_burst` 翻转位 {0,1,4,10}。`corrupt_burst = target_burst`，无 witness。
- F1W_F4S：在 `witness_burst = target_burst` 翻转位 {15}（会被纠正的 CE 见证）；再按确定性规则选 `corrupt_burst`：从 `b = target_burst+1` 开始逐个递增（到 `dim` 后回到 0），取第一个满足条件的 b。SAME_BANK 的条件是 `codec.decode(aAddress + b*32).bank == witness 的 bank`，DIFF_BANK 的条件是 `!=`。在 `corrupt_burst` 翻转位 {0,1,4,10}。C++ 不得随机抽样。

每次 `injectRawDataBit` 调用都写一行 INJECTION 到 `truth.csv`（沿用 P1-C 的列，`bit_positions` 用 `|` 连接，`target_access_id` 写 `A_burst_<b>`）。

**评估器（只在运行结束后执行，只读，不经过 runtime，不调用 `Bank::read`，不产生 ECC 报告）：**

1. `corruption_propagates`：取 `inputA[corrupt_burst]`，对其 cw0 的 64 位做 ⊕ 位{0,1,4,10} 得到 A′；对 F3M 额外断言 `Phase1Ecc::decode(被翻转后的原始字, 原 parity).data == A′`，对 F4S 断言 `decode` 状态为 CLEAN 且 data == A′（不成立则 `INVALID_CASE`）。计算 B′ = A′ + X（与 `expectedAdd` 相同的 FP16 运算）；PIM consumer 再算 C′ = 2·B′。若 B′（HOST）或 C′（PIM）与无故障期望逐字节相同，则 `corruption_propagates=false`（FP16 舍入掩蔽）。
2. `held_output_corrupt`：仅当输出**未发布**时计算。B 区起始地址为 `codec.encode({0,0,0,256,0})`；用 `codec.decode(bAddress + corrupt_burst*32)` 得到 tuple，从对应 Bank 的 `phase1Storage().readRawData(key)` 读出 4 个 codeword，拼成 32 B 与 `expectedB[corrupt_burst]` 比较：不同为 true，相同为 false，任一 key 不存在为 null。
3. `report_region_ids`：从 `reports.csv` 读出的全部 region_id 列表；`corrupt_bank`、`witness_bank`；`report_covers_corrupt_bank` = 是否有某个 bank 粒度 region_id 对应 corrupt_bank（region_id 与 bank 的换算必须照抄 `Phase1Reports.cpp` 的实现，不要猜）。

**结果分类 `escape_outcome`（按顺序判定，命中即停）：**

| 顺序 | 条件 | 类别 |
|---|---|---|
| 1 | timeout | `TIMEOUT` |
| 2 | 评估器断言失败 | `INVALID_CASE` |
| 3 | `corruption_propagates == false` | `MASKED` |
| 4 | `consumed && output_changed` | `CORRUPT_CONSUMED` |
| 5 | `!published && lock_cycle != 0 && held_output_corrupt == true` | `PREVENTED` |
| 6 | 其他任何情况 | `UNCLASSIFIED`（contract 失败，需调查） |

原有的 `outcome_class`（TIMELY_CONTROL / LATE_CONTROL / NO_REPORT …）照常计算和输出，用于和 Fig. 3 对照。

**`contract_pass` 条件（全部成立）：**

- 不超时；`escape_outcome ∈ {PREVENTED, CORRUPT_CONSUMED, MASKED}`；`internal_ue == 0`；
- F4S：`delivered_reports == 0` 且 `detected_events == 0`；
- F3M：`detected_events >= 1`，且 truth 中该 burst 的解码状态为 CORRECTED；
- F1W_F4S：`detected_events >= 1`（来自 witness），corrupt_burst 不产生检测事件；
- runtime 对象不持有 truth、golden、FaultSchedule 的引用（沿用 P1-C 的隔离写法，不要新增引用）。

**`metrics.json` 新增字段：** `fault_kind, escape_placement, witness_burst, corrupt_burst, witness_bank, corrupt_bank, corruption_propagates, held_output_corrupt, escape_outcome, report_region_ids, report_covers_corrupt_bank`。原 P1-C 字段全部保留。

### 4.4 矩阵

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

### 4.5 预注册假设（检验对象，不是结果）

- H-G1：F3M 的 PREVENTED 格局与 Fig. 3 同一格子的 TIMELY 比例一致；迟到的格子变成 CORRUPT_CONSUMED。
- H-G2：F4S 在所有 D/H 下都是 CORRUPT_CONSUMED 或 MASKED，PREVENTED=0，因为没有报告。
- H-G3：F1W_F4S 的 SAME_BANK 与 DIFF_BANK 结果相同，因为当前 runtime 收到任何报告就锁住整个 kernel 输出；DIFF_BANK 的 `report_covers_corrupt_bank` 全为 false。这说明如果将来只按报告区域做选择性恢复，DIFF_BANK 的损坏会被漏掉。

结果与假设不符时如实报告，不调参数。

---

## 5. P1-H：报告序列能区分什么

### 5.1 C++ 改动（默认关闭，默认行为必须逐位不变）

1. **scrub 开关**：在 `Bank` 中增加 `void setPhase1Scrub(bool enabled)` 和计数器 `uint64_t phase1ScrubWrites() const`，成员默认 `false`。在 `Bank::read` 受保护路径中，当 `result.status == Phase1EccStatus::CORRECTED` 且开关为 true 时，调用 `phase1Storage_.writeLegal(key, result.data)` 并计数。不产生额外总线事务，不增加周期（ASSUMED，理想化零成本）。开关为 false 时不得执行任何新代码路径。
2. **codeword 粒度**：在 `Phase1Reports` 中让 granularity 额外接受 `"codeword"`，此时 `region_type="codeword"`，`region_id` 为 `(channel, actual_bank, row, burst_column, codeword_index)` 的确定性线性编码，公式写进代码注释和协议文档。bank/page 的行为不变。这是 ASSUMED 接口。

改完立即执行 §3.2 的回归对比，必须 `REGRESSION=PASS`。

### 5.2 fixture

新建 `simulator/src/tests/Phase1G11SequenceTestCases.cpp`，`TEST(Phase1G11SequenceFixture, RunCase)`。只用主机读写路径，复用 `Phase1G4TestCases.cpp` 里的 host write/read 写法（`memory.addTransaction(true/false, address, payload)`，约第 140–170 行），先读懂再复用，不要凭函数名猜。

**参数：** `scenario`、`granularity ∈ {bank,page,codeword}`、`scrub ∈ {0,1}`、`seed`、`bank_p`、`bank_r`、`rounds=4`、`transport_delay_cycles=0`、`service_interval_cycles=1`、`report_queue_capacity=64`、`watchdog_cycles=200000`。profile 固定 LOCAL_VISIBLE。

**三个存储位置**（地址用 `codec.encode(Phase1AddrTuple{0,0,bank,row,col})`，并做 decode round-trip 断言）：

- P = (bank_p, row 0, col 0)
- Q = (bank_p, row 1, col 0)，与 P 同 bank、不同 row
- R = (bank_r, row 0, col 0)，不同 bank

**流程：**

1. 主机合法写入 P、Q、R，数据用确定性模式（例如 `0x0123456789ABCDEF ^ (seed * 0x9E3779B97F4A7C15)` 依次填 4 个 codeword），运行到 idle。
2. 对 `k = 0 .. rounds-1`：先执行该轮预定的注入；再按 **P、Q、R 的固定顺序**各主机读一次；推进模拟直到 idle 且报告队列清空（`reports.pendingCount()==0`）。
3. 每轮读取次数在所有场景中相同，避免读取次数本身泄露场景信息。

**场景表（`round: burst bitInBurst`）：**

| 场景 | 对应论文中的情形 | 注入 | scrub |
|---|---|---|---|
| S1_SINGLE | 一个持续错误位 | r0: P 15 | 0, 1 |
| S2_DIFF_CW | 不同 codeword 的独立错误（同 burst） | r0: P 15；r1: P 79 | 0, 1 |
| S3_DIFF_BURST_SAME_BANK | 不同 codeword 的独立错误（同 bank 不同 row） | r0: P 15；r1: Q 15 | 0, 1 |
| S4_SAME_CW_TWO_BITS | **同一 codeword 内两个不同易翻转位（危险情形）** | r0: P 15；r1: P 14 | 0, 1 |
| S5_DIFF_BANK | 不同 bank | r0: P 15；r1: R 15 | 0, 1 |
| S6_SAME_BIT_RECUR | 同一位反复翻转 | r0: P 15；r1: P 15 | **仅 1** |

S6 只在 scrub=1 时运行：scrub 已把第一次翻转修好，第二次注入才代表“再次翻转”；scrub=0 时 XOR 注入会把位翻回去，没有意义。S6 在 r1 注入前必须断言 P 的原始 cw0 数据位 15 已等于初始值，否则 `INVALID_CASE`。

**输出：** `reports.csv`（公开字段）、`truth.csv`（每次注入一行 INJECTION，每次检测一行 DETECTION）、`metrics.json`（`scenario, granularity, scrub, seed, bank_p, bank_r, rounds, detected_events, delivered_reports, internal_ce, internal_ue, scrub_writes, report_signature, contract_pass, timeout`）。`report_signature` 是按 `report_seq` 排序后的 `error_class:region_id` 用 `;` 连接的字符串，不含 report_cycle。

**contract_pass：** 不超时；每轮注入实际发生；`delivered_reports == detected_events`（Q=64 时不应丢报告）；每条报告都对应 truth 中一次真实检测。

### 5.3 矩阵

seed 0..9；`bank_p`、`bank_r` 在 `expand_supplement.py` 中用 `numpy.random.Generator(PCG64(seed))` 生成：`bank_p = rng.integers(0,16)`，`bank_r = (bank_p + 1 + rng.integers(0,15)) % 16`。每个 seed：3 粒度 ×（scrub=0 的 5 个场景 + scrub=1 的 6 个场景）= 33；总数 **330**。smoke：seed 0 的全部 33 个。

### 5.4 分析定义（在 G15 实现）

- **不可区分**：同一 seed、同一 (granularity, scrub) 下，两个场景的 `report_signature` 完全相同。对每个场景对统计 10 个 seed 中不可区分的个数 k/10。另输出一列 `identical_with_timing`（签名中加入相对于第一份报告的 report_cycle 差值），只放 CSV，不进主图。
- **CoF 式规则**：`cof_fire` = 存在某个 region_id 收到 ≥2 份 CE。按 (scenario, granularity, scrub) 统计 fire 次数 / 10。
- `ue_seen` = 是否出现 UE 报告。
- 危险真值：只有 S4 属于“同一 codeword 内积累多个易翻转位”。

### 5.5 预注册假设（检验对象，不是结果）

- H-H1：scrub=1、bank 粒度下，S2、S3、S4、S6 两两不可区分。
- H-H2：scrub=1、codeword 粒度下，S4 与 S6 仍不可区分（区分它们需要位位置或 syndrome，威胁模型假设接口不提供）。
- H-H3：scrub=0 时 S4 出现 UE，因而可区分，但此时错误已不可纠正。
- H-H4：CoF 式规则在 S2/S3/S4（scrub=1）上同样触发，不能把危险情形与良性情形分开。

---

## 6. 流水线脚本（G14–G15）

不要改动 `scripts/expand_suite.py`、`aggregate.py`、`plot_paper.py` 的行为（它们写死了 paper 路径）。新建独立脚本：

1. **`configs/suite_supplement.yaml`**：两个 group `P1G`、`P1H`，写入 §4.4、§5.3 的全部参数，外加 `smoke: true/false` 开关或单独的 `suite_supplement_smoke.yaml`。
2. **`scripts/expand_supplement.py`**：可以 `import` 或复制 `expand_suite.py` 中的 `add_case`、`target_burst_for`、`base_params`，但 `RUN_ROOT=runs/supplement`、`CONFIG_ROOT=configs/generated/supplement`、manifest 写到 `configs/generated/supplement-manifest.json`。group 标签用 `"P1-G Escape"`、`"P1-H Sequence"`。未知 key、缺失 key 报错。打印各组数量，并断言 P1G=4320、P1H=330（smoke 为 24、33）。
3. **`scripts/run_suite.py`**：在 `validate_case` 中新增两个分支，分别调用 `validate_p1g`、`validate_p1h`，检查 §4.3、§5.2 的 contract 条件，并从保存的文件**重新计算**（不要只信 metrics.json）：`truth.csv` 的注入行数、`reports.csv` 的行数与 `delivered_reports` 一致、`escape_outcome` 与 published/consumed/output_changed 的逻辑一致。不改其他分支。
4. 运行：

```bash
.venv/bin/python scripts/expand_supplement.py --suite configs/suite_supplement.yaml
.venv/bin/python scripts/run_suite.py --manifest configs/generated/supplement-manifest.json \
    --results-dir results/supplement --jobs 8 --resume
```

5. **`scripts/aggregate_supplement.py`**：读 `runs/supplement`，输出
   - `results/supplement/figure_data/fig7_escape.csv`：每 case 一行，列 `n_tiles, consumer, seed, fault_kind, escape_placement, delay_ratio, hold_ratio, transport_delay_cycles, hold_cycles, outcome_class, escape_outcome, corruption_propagates, held_output_corrupt, published, consumed, output_changed, report_covers_corrupt_bank`；
   - `fig8_signatures.csv`：每 case 一行，含 `report_signature`；
   - `fig8_pairs.csv`：每个 (granularity, scrub, scenario_i, scenario_j) 一行，含 `indistinguishable_seeds, n_seeds, identical_with_timing_seeds`；
   - `fig8_cof_rule.csv`：每个 (granularity, scrub, scenario) 一行，含 `cof_fire_seeds, ue_seen_seeds, n_seeds`；
   - `results/supplement/statistics_supplement.csv`：每个 P1-G 格子 × fault 配置 × (n_tiles, consumer) 的 CORRUPT_CONSUMED、PREVENTED、MASKED 计数和 Wilson 95% CI。分母规则：PREVENTED 与 CORRUPT_CONSUMED 的比例用 `n − MASKED` 作分母，同时列出 MASKED 和总数 n。另有 F3M 的 PREVENTED 比例与 `results/figure_data/fig3_control_window.csv` 同格 TIMELY 比例的对照列。Wilson 公式和 bootstrap 约定照抄原手册 §9.1。
   - manifest 对账：总数 = SUCCESS + FAIL + TIMEOUT + MISSING，失败 case 单列 `results/supplement/failure_cases.csv`，不得删除。
6. **`scripts/plot_supplement.py`**：只读 `results/supplement/figure_data/*.csv`，绘图样式照抄 `plot_paper.py` 的 rcParams 和配色，输出 `figures/plot_manifest_supplement.json`（输入/输出 SHA256、过滤条件、样本数）。

---

## 7. 图规范（G15）

**Fig. 7 `fig07_escape_efficacy.{pdf,svg,png}`**（双栏宽 7 inch）
- 1 行 4 个面板，依次为 F3M、F1W_F4S SAME_BANK、F1W_F4S DIFF_BANK、F4S；数据取 n_tiles=8、HOST consumer。
- 每个面板是热图：x = H/T_ref（0, 0.25, 1），y = D/T_ref（0, 0.5, 1，从上到下递增，与 Fig. 3 方向一致），颜色 = CORRUPT_CONSUMED 比例（分母 n − MASKED），色条 0–1，单一顺序色（白→深红）。
- 格内文字两行：`C k/n′` 和 `P k/n′`（C = corrupt consumed，P = prevented，n′ = n − MASKED）。若 MASKED>0，在格子右下角加小字 `m=<MASKED>`。
- 图注写明：F3M 与 F4S 的坏数据相同，仅差是否有报告。

**Fig. S2 `figS2_escape_by_config.*`**：4 行（n_tiles × consumer）× 4 列（fault 配置），格式同 Fig. 7。

**Fig. 8 `fig08_report_ambiguity.*`**（双栏宽）
- 2 行（scrub=0、scrub=1）× 3 列（bank、page、codeword）。
- 每个面板是场景 × 场景矩阵（S1…S6），颜色 = 不可区分的 seed 比例（0 白 → 1 深蓝），格内写 `k/10`。scrub=0 的 S6 行和列用斜线填充并标 `N/A`，不得涂成 0。
- S4 的行标签加粗或标星号，图注说明 S4 是危险情形。
- 在图注或报告表格中列出 `fig8_cof_rule.csv` 的结果（CoF 规则在哪些场景触发）。

**通用检查：** 渲染后用 PNG 逐张看。**图例和注释不得遮挡任何数据点或柱子**（原 Fig. 5、Fig. 6 有这个问题，不要重复）。灰度副本存到 `figures/gray/`，新文件名不得与原文件冲突。在 `figures/captions.md` 末尾追加三张图的条目，格式与已有条目相同（Source、Matrix、Cell text、Sample、Denominator、Failure handling、Model assumption）。

---

## 8. G16：修改报告

只改下列位置；原有数字、表格、结论一律不改。所有新数字必须来自 `results/supplement/` 下的 CSV，并在句末给出 CSV 文件名。下面尖括号是待填的实际值，**没有跑出来的值不得填写**。

`report/phase1_results.md`：

1. **§1 Research question and scope**：在 scope 列表末尾增加两条：
   - `supplement P1-G: constructed ECC escapes (miscorrection F3M, silent F4S, CE witness + silent) to test whether timely control withholds an actually corrupted output;`
   - `supplement P1-H: multi-report sequences under bank/page/codeword granularity with and without scrub, to test what coarse reports can distinguish.`
2. **§3.1**：在 G8 manifest 表之后新增“Supplement manifest”小表（P1-G 4320、P1-H 330，以及 SUCCESS/FAIL/TIMEOUT 计数），并写明它是独立 manifest，不计入 2602。
3. **新增 §4.7 “P1-G (supplement): does timely control stop a corrupted value?”**，按这个结构写：
   - 设计一段：位模式、F3M 与 F4S 坏数据相同、评估器只读检查 held output；
   - 结果表：按 fault 配置汇总全部格子的 PREVENTED / CORRUPT_CONSUMED / MASKED 计数（n_tiles=8 HOST 一张；其余配置一句话指向 Fig. S2）；
   - 与 Fig. 3 的对照：F3M 的 PREVENTED 比例与同格 TIMELY 比例是否一致（给出不一致格子数 `<k>`/27）；
   - F4S 结论；SAME_BANK 与 DIFF_BANK 的比较，以及 `report_covers_corrupt_bank` 的含义；
   - Evidence 行列出 CSV 和图文件。
4. **新增 §4.8 “P1-H (supplement): what a sequence of coarse reports can and cannot distinguish”**：设计一段；按 (scrub, granularity) 写不可区分的场景组（例如 “S2≡S3≡S4≡S6, 10/10 seeds”，以实际数据为准）；CoF 规则触发表；UE 出现情况；Evidence 行。
5. **§5**：增加一段 “Supplement: manifest N, success, failure, timeout”。
6. **§6 Figures and data**：表中追加 Fig. 7、Fig. 8、Fig. S2 三行。
7. **§7 Platform capability judgment**：在 “Within the model, Phase 1 supports …” 列表后，按实际结果追加一到两条（例如“及时控制可阻止 constructed escape 的错误输出被消费，条件是存在同 kernel 内的报告”）。在 “It does **not** establish” 列表中追加：`that coarse (bank/page/codeword) reports identify whether multiple susceptible bits share one codeword; P1-H shows <实际结论>.` 原有条目一条都不删。
8. **§8 Limitations**：追加：scrub 理想化零成本；codeword 粒度是 ASSUMED 接口；F3M/F4S 是构造模式，不是 Rowhammer 概率；注入在 kernel 开始前一次性完成；runtime 锁定粒度是整个 kernel。
9. **§9 Reproducibility**：追加补充实验的命令序列（expand_supplement → run_suite → aggregate_supplement → plot_supplement）。

`report/phase1_results.tex`：用英文做同样的增补（新增两个 subsection、三个 figure 环境、限制条目）。

`docs/g16_supplement_results.md`：完整记录补充实验：预注册假设逐条判定（SUPPORTED / NOT SUPPORTED / INCONCLUSIVE，附数据）、偏离预注册的地方、失败 case 列表、回归检查结果。

**禁止写法：**
- 不得写“证明了可以防御 Rowhammer”或“prevents silent corruption”。只能写“在构造的漏检模式与给定 D/H 下，<k>/<n> 个错误输出在消费前被扣留”。
- 不得把 P1-H 的不可区分结论推广到真实 HBM 报告接口。
- 不得把 codeword 粒度写成真实平台能力。

---

## 9. G16：收尾

1. 再跑一次 §3.2 回归对比，必须 `REGRESSION=PASS`。
2. 生成补丁快照：用与原工程相同的方法（参见 `docs/implementation_log.md` 中 `source_g8.patch` 的生成命令）生成 `environment/source_g16.patch`，并更新累计的 `environment/source.patch`；**新增的未跟踪文件也必须包含在补丁里**。
3. 在干净目录重放补丁并构建，跑 P1-G smoke 和 P1-H smoke，结果与 `runs/supplement` 中同 run_id 的 metrics 一致（忽略 wall 时间）。
4. 更新 `README.md`：增加补充实验的一条 smoke 命令和一条完整复现命令。
5. 更新 `docs/STATUS.md`：G10–G16 状态表、证据表、各补丁 SHA256、协议文档 SHA256。
6. 不要求重新打包 `artifact/`；如果重新打包，必须生成新的文件名，不得覆盖旧 tar.gz 和清单。

---

## 10. 常见错误与固定处理

| 现象 | 先检查 | 处理 |
|---|---|---|
| F3M 没有 CE 或返回数据不是原值 ⊕{0,1,4,10} | bitInBurst 是否落在 cw0；是否误用了会重新编码的写接口 | 回到 G11 测试，不改 ECC |
| F4S 产生了报告 | 注入的位是否正好 4 个；是否有其他 burst 被误注入 | 检查 truth.csv 的 INJECTION 行 |
| 所有 F1W_F4S 的 corrupt_burst 等于 witness_burst | 搜索起点是否从 target_burst+1 开始 | 修搜索逻辑并加断言 `corrupt_burst != witness_burst` |
| `held_output_corrupt` 总是 null | B 区 row 是否为 256；是否在发布后才读；key 构造是否用实际 bank | 照抄 P1-C 的 B 地址写法 |
| PREVENTED 但 held_output_corrupt=false | 损坏被掩蔽，但第 3 步判定漏掉了 | 检查 `corruption_propagates` 的计算 |
| scrub=0 时回归出现差异 | 新代码路径没有被开关完全隔离 | 修到 0 差异为止，不得放宽对比 |
| P1-H 里 S6 的 r1 断言失败 | scrub 是否在 CORRECTED 时真的调用了 writeLegal | 查 `phase1ScrubWrites()` 计数 |
| 跑补充矩阵时 `results/g8` 被改写 | 是否忘了传 `--results-dir` | 从备份恢复冻结文件，并检查 run_suite 默认值 |
| 图中某格为空或显示 0% | 是否把 n′=0（全部 MASKED）画成 0 | n′=0 时画斜线并标 `all masked` |

**不允许的“修复”**（与原手册 §12 相同，另加）：更换位模式让结果更好看；把 MASKED 并入 PREVENTED；只报告 n_tiles=8 而隐藏其他配置；让 runtime 读取 `corrupt_burst` 或 truth；修改冻结清单中的任何文件。

---

## 11. 最终验收清单

- [ ] G11：Python 与 C++ 的位模式测试全部通过，与 §4.1 表格一致。
- [ ] 回归对比在 G10（改代码前）、G13（改代码后）、G16（收尾）三次都是 `REGRESSION=PASS`。
- [ ] P1-G 4320 个、P1-H 330 个 case 全部执行；失败与超时单列，没有删除。
- [ ] runtime 隔离：新 fixture 中 runtime 不持有 truth、golden、注入位置的引用。
- [ ] `escape_outcome` 由 run_suite 从保存文件独立复算并一致。
- [ ] Fig. 7、Fig. 8、Fig. S2 只由 figure_data CSV 生成；图例不遮挡数据；灰度可读；captions.md 已追加。
- [ ] 报告 §1、§3.1、§4.7、§4.8、§5、§6、§7、§8、§9 和 tex 已按 §8 修改，原有内容未被改动。
- [ ] `docs/g16_supplement_results.md` 逐条判定预注册假设。
- [ ] 补丁快照包含全部新增文件；干净重放 smoke 一致。
- [ ] 冻结清单中的文件 SHA256 与 G10 开始时一致（G10 时先记录一份 `results/regression_g10/frozen_sha256.txt`，G16 时比对）。

有任何一项未通过，STATUS 标 incomplete/blocked，写明失败日志位置和下一步，不得用“基本完成”掩盖。

---

## 12. 给执行模型的提示词

**首条提示词：**

```text
你负责为 HBM-PIM Phase 1 补充两组实验（P1-G 漏检有效性、P1-H 报告序列可区分性）。
先完整阅读 PHASE1_SUPPLEMENT_HANDOFF.md，再阅读 pim-phase1/HANDOFF.md 的 §0、§3.3、§6、§9、§12，
以及 pim-phase1/report/phase1_results.md 和 docs/STATUS.md，了解已完成的工作。

本次只完成 G10。不要提前实现后续关卡。
冻结清单（补充手册 §1）中的文件只读；补充实验的一切输出写到 supplement / regression_g10 目录。
修改任何文件前先阅读目标文件和调用路径，不要凭函数名猜 API。
新增接口（scrub、codeword 粒度）一律标为 ASSUMED。

完成后更新 docs/STATUS.md 与 docs/implementation_log.md，报告：
1. 实际改动的文件；2. 执行的命令和退出码；3. 通过/失败的检查；
4. 产物位置；5. 下一关唯一任务。

遇到失败，按补充手册 §10 诊断并修复当前关。不得删除测试、修改预期、
更换位模式、让 runtime 读取真值、伪造数据，或把未执行的命令写成已完成。
只有用户继续指示时才进入下一关。
```

**后续每轮提示词：**

```text
读取 docs/STATUS.md，核对上一关的证据文件确实存在且检查通过；
然后按 PHASE1_SUPPLEMENT_HANDOFF.md 完成下一个未完成的关卡，不跳关。
```

**人工复核重点：** G11 的位模式测试输出、G12 中评估器是否真的只读且不经过 runtime、G13 回归对比结果、G15 的 Fig. 7 分母是否排除了 MASKED。这几处决定补充结论是否可信。

# HBM-PIM Phase 1

本仓库是 HBM-PIM Phase 1 实验框架的实现工作区。当前完成 **G0（环境/基线）**、**G1（源码审计、Phase1 fixture、地址检查、有界 feeder、无故障正确性）**、**G2（事件记录与数据协议）**、**G3（SECDED 独立模块与存储注入）** 、**G4（全路径 ECC 与粗粒度报告）** 、**G5（输出状态与 host/device 门禁）**、**G6（暂停、排空与两 channel 验证）**、**G7（可信输入恢复与状态复位）**、**G8（paper 矩阵展开与完整执行）**、**G9（统计、画图、报告与 artifact）**，以及补充实验 **G10–G16（P1-G 漏检有效性、P1-H 报告序列可区分性、聚合、图与报告）**；Phase 1 G0–G16 已完成。

## 固定上游

| 项目 | 值 |
|---|---|
| 仓库 | `https://github.com/SAITPublic/PIMSimulator.git` |
| 固定提交 | `3703d1f19c8f027360cc33a3243eb271e3bb6898` |
| 固定提交的 tree | `7ac34faf86579aca00f5b357c61b5550d1dff7e7` |
| 校验时 `refs/heads/dev` | 同一提交（见 `logs/g0_ref_check.log`） |
| 数据存储 | 启用，构建参数 `NO_STORAGE=0` |

`NO_STORAGE=0` 是上游 `Sconstruct` 的“启用存储”语义：只有非零参数才会追加 `-DNO_STORAGE`。G0/G1 构建日志中没有该宏。

## 快速复现 G0

```bash
bash scripts/bootstrap.sh --check-only
bash scripts/build.sh --clean            # 会重建 G0 基线二进制，勿覆盖已保存证据
bash scripts/run_baseline.sh
.venv/bin/python scripts/validate_g0_artifacts.py
```

`validate_g0.py` 是进入 G1 前的 live 校验；G1 修改源码后，使用
`validate_g0_artifacts.py` 校验已保存的 G0 二进制/XML/配置证据。

G0 基线源码和配置未修改；`runs/baseline/sim-g0` 保存了 G0 二进制副本。

## 快速复现 G1

G1 的源码快照为 `environment/source_g1.patch`；当前累计补丁为
`environment/source.patch`（G2 之后包含 G1+G2）。

```bash
bash scripts/bootstrap.sh --check-only
# 当前工作区已包含 source.patch 的内容；干净副本重放见下方
(cd simulator && scons NO_STORAGE=0 NO_LIBRARY=1)
.venv/bin/python scripts/run_g1.py
```

`run_g1.py` 会为 1-channel W0/W1、1/2-channel 地址检查分别创建独立进程和 `params.ini`，保存 stdout/stderr、退出码、原始输出和 timing；随后执行 `scripts/g1_validate.py`。结果在 `runs/g1/`、`results/g1/`。

干净副本重放：

```bash
git -C simulator worktree add --detach /tmp/pim-g1-clean 3703d1f19c8f027360cc33a3243eb271e3bb6898
git -C /tmp/pim-g1-clean apply "$(pwd)/environment/source_g1.patch"
(cd /tmp/pim-g1-clean && scons NO_STORAGE=0 NO_LIBRARY=1)
# 使用 clean smoke 的 params.ini 运行 Phase1Fixture.RunCase
```

实际干净重放记录见 `logs/g1_clean_*.log`、`runs/g1/clean_replay/`。

## G1 关键结果

- 新 fixture 固定 1 channel、1 rank、16 banks、8 PIM blocks；容量 256 MiB。2-channel 配置（512 MiB）只用于地址检查，未实现并发暂停。
- W1 输入按 `A[i]=((i+seed) mod 17 - 8)/8`、`X[i]=((3i+seed) mod 13 - 6)/8` 生成；`dim_bursts=n_tiles*16*channels*ranks*8`，每个 burst 16 个 FP16。n_tiles=1/8、seed=0/1/2 的 direct 与 feeder 输出均与独立 Python FP16 参考逐 bit 一致。
- 地址 round-trip：1-channel 48/48、2-channel 96/96；bank 0/3/15，row 0/1/8192/16383，burst column 0/1/16/31；无地址碰撞。
- 有界 feeder：每 channel 最多 8 个未完成事务、每 cycle 最多提交 1 个；控制器拒绝时保留并重试。W1 观测到的 feeder 最大外部待提交队列为 487（n_tiles=1）或 3553（n_tiles=8），最大在途事务 8。
- 周期差异：W0 direct 54 / feeder 55；W1 n_tiles=1 direct 1547 / feeder 2565；n_tiles=8 direct 9990 / feeder 18030。feeder 的 barrier 采用保守的“前一阶段完全静止后再放行”实现，因此周期差异是模型选择，不是硬件测量。
- 原始 `PIMKernelFixture.*` 仍为 5/5 PASS；`PIMBenchFixture.add` 性能阈值失败按 G0 基线保留（1.98597 < 2.0），未修改阈值。

## 快速复现 G2

G2 使用累计补丁 `environment/source.patch`，新增被动 `Phase1Trace` 和
run-directory 数据协议。G1 的补丁快照保留为 `environment/source_g1.patch`。

```bash
(cd simulator && scons NO_STORAGE=0 NO_LIBRARY=1)
.venv/bin/python scripts/run_g2.py
```

`run_g2.py` 运行 W1 n_tiles=1/8、seed=0 的 feeder `ext_off`/`ext_on`，生成
`events.csv`、`reports.csv`、`truth.csv`、`metadata.json`、`metrics.json`、
`validation.json`，并比较输出/周期。验证脚本为 `scripts/validate_run.py`；
schema 为 `configs/schema.json`。G2 结果在 `runs/g2/`、`results/g2/`。

G2 关键结果：

- nt1 ext_off/ext_on/G1 feeder 均为 2565 cycles；nt8 均为 18030 cycles。
- ext_off 与 ext_on 输出逐 bit 相同，且与 G1 feeder 输出相同。
- ext_on 事件：nt1 1449 条、nt8 10605 条；ext_off 只有 header。
- event_seq 从 1 连续；cycle 非递减；同 cycle phase 按
  `ENQUEUE<START<FEEDER<MEMORY<COMPLETE<FINALIZE`。
- PIM 广播事件 `actual_bank` 为空并标 `:PIM_BROADCAST`；PIM 控制事件标
  `:PIM_CONTROL`，不把 packet bank 冒充实际 bank。
- `reports.csv`/`truth.csv` 在 G2 仅表头；没有 ECC、故障注入或报告。
- G2 二进制 `f54c8a...`；G2 累计补丁 `2b7b65...`；G1 快照 `629a36...`。
- 干净副本应用 G2 补丁后构建和 smoke 通过（clean binary `0d8723...`）。

## 快速复现 G3

G3 新增独立 SECDED(72,64) 模型和 storage-level 注入；尚未接入 host/PIM 全路径，
也不产生 CE/UE 报告。

```bash
(cd simulator && scons NO_STORAGE=0 NO_LIBRARY=1)
.venv/bin/python scripts/run_g3.py
```

G3 关键结果：

- 8 个固定 pattern × 全部 72 个单 bit -> 576/576 CORRECTED；全部
  C(72,2)=2556 个双 bit -> 20448/20448 UNCORRECTABLE。
- C++ CSV 共 21032 行；`tests/test_ecc_reference.py` 用独立 Python 72-bit
  参考逐行交叉检查，17 passed。
- F1 witness：`0x0123456789ABCDEF` 的 data bit 15（FP16 lane 0 sign bit）
  被 raw flip 后 parity 不变，读取 CORRECTED 并恢复原值；再次读取仍 CE
  （不自动 scrub）；合法写后 CLEAN。
- G3 二进制 `6b31ba...`；G3 累计补丁 `933b32...`；G2 快照 `2b7b65...`；
  G1 快照 `629a36...`。
- 干净副本应用 G3 补丁后 ECC 3/3、Python 17 passed、PIMKernel 5/5；
  clean binary `97809a...`。

证据：`runs/g3/ecc_cpp_exhaustive.csv`、`runs/g3/f1_witness.json`、
`results/g3/validation.json`、`runs/g3/clean_replay/`、`docs/g3_ecc.md`。

## 快速复现 G4

G4 把 SECDED 接入 host/PIM 存储路径，并增加 bank/page 粗粒度报告队列；
G5 已实现输出门禁和 hold；G6 已完成暂停/排空；G7 已实现可信输入恢复。

```bash
(cd simulator && scons NO_STORAGE=0 NO_LIBRARY=1)
.venv/bin/python scripts/run_g4.py
```

G4 结果：

- P1-A smoke：4 profiles × 4 paths × F0/F1/F2 × bank 0/3/15 = 144/144
  PASS。LOCAL_VISIBLE F1 四路径纠正并送达 CE；LOCAL_HIDDEN F1 内部
  CE 不送达；NATIVE 无 ECC/report；HOST_ONLY PIM 写回返回
  `UNPROTECTED_FORMAT` 而非 CE/UE。
- P1-B smoke：63/63 PASS；D=0/1/16/256/4096 的单事件延迟分别为
  1/1/16/256/4096 cycles；32 次真实 decode burst 产生 32 条 CE，无 drop，
  服务间隔 >= I=8。参数 D/I 是模型配置，不是硬件测量。
- 报告 bank 位置检查使用实际 bank 0/3/15，未使用 broadcast packet bank。
- G4 二进制 `1d3aa6...`；G4 cumulative patch `9c7331...`；G3 snapshot
  `933b32...`。干净副本 selected smoke 7/7、ECC 3/3、PIMKernel 5/5；
  clean binary `0b63dd...`。

证据：`runs/g4/`、`results/g4/`、`docs/g4_ecc_reports.md`、
`runs/g4/clean_replay/`。

## 快速复现 G5

G5 增加输出 buffer 版本状态机、host/PIM consumer gate、SUSPECT/POISONED
锁定和同周期边界测试；G6 已完成暂停/排空；G7 已实现可信输入恢复。

```bash
(cd simulator && scons NO_STORAGE=0 NO_LIBRARY=1)
.venv/bin/python scripts/run_g5.py
```

G5 结果：

- 12 个 W1 smoke case（F0/F1/F2 × H=0/8 × HOST/PIM）全部 PASS。
- F0 在 H 到期后 PUBLISHED；HOST 输出与独立 A+X 参考逐 bit 一致。
- F1 -> SUSPECT，F2 -> POISONED；host/PIM 均 blocked，不产生消费者数据。
- `bypass_assert=true`，旧句柄 stale，runtime 同周期边界单测 PASS。
- G5 二进制 `1a527f...`；G5 cumulative patch `f3d898...`；G4 snapshot
  `9c7331...`。干净副本 12/12 + runtime + ECC 3/3 + PIMKernel 5/5；
  clean binary `5ca0c6...`。

证据：`runs/g5/`、`results/g5/`、`docs/g5_runtime.md`、
`runs/g5/clean_replay/`。

## 快速复现 G9

G9 从已保存的 `results/figure_data/*.csv` 计算统计并绘图，不重跑模拟器 case。

```bash
.venv/bin/python scripts/plot_paper.py --results results --out figures
.venv/bin/python scripts/package_artifact.py
```

G9 结果：

- 242 条统计：连续量 95% bootstrap（10000 次，seed=20261115），
  比例 Wilson 95%；写入 `results/statistics.csv`、`results/statistics.json`。
- 六张必交图 + 补充 robustness 图：PDF/SVG/300 dpi PNG，灰度副本和
  `figures/captions.md`；`figures/plot_manifest.json` 记录输入/输出哈希、
  列、过滤、样本数和聚合。
- 报告：`report/phase1_results.md`、`report/phase1_results.tex`、
  `report/references.bib`。LaTeX 工具链不可用，`.tex` 未本地编译。
- Artifact：`artifact/phase1-artifact.tar.gz`、`artifact/MANIFEST.sha256`、
  `artifact/ARCHIVE.sha256`；manifest 逐文件校验 PASS。
- Artifact SHA256：`ad8e70305909531de06aa0ad7c70b90fb16569e23dc21bf5256eb030c92d6bac`
  （`artifact/phase1-artifact.tar.gz`，约 80 MB；`artifact/ARCHIVE.sha256` 同步记录）。
- 关键结果：P1-C 主矩阵 TIMELY 1118/1800（62.1%）、LATE 682/1800
  （37.9%）、output_changed 0；P1-E 20/20 正例、7/7 expected negative；
  P1-F H0/H0.25T/H1T overhead 0.00%/10.21%/43.96%。

证据：`figures/`、`report/`、`results/statistics.csv`、
`results/validation_summary.json`、`artifact/`。

## 快速复现 G8

G8 将 paper 矩阵展开为 hash-addressed manifest，逐个 case 独立进程执行，
并对账 success / expected-negative / failure / timeout / missing。

```bash
.venv/bin/python scripts/calibrate_g8.py
.venv/bin/python scripts/expand_suite.py --suite configs/suite_paper.yaml
.venv/bin/python scripts/run_suite.py --manifest configs/generated/paper-manifest.json --jobs 8
.venv/bin/python scripts/aggregate.py --runs runs/paper --out results
```

G8 结果：

- manifest **2602 cases**：P1-A 144、P1-B 63、P1-C 1848、P1-D 22、
  P1-E 27、P1-F cost 480、P1-F robustness 18。
- 对账：**2595 SUCCESS + 7 EXPECTED_NEGATIVE + 0 FAIL + 0 TIMEOUT +
  0 MISSING**。
- P1-C：1800 main + 48 LOCAL_HIDDEN/HOST_ONLY controls；outcome
  TIMELY 1118、LATE 682、NO_REPORT 24、FORMAT_UNSUPPORTED 24。
- P1-D：22/22，ACK 后 data delta 0，quiesce 1–402 cycles。
- P1-E：20 正例 + 7 expected negative 全部通过。
- P1-F：480 个 cost cases；H0 0.0%、H0.25T 平均 10.21%、H1T 平均
  43.96% 模拟周期 overhead；robustness 18/18。
- G8 二进制 `ab2165c0...`；G8 patch `93daf08c...`；G7 snapshot
  `d94c5c...`。干净副本 8/8 selected smoke + ECC 3/3 + PIMKernel 5/5；
  clean binary `71b11534...`。

证据：`configs/generated/paper-manifest.json`、`runs/paper/`、
`results/g8/`、`results/figure_data/`、`docs/g8_paper_matrix.md`。

## 快速复现 G7

G7 在暂停/排空之后实现整 kernel 恢复：可信 A/X checkpoint -> 丢弃旧未提交
工作 -> 退出 PIM 模式并复位 CRF/GRF/SRF/中间/控制状态 -> 通过模拟 host
writes 恢复 A/X -> 重执行整个 W1 -> 发布新版本 -> HOST 逐 bit 验证。

```bash
(cd simulator && scons NO_STORAGE=0 NO_LIBRARY=1)
.venv/bin/python scripts/run_g7.py
```

G7 结果：

- 20/20 report-triggered 正例：`n_tiles∈{1,8}`，seeds=0..9；W1 F1 CE
  report 触发暂停，旧输出保持 `SUSPECT` 且未发布。
- 4/4 `RECOVERY_PROBE`：显式 probe 触发暂停，旧输出保持 `PRODUCING`；
  probe 不注入存储故障，单独标注为接口测试。
- checkpoint：A+X 共 8192 B（n_tiles=1）/65536 B（n_tiles=8）；恢复写
  256/2048 次、8192/65536 B，raw A/X 逐 codeword 与 checkpoint 匹配。
- 状态复位：恢复前写入已知非默认 GRF/SRF/中间/CRF 值；reset 后归零，
  report cases 从 `HAB_PIM` 回到 `SB`。
- 丢弃旧未提交 descriptor：report 68/446，probe 103/481；已接受事务由
  G6 drain 完成。
- 版本 `1 -> 2`，旧 handle stale；新版本 `PUBLISHED`，HOST 输出与独立
  Python A+X 参考逐 bit 一致（20/20 + 4/4）。
- 五类负例实际返回 `CHECKPOINT_MISSING`、`VERSION_MISMATCH`、
  `CHECKPOINT_CORRUPT`、`ALREADY_COMMITTED`、`STALE_HANDLE`。
- 两个必要初始化负面对照：`NO_INPUT_RESTORE` 保留 raw A 错误并产生 CE，
  输出不发布；`NO_STATE_RESET` 停在 `HAB_PIM`，restore 超时被捕获。
- G7 二进制 `0bb330...`；G7 patch `d94c5c...`；G6 snapshot `53b188...`。
  干净副本 31/31 + ECC 3/3 + PIMKernel 5/5 + checkpoint 1/1；clean binary
  `b24a34...`。

证据：`runs/g7/`、`results/g7/`、`docs/g7_recovery.md`、
`runs/g7/clean_replay/`。

## 快速复现 G6

G6 增加 whole-channel quiesce：停止目标 channel 的新 feeder 提交，排空已接受/
在途事务，冻结外部未提交 descriptor，ACK 后目标数据访问为 0；不冻结 DRAM
时钟/REF；G7 的恢复在其上完成。

```bash
(cd simulator && scons NO_STORAGE=0 NO_LIBRARY=1)
.venv/bin/python scripts/run_g6.py
```

G6 结果：

- 16/16 case PASS：4 个暂停时点 × K∈{1,8,32}，实际 CE L∈{0,8,64}，
  以及两 channel smoke。
- 每个 case ACK 后 data_delta=0；8000-cycle 观察窗 clock_delta=8000，
  refresh 计数不倒退并观察到增加。
- 外部 descriptor 保持冻结（synthetic 约 2528/2529；two-channel 56）。
- 实际 CE：report 1305，L=0/8/64 request 1305/1313/1369，ACK
  1354/1354/1422，ACK 后无新数据访问。
- 两 channel：channel0 8->8，channel1 8->64；不声称零性能影响。
- G6 二进制 `13cf8a...`；G6 patch `53b188...`；G5 snapshot `f3d898...`。
  干净副本 16/16 + ECC 3/3 + PIMKernel 5/5；clean binary `7f531f...`。

证据：`runs/g6/`、`results/g6/`、`docs/g6_pause.md`、
`runs/g6/clean_replay/`。

## 补充实验 P1-G/P1-H（G10–G16）

补充实验使用独立 manifest，不计入 paper 的 2602 cases。P1-G 构造 F3M/F4S
ECC 漏检和 F1W_F4S 见证，检查及时控制是否扣留了一个真正错误的输出；P1-H 比较
bank/page/codeword 粒度、scrub 0/1 下 S1–S6 的公开报告序列。

```bash
# 环境检查与回归基线
.venv/bin/python scripts/run_suite.py \
  --manifest configs/generated/regression_g13-manifest.json \
  --results-dir results/regression_g13 --runs-dir runs/regression_g13 --jobs 8
.venv/bin/python scripts/compare_regression.py \
  --manifest configs/generated/regression_g13-manifest.json \
  --out-dir results/regression_g13

# smoke：24 P1-G + 33 P1-H
.venv/bin/python scripts/expand_supplement.py \
  --suite configs/suite_supplement.yaml --smoke
.venv/bin/python scripts/run_suite.py \
  --manifest configs/generated/supplement-smoke-manifest.json \
  --results-dir results/supplement/smoke \
  --runs-dir runs/supplement/smoke --jobs 8 --resume

# 全量：4320 P1-G + 330 P1-H
.venv/bin/python scripts/expand_supplement.py --suite configs/suite_supplement.yaml
.venv/bin/python scripts/run_suite.py \
  --manifest configs/generated/supplement-manifest.json \
  --results-dir results/supplement --runs-dir runs/supplement --jobs 8 --resume
.venv/bin/python scripts/aggregate_supplement.py
.venv/bin/python scripts/plot_supplement.py
```

全量对账：4650 = 4320 P1-G + 330 P1-H，全部 SUCCESS，0 FAIL/TIMEOUT/MISSING。
P1-G 总结果为 PREVENTED 2166、CORRUPT_CONSUMED 1920、MASKED 234。补充图：
`figures/fig07_escape_efficacy.*`、`fig08_report_ambiguity.*`、
`figS2_escape_by_config.*`；统计与图源数据在 `results/supplement/`。
`codeword` 报告粒度和 `scrub` 均为 ASSUMED 模型接口；F3M/F4S 是构造位模式，
不是 Rowhammer 概率模型。补丁快照为 `environment/source_g16.patch`。
G16 的 clean worktree build/replay 按用户“无需重跑代码”的指示未执行，STATUS 与
`docs/g16_supplement_results.md` 已如实标注。

## 目录

```text
configs/       1/2-channel Phase1 system ini、G2 schema
environment/   G0/G1/G2 lock、补丁、二进制/配置哈希
simulator/     固定上游提交 + G1/G2 源码修改（见 environment/source.patch）
scripts/       bootstrap、G0 基线、G1/G2/G3 runner、validators
runs/baseline/ G0 只读基线
runs/g1/       G1 运行、地址 CSV、clean replay
runs/g2/       G2 事件/协议运行、clean replay
runs/g3/       G3 ECC/witness、clean replay
runs/g4/       G4 全路径/报告 smoke、clean replay
runs/g5/       G5 状态/门禁 smoke、clean replay
runs/g6/       G6 pause/quiesce、two-channel、clean replay
runs/g7/       G7 恢复正例/负例/clean replay
runs/g8/       G8 calibration/clean replay
runs/paper/    G8 paper 逐 case 运行
runs/supplement/ G10–G16 P1-G/P1-H 逐 case 运行
results/supplement/ 补充图源数据、统计与对账
results/figure_data/ G8/G9 图源数据
results/statistics.csv G9 统计
figures/      G9 六图 + 补充图、灰度图、captions、plot manifest
report/       Phase 1 论文式报告 Markdown/LaTeX/BibTeX
artifact/     G9 artifact、SHA256 清单和复现日志
results/g1/    G1 summary 与 validation
results/g2/    G2 comparison 与 validation
results/g3/    G3 ECC summary 与 validation
results/g4/    G4 P1-A/P1-B smoke 与 validation
results/g5/    G5 状态/门禁 smoke 与 validation
results/g6/    G6 pause/quiesce 与 validation
results/g7/    G7 恢复 summary/validation 与 clean replay validation
results/g8/    G8 reconciliation、run_metrics、summary、failure_cases
results/figure_data/ G8 聚合后的六类图源数据（G9 绘图输入）
logs/          实际命令、stdout/stderr、退出码和诊断日志
docs/          source_audit.md、g2_protocol.md、g3_ecc.md、g4_ecc_reports.md、g5_runtime.md、g6_pause.md、g7_recovery.md、g8_paper_matrix.md、STATUS.md、implementation_log.md
```

## 限制

- G1 只验证无故障 W0/W1、地址映射和提交机制；G2 只增加被动事件记录和数据协议；G3 只实现独立 SECDED 与 storage-level F1 witness。
- G5 已完成输出门禁；G6 完成 channel 级暂停/排空；G7 完成可信输入恢复与状态复位；G8 完成 paper 矩阵执行与对账；G9 完成统计、绘图、报告和 artifact。G7 的 checkpoint 是模型内受保护 host 区域，恢复写入不建模真实 host/互连开销；G8 的 P1-C 使用 direct memory path，P1-F cost 使用 bounded feeder。
- feeder 的 barrier 采用保守的阶段性静止，不等价于完整硬件 barrier；周期差异已记录。
- 地址检查是 Scheme8 模型检查，不代表真实 HBM 的所有地址映射细节。
- G2 的 `reports.csv`/`truth.csv` 在无故障时是 header-only；G4 的 P1-A injection 写 evaluator truth 行，runtime 不读取 truth。
- `PIMBenchFixture.add` 的已知基线失败保留，不通过修改阈值消除。
- 补充实验的 F3M/F4S 是 CoF 式构造位模式，不是 Rowhammer 概率；`MASKED` 是 FP16
  舍入，不计为 PREVENTED。
- 补充实验的 codeword 报告粒度是 ASSUMED 接口，scrub 是理想化零成本模型；P1-H
  每个 (scenario, granularity, scrub) 只有 10 seeds，不能外推为真实 HBM 报告能力。

上游代码保留原始 `LICENSE-PIMSimulator` 与 `LICENSE-DRAMSIM2`；本项目新增内容不改变上游许可证。

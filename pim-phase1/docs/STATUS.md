# Phase 1 STATUS

- 最后更新：2026-11-16（UTC）
- 当前关卡：**G16 已完成（报告/图注/tex、最终回归、补丁；clean replay 未按用户指示重跑）**
- 下一关：**无（G0–G16 报告与证据已收尾）**
- 工作目录：`/opt/data/data/workspace-dynamic/Rowhammer/pim-phase1`
- 补丁说明：`environment/source_gN.patch` 是各关不可变快照；`environment/source.patch` 是当前累计补丁，后续关卡会更新。各关表中列出的补丁哈希指当时快照/当时累计值。

## 关卡状态

| 关卡 | 状态 | 说明 |
|---|---|---|
| G0 | **PASS** | 固定提交、干净构建、规定的原功能测试、环境锁定均完成；额外全量测试中的上游性能阈值失败单独记录 |
| G1 | **PASS** | 源码审计、Phase1 fixture、地址 round-trip、有界 feeder、W0/W1 无故障逐 bit 验证、补丁干净重放均完成 |
| G2 | **PASS** | Phase1Trace、events/reports/truth/metadata/metrics/validation 数据协议、schema 与序号检查、EXT_OFF/EXT_ON 行为对照、干净重放均完成 |
| G3 | **PASS** | SECDED(72,64) 独立模块、穷举单/双 bit、Python 独立交叉检查、F1 storage witness、干净重放均完成 |
| G4 | **PASS** | 全路径 profile/access hooks、粗粒度 bank/page 报告队列、P1-A 144/144、P1-B 63/63 smoke、位置与广播检查、干净重放均完成 |
| G5 | **PASS** | 输出版本状态机、host/PIM 门禁、SUSPECT/POISONED 锁定、同周期边界、旧句柄与旁路负测、干净重放均完成 |
| G6 | **PASS** | 整 channel quiesce/drain/ACK、外部队列冻结、ACK 后数据访问 0、REF/时钟继续、两 channel 独立进度、干净重放均完成 |
| G7 | **PASS** | 可信输入 checkpoint、暂停触发、旧工作丢弃、PIM 状态复位、A/X 模拟 host 写恢复、整 W1 重执行、新版本发布与逐 bit 验证；20/20 正例、4/4 probe、5/5 负例状态、2/2 必要初始化负面对照、干净重放均完成 |
| G8 | **PASS** | paper manifest 2602 cases：2595 SUCCESS + 7 EXPECTED_NEGATIVE + 0 FAIL/TIMEOUT/MISSING；P1-A/B/C/D/E 与 P1-F cost/robustness 全部执行并聚合 |
| G9 | **PASS** | 242 条统计、六张主图 + 补充 robustness 图、plot manifest、论文式报告、80 MB artifact 与 SHA256 清单 |
| G10 | **PASS** | 补充实验环境恢复、240-case 回归基线在改代码前 0 差异、预注册协议冻结；未实现 G11–G16 |
| G11 | **PASS** | Python 独立参考与 C++ SECDED 均验证 F1/F3M/F4S/S4/cw1 位模式；25 Python tests + 2 C++ tests 通过；未修改 ECC 实现 |
| G12 | **PASS** | `Phase1G10EscapeFixture` 实现 F3M/F4S/F1W_F4S 注入、只读评估器与分类；24/24 smoke contract_pass；未实现 scrub/codeword/全矩阵 |
| G13 | **PASS** | 默认关闭 scrub、codeword 粒度（ASSUMED）、P1-H fixture；33/33 smoke contract_pass；240-case 回归 `REGRESSION=PASS`；未实现全矩阵/聚合/图 |
| G14 | **PASS** | 补充矩阵展开 4650（P1-G 4320、P1-H 330）；4650/4650 SUCCESS，0 FAIL/TIMEOUT/MISSING；独立复核 0 不一致；未实现统计/图/报告 |
| G15 | **PASS** | figure_data、432 条统计、Fig.7/8/S2、plot manifest 与灰度图；manifest 对账 0 缺失；补充图注已追加，报告/tex 未改 |
| G16 | **PASS（报告/分析）** | 报告 Markdown/LaTeX、补充图注、g16 结果文档完成；G10/G13 最终回归 PASS；`source_g16.patch` 生成；clean replay 按用户“无需重跑代码”未执行 |

## G16 验收证据

| 检查 | 结果 | 证据 |
|---|---|---|
| Markdown 报告 | PASS：新增 §1 scope、§3.1 supplement manifest、§4.7 P1-G、§4.8 P1-H、§5 failure、§6 figures、§7 capability、§8 limitations、§9 reproducibility；原有数字/表格未改 | `report/phase1_results.md` |
| LaTeX 报告 | PASS：英文对应 subsection、P1-G/P1-H 表、Fig.7/Fig.8/Fig.S2 环境、failure/capability/limitations/reproducibility；未本地编译（环境无 LaTeX） | `report/phase1_results.tex` |
| 图注 | PASS：Fig.7/Fig.8/Fig.S2 的 Source、Matrix、Cell text、Sample、Denominator、Failure handling、Model assumption 已写明 | `figures/captions.md` |
| G16 结果文档 | PASS：H-G1..H-G3、H-H1..H-H4 逐条判定；偏离预注册、失败 case、回归和边界已记录 | `docs/g16_supplement_results.md` |
| 最终回归 | PASS：G10 240-case `REGRESSION=PASS`；G13 240-case `REGRESSION=PASS` | `results/regression_g10/`、`results/regression_g13/` |
| 冻结文件 | PASS（允许例外）：33,867/33,868 未变；唯一变化是允许追加的 `figures/captions.md` | `results/regression_g10/frozen_sha256.txt` |
| 补丁快照 | PASS：`environment/source_g16.patch`，41 个文件、8,440 行，SHA256 `c8cf0ce1...`；累计 `environment/source.patch` 同哈希 | `environment/source_g16.patch`、`environment/source.patch` |
| 补丁内容检查 | PASS：包含 Bank/Phase1Reports 修改和全部新增 Phase1 fixture/test；`git apply --check --reverse` exit 0；`git diff --check` exit 0 | `logs/g16_patch.log` |
| README | PASS：增加补充实验 smoke/全量命令、结果摘要、目录和限制 | `README.md` |
| 最终状态 | PASS（报告/分析）：报告与证据收尾；**clean replay 未执行**，未把未执行命令写成完成 | `docs/STATUS.md` |
| Artifact | 未重新打包；旧 `artifact/` tar.gz 与清单未被覆盖 | `artifact/` |

### G16 结果摘要

- 报告新增 P1-G/P1-H 两节，并把 supplement manifest、失败对账、能力边界、限制和复现命令写入；原有 paper 的 2602-case 数字与结论未改。
- P1-G 全量 4320：PREVENTED 2166、CORRUPT_CONSUMED 1920、MASKED 234；F4S 无报告，未掩蔽的损坏全部被消费。
- P1-H 330：bank/page 不能把 S4 从良性情形中分出来；codeword 能分出 S2/S3，但仍不能分出 S4/S6；CoF 规则会同时触发危险与良性情形；scrub=0 的 S4 以 UE 出现，但已不可纠正。
- 最终回归与补丁检查通过；`source_g16.patch` 可反向检查通过，累计 `source.patch` 已更新。
- 按用户“无需重跑代码”，没有执行 G16 clean worktree build/replay；STATUS 与 `docs/g16_supplement_results.md` 均明确标注，不伪造 clean replay 结果。
- G0–G16 的 paper/frozen 数据、图、artifact 未被覆盖。

### G16 关键文件

- 报告：`report/phase1_results.md`、`report/phase1_results.tex`
- 图注：`figures/captions.md`
- 补充结果：`docs/g16_supplement_results.md`
- 补丁：`environment/source_g16.patch`、`environment/source.patch`
- 最终回归：`results/regression_g10/compare.csv`、`results/regression_g13/compare.csv`
- 日志：`logs/g16_patch.log`

## G15 验收证据

| 检查 | 结果 | 证据 |
|---|---|---|
| P1-G figure_data | PASS：4320 行，144 个 D/H×fault×size×consumer 格子，每格 30 seeds；outcome 计数与 metrics 一致 | `results/supplement/figure_data/fig7_escape.csv` |
| P1-H figure_data | PASS：330 signatures、90 个场景对、36 个 (granularity,scrub,scenario) 行 | `fig8_signatures.csv`、`fig8_pairs.csv`、`fig8_cof_rule.csv` |
| 统计表 | PASS：432 行 = 144 格 × {PREVENTED, CORRUPT_CONSUMED, MASKED}；Wilson 95%；分母 n−MASKED；同时列 masked/total | `results/supplement/statistics_supplement.csv` |
| F3M 对照 | PASS：36 个 F3M 格子的 PREVENTED 与 fig3 同格 TIMELY 对照列已生成；8/36 格存在差异（多为 MASKED 造成的分母变化），留给 G16 如实报告 | `statistics_supplement.csv` |
| manifest 对账 | PASS：4650 = 4320 P1-G + 330 P1-H；missing=0；runner counts 4650 SUCCESS | `results/supplement/reconciliation_supplement.json` |
| Fig.7 | PASS：1×4 面板（F3M、F1W SAME、F1W DIFF、F4S），n_tiles=8 HOST；C/P 只显示计数，面板副标题给 n′/M；去掉了每格重复的 m 和分母；色条 0–1 | `figures/fig07_escape_efficacy.{pdf,svg,png}` |
| Fig. S2 | PASS：4×4（n_tiles×consumer × fault）；只在外缘显示 tick labels，统一 D/T_ref 和 C/P 说明，面板副标题给 n′/M；全部 4320 cases | `figures/figS2_escape_by_config.{pdf,svg,png}` |
| Fig.8 | PASS：2×3（scrub × granularity），仅画非对角场景对；零值留空、对角省略、S6/scrub=0 斜线；颜色=不可区分 seed 比例；S4* 加粗 | `figures/fig08_report_ambiguity.{pdf,svg,png}` |
| 图注 | PASS：`figures/captions.md` 追加 Supplement figures、Fig.7、Fig.8、Fig.S2 条目，明确 S1–S6、C/P、n′、分母、N/A、scrub/codeword 假设 | `figures/captions.md` |
| 灰度与可读性 | PASS：三张图均有 300 dpi PNG、灰度为 `figures/gray/*.png`；逐张查看，文字不再跨格，图例/注释不遮挡数据 | `figures/`、`figures/gray/` |
| plot manifest | PASS：输入仅 `results/supplement/figure_data/*.csv`；记录输入/输出 SHA256、列、过滤、样本数、聚合 | `figures/plot_manifest_supplement.json` |
| 独立复核 | PASS：figure_data 行数、格子计数、stats 分子、P1-H signature、plot manifest 输入/输出哈希全部一致；`G15_VALIDATE=PASS` | `logs/g15_validate.log` |
| 报告未改 | PASS：未修改 `report/phase1_results.md/.tex` 或已有 paper 图 | 文件状态 |
| 默认行为 | PASS：G14 结果、G13/G10 回归与冻结文件未变 | `results/supplement/`、`results/regression_g10/`、`results/regression_g13/` |

### G15 结果摘要

- 生成 4320 行 P1-G 图数据和 330 行 P1-H 签名数据；统计表 432 行，覆盖 144 个格子。
- P1-G：PREVENTED 2166、CORRUPT_CONSUMED 1920、MASKED 234。
- P1-H 观察（10 seeds）：
  - bank、scrub=1：S2/S3/S4/S6 两两 10/10 不可区分，支持 H-H1；
  - codeword、scrub=1：S4 与 S6 10/10 不可区分，S2/S3 可区分，支持 H-H2；
  - scrub=0：S4 在三种粒度均 10/10 出现 UE，支持 H-H3；
  - bank、scrub=1：S2–S6 的 CoF 规则均 10/10 触发，S1、S5 为 0，支持 H-H4。
- F3M PREVENTED 与 fig3 TIMELY 的逐格对照已写入统计表；8/36 格不一致，G16 需按实际数字报告，不调参。
- Fig.7/S2 的分母为 n−MASKED；MASKED 另列；n′=0 的斜线处理已实现（本矩阵无 n′=0 格）。
- 可读性修订：Fig.7/Fig.S2 的格内只保留 C/P 计数，n′/M 放在面板副标题；Fig.S2 只在外缘显示 tick labels；Fig.8 只画非对角场景对，零值留空、对角省略、S6/scrub=0 斜线，不再重复 N/A。
- `figures/captions.md` 已追加三张补充图的完整图注（S1–S6 定义、C/P、n′、分母、N/A 和模型假设）；`report/phase1_results.md/.tex` 未改。
- 下一关为 G16。

### G15 关键文件

- 聚合：`scripts/aggregate_supplement.py`、`logs/g15_aggregate.log`、`logs/g15_validate.log`
- 图数据：`results/supplement/figure_data/fig7_escape.csv`、`fig8_signatures.csv`、`fig8_pairs.csv`、`fig8_cof_rule.csv`
- 统计：`results/supplement/statistics_supplement.csv`、`reconciliation_supplement.json`
- 绘图：`scripts/plot_supplement.py`、`logs/g15_plot.log`
- 图：`figures/fig07_escape_efficacy.*`、`figures/fig08_report_ambiguity.*`、`figures/figS2_escape_by_config.*`
- 灰度：`figures/gray/fig07_escape_efficacy.png`、`fig08_report_ambiguity.png`、`figS2_escape_by_config.png`
- Manifest：`figures/plot_manifest_supplement.json`
- 图注：`figures/captions.md`

## G14 验收证据

| 检查 | 结果 | 证据 |
|---|---|---|
| suite 定义 | PASS：`configs/suite_supplement.yaml`，`smoke: false`；P1G/P1H 全部参数 | `configs/suite_supplement.yaml` |
| expander | PASS：P1G 4320、P1H 330；smoke 断言 24/33；run_id 4650 个且唯一 | `scripts/expand_supplement.py`、`logs/g14_expand.log` |
| manifest | PASS：`supplement-manifest.json`，`expected_total=4650`，`expected_by_group={P1-G Escape:4320, P1-H Sequence:330}` | `configs/generated/supplement-manifest.json`，SHA256 `781e560e...` |
| case 配置 | PASS：`configs/generated/supplement/<run_id>/config.json` 4650 个 | `configs/generated/supplement/` |
| run_suite 验证分支 | PASS：新增 `validate_p1g`/`validate_p1h`，从 truth/reports/metrics 独立复算 contract、注入数、报告数、signature、region_id、scrub 计数和 escape 逻辑 | `scripts/run_suite.py` |
| 全量运行 | PASS：4650/4650 SUCCESS；0 FAIL、0 TIMEOUT、0 MISSING | `results/supplement/validation.json`、`summary.csv`、`run_metrics.csv` |
| 分组对账 | PASS：P1-G 4320/4320、P1-H 330/330，contract_pass 全部 | `results/supplement/summary.csv` |
| 失败样本 | PASS：`failure_cases.csv` 仅 header，0 条；未删除任何 case | `results/supplement/failure_cases.csv` |
| 独立复核 | PASS：逐 case 重跑验证器，missing=0、failed=0、checks_failed={}；`G14_VALIDATE=PASS` | `logs/g14_validate.log` |
| P1-G 结果分布 | PREVENTED 2166、CORRUPT_CONSUMED 1920、MASKED 234（共 4320） | 每 case `metrics.json` |
| P1-H 结果分布 | 330/330 contract；S1 60、S2 60、S3 60、S4 60、S5 60、S6 30（按全部粒度/scrub） | 每 case `metrics.json` |
| 默认行为保持 | PASS：G10 240-case `REGRESSION=PASS`；G13 240-case `REGRESSION=PASS`；G10 冻结 33,868/33,868 | `results/regression_g10/`、`results/regression_g13/` |
| 构建 | G14 未改 C++；二进制仍为 `1662ad05...` | `environment/binary.sha256` |
| 补丁 | G14 未生成独立快照；累计 `environment/source.patch` 仍为 `93daf08c...`，G16 统一更新 | 补充手册 §9 |

### G14 结果摘要

- 全量补充矩阵 4650 cases：P1-G 4320、P1-H 330；manifest 对账为 `4650 = 4650 SUCCESS + 0 FAIL + 0 TIMEOUT + 0 MISSING`。
- P1-G：PREVENTED 2166、CORRUPT_CONSUMED 1920、MASKED 234。
  - F3M（1080）：PREVENTED 706、CORRUPT_CONSUMED 302、MASKED 72。
  - F4S（1080）：CORRUPT_CONSUMED 1008、MASKED 72。
  - F1W_F4S（2160）：PREVENTED 1460、CORRUPT_CONSUMED 610、MASKED 90。
- P1-H：330/330 contract；S1/S2/S3/S4/S5 各 60、S6 30（覆盖 3 粒度与 scrub 组合）。
- 全量运行无失败、无超时、无缺失；`failure_cases.csv` 只有表头。
- G14 未实现统计、聚合、figure_data、图或报告修改；下一关为 G15。
- 运行器 metadata 的 `gate` 字段沿用 G8 名称；本关以 supplement manifest、validation 和独立复核为准，不把该字段当作关卡证据。

### G14 关键文件

- suite：`configs/suite_supplement.yaml`（SHA256 `3934dc67...`）
- expander：`scripts/expand_supplement.py`
- runner：`scripts/run_suite.py`（`validate_p1g`/`validate_p1h`）
- manifest：`configs/generated/supplement-manifest.json`（SHA256 `781e560e...`）
- configs：`configs/generated/supplement/`
- runs：`runs/supplement/<4650 run_id>/`
- results：`results/supplement/run_metrics.csv`、`summary.csv`、`validation.json`、`failure_cases.csv`
- 日志：`logs/g14_expand.log`、`logs/g14_run_full.log`、`logs/g14_validate.log`

## G13 验收证据

| 检查 | 结果 | 证据 |
|---|---|---|
| `Bank` scrub 接口 | PASS：`setPhase1Scrub(bool)`、`phase1ScrubWrites()`；默认 false；仅在 `CORRECTED` 且开关 true 时 `writeLegal` 并计数；无额外总线事务/周期 | `simulator/src/Bank.h`、`Bank.cpp` |
| scrub 默认隔离 | PASS：G13 240-case 回归与 paper 结果 0 差异；默认关闭路径不改变输出/报告/周期 | `results/regression_g13/compare.csv` |
| codeword 粒度 | PASS：`Phase1Reports` 接受 `"codeword"`，`region_type="codeword"`，公式 `(((channel*num_banks+bank)*num_rows+row)*burst_columns+col)*4+cw`；bank/page 行为不变 | `simulator/src/Phase1Reports.cpp`、`docs/g10_supplement_protocol.md` |
| P1-H fixture | PASS：`TEST(Phase1G11SequenceFixture, RunCase)`；P/Q/R 地址 round-trip；固定 P,Q,R 读取顺序；4 rounds；S1–S6/scrub 参数校验 | `simulator/src/tests/Phase1G11SequenceTestCases.cpp` |
| S6 scrub 断言 | PASS：r1 注入前 P 的 cw0 data bit 15 已恢复为初始值；S6 的 `scrub_writes=2` | `runs/supplement/g13_smoke/*/metrics.json` |
| smoke 数量 | PASS：33 cases = 3 粒度 ×（S1–S5 各 scrub=0/1 + S6 scrub=1） | `runs/supplement/g13_smoke/manifest.json` |
| smoke 结果 | PASS：33/33 contract_pass；0 FAIL/TIMEOUT；注入数 S1=1、其余=2；delivered=detected | `results/supplement/g13_smoke_summary.json`、`g13_smoke_run_metrics.csv` |
| scrub 计数 | PASS：scrub=0 全为 0；scrub=1 时 S1=1，S2–S6=2 | 每 case `metrics.json` |
| 独立复核 | PASS：reports/truth 行数、report_signature、scrub_writes、bank/codeword region_id 公式全部一致 | `logs/g13_validate.log` |
| 默认回归 | PASS：240/240 SUCCESS；`REGRESSION=PASS`，0 差异 | `results/regression_g13/validation.json`、`compare.csv` |
| G10 基线保持 | PASS：G10 冻结文件 33,868/33,868 一致；G10 的 240-case 结果未被覆盖 | `results/regression_g10/frozen_sha256.txt` |
| 构建 | PASS：`bash scripts/build.sh` exit 0；最终二进制 SHA256 `1662ad05...` | `logs/g13_build.log`、`environment/binary.sha256` |
| 协议补充 | PASS：codeword 公式已写入协议；当前 SHA256 `86d2336e...`（G10 原版为 `8758ead...`） | `docs/g10_supplement_protocol.md` |
| 补丁 | G13 未生成独立快照；累计 `environment/source.patch` 仍为 `93daf08c...`，G16 统一更新 | 补充手册 §9 |

### G13 结果摘要

- scrub 是默认关闭的理想化零成本接口；仅当 `CORRECTED` 且开关开启时直接重写 codeword，不产生总线事务或周期。
- codeword 粒度是 ASSUMED 报告接口；smoke 中 bank 粒度有两个 report 时 region_id 相同，codeword 粒度能区分 S2（cw0/cw1）与 S3（不同 row/burst），但 S4 与 S6 仍相同。
- smoke 观察：
  - bank、scrub=1：S2/S3/S4/S6 的 signature 均为 `CE:13;CE:13`，在 smoke 子集内支持 H-H1；
  - codeword、scrub=1：S4/S6 均为同一 codeword 的 `CE:...;CE:...`，S2/S3 可区分，支持 H-H2；
  - scrub=0：S4 在 bank/page/codeword 三种粒度都出现 UE，支持 H-H3；
  - bank、scrub=1：S2/S3/S4/S6 都有同一 region 收到 2 份 CE，CoF 式规则同样触发，支持 H-H4；S1 只有 1 份，S5 是不同 region。
- 这些是 smoke（seed 0）观察，不是完整 10-seed 统计；G14/G15 才做完整判定。
- G13 未实现完整 4320/330 矩阵、聚合、图或报告；下一关为 G14。

### G13 关键文件

- scrub：`simulator/src/Bank.h`、`simulator/src/Bank.cpp`
- codeword：`simulator/src/Phase1Reports.cpp`
- fixture：`simulator/src/tests/Phase1G11SequenceTestCases.cpp`（SHA256 `d00aa023...`）
- smoke：`runs/supplement/g13_smoke/manifest.json`、`runs/supplement/g13_smoke/<33 run_id>/`
- 汇总：`results/supplement/g13_smoke_summary.json`、`results/supplement/g13_smoke_run_metrics.csv`
- 回归：`configs/generated/regression_g13-manifest.json`、`runs/regression_g13/`、`results/regression_g13/`
- 日志：`logs/g13_build.log`、`logs/g13_smoke.log`、`logs/g13_validate.log`、`logs/g13_regression_run.log`、`logs/g13_regression_compare.log`

## G12 验收证据

| 检查 | 结果 | 证据 |
|---|---|---|
| G10/G11 复核 | PASS：G10 冻结文件 33,868/33,868 一致，240-case `REGRESSION=PASS`；G11 Python 25 passed、C++ `Phase1Ecc.*` 2/2 passed | `results/regression_g10/`、`tests/test_ecc_reference.py`、`logs/g11_*` |
| 新 fixture | PASS：`TEST(Phase1G10EscapeFixture, RunCase)`，文件 898 行；主流程复用 P1-C 的 preload → 注入 → ADD → producer → hold → consumer | `simulator/src/tests/Phase1G10EscapeTestCases.cpp` |
| 参数校验 | PASS：`fault_kind∈{F3M,F4S,F1W_F4S}`；F3M/F4S 要求 `escape_placement=NONE`，F1W 要求 SAME_BANK/DIFF_BANK；profile 固定 LOCAL_VISIBLE | fixture 内校验 |
| 注入规则 | PASS：F3M 翻 {0,1,4}；F4S 翻 {0,1,4,10}；F1W 先翻 witness {15}，再从 target+1 确定性搜索第一个同/不同 bank 的 corrupt_burst 翻 {0,1,4,10}；corrupt≠witness | `truth.csv`、`metrics.json` |
| 只读评估器 | PASS：`corruption_propagates` 用 A′ 重算 B′/C′；`held_output_corrupt` 只在未发布时从 B 区 raw storage 读取 4 个 codeword；`report_region_ids` 来自公开 reports | 每 case `metrics.json` |
| escape 分类 | PASS：24/24 命中 PREVENTED / CORRUPT_CONSUMED / MASKED；0 UNCLASSIFIED、0 INVALID_CASE、0 TIMEOUT | `results/supplement/g12_smoke_summary.json` |
| smoke 结果 | PASS：24/24 contract_pass；PREVENTED 8、CORRUPT_CONSUMED 14、MASKED 2 | `results/supplement/g12_smoke_summary.json`、`g12_smoke_run_metrics.csv` |
| F3M | PASS：D=0 的 3 个 case 全部 PREVENTED；D=731 的 3 个全部 CORRUPT_CONSUMED；truth decoder_status=CORRECTED | `runs/supplement/g12_smoke/*/truth.csv` |
| F4S | PASS：6/6 CORRUPT_CONSUMED；`detected_events=0`、`delivered_reports=0`、`internal_ue=0` | 同上 |
| F1W_F4S | PASS：SAME_BANK 与 DIFF_BANK 均产生 witness CE；`report_covers_corrupt_bank` 分别为 true/false；DIFF 的损坏未被公开报告覆盖 | 同上 |
| 独立复核 | PASS：从保存的 truth/reports/metrics 重新计算，0 不一致；reports 行数等于 delivered_reports，truth 注入行数正确 | `logs/g12_validate.log` |
| 构建 | PASS：`bash scripts/build.sh` exit 0；二进制 SHA256 `1c2d3fc6...` | `logs/g12_build.log`、`environment/binary.sha256` |
| ECC 实现未改 | PASS：`Phase1Ecc.cpp` 仍为 `2e5f9fa3...` | 文件哈希 |
| 补丁 | G12 未生成独立快照；G16 统一生成 `source_g16.patch` 并更新累计补丁 | 补充手册 §9 |

### G12 结果摘要

- smoke 严格为 24 cases：n_tiles=1、HOST、seed 0..2、四种 fault 配置、D/T∈{0,1}、H/T=0；T_ref=731。
- F3M：D=0 在报告到达后锁住输出，held B 确实损坏（PREVENTED）；D=731 报告迟到，损坏输出被消费（CORRUPT_CONSUMED）。这验证了“及时控制拦住的确实是一个错误值”，而不是已纠正值。
- F4S：无报告、无 UE，6/6 损坏输出被消费；与 F3M 的坏数据相同，差别只在报告。
- F1W_F4S：witness CE 能触发同样的 kernel 锁定；SAME_BANK 的报告覆盖 corrupt bank，DIFF_BANK 不覆盖。smoke 中 seed=1 的 SAME/DIFF 选择了不同 corrupt_burst，FP16 舍入使 SAME_BANK 的两个 case 成为 MASKED，因此 smoke 子集内 H-G3 的“结果相同”未被完全复现；这是预注册假设的观察，留给 G14/G16 按完整矩阵判定，未调参。
- 未实现 scrub、codeword 粒度、P1-H、完整 4320 矩阵或聚合/绘图；下一关为 G13。

### G12 关键文件

- fixture：`simulator/src/tests/Phase1G10EscapeTestCases.cpp`（SHA256 `bdb3945d...`）
- smoke manifest：`runs/supplement/g12_smoke/manifest.json`
- smoke runs：`runs/supplement/g12_smoke/<24 run_id>/`
- 汇总：`results/supplement/g12_smoke_summary.json`、`results/supplement/g12_smoke_run_metrics.csv`
- 日志：`logs/g12_build.log`、`logs/g12_smoke.log`、`logs/g12_validate.log`
- 新二进制：`environment/binary.sha256`，`1c2d3fc6...`

## G11 验收证据

| 检查 | 结果 | 证据 |
|---|---|---|
| Python `test_escape_patterns` | PASS：8 个数据 pattern × 5 种位模式；F1=CORRECTED/原值，F3M=CORRECTED/原值⊕{0,1,4,10}，F4S=CLEAN/同一坏值，S4=UE，cw1=CORRECTED | `tests/test_ecc_reference.py`、`logs/g11_ecc_python.log` |
| Python 全套 ECC 参考测试 | PASS：25 passed in 1.79s（含原有 clean/single/double/crosscheck） | `logs/g11_ecc_python.log` |
| C++ `TEST(Phase1Ecc, EscapePatterns)` | PASS：8 个数据 pattern 全部断言通过，映射 0→3、1→5、4→9、10→15、14→20、15→21；bitInBurst=79→cw1/data bit 15 | `simulator/src/tests/Phase1EccTestCases.cpp`、`logs/g11_ecc_cpp.log` |
| C++ `Phase1Ecc.*` | PASS：2/2 tests（ExhaustiveSingleAndDoubleBit、EscapePatterns），exit 0 | `logs/g11_ecc_cpp.log` |
| F3M/F4S 同坏值 | PASS：两者返回数据均等于原值⊕位{0,1,4,10}；F3M syndrome=15/overall=1/CORRECTED，F4S syndrome=0/overall=0/CLEAN | 两份测试日志 |
| S4 第二位 | PASS：{15}+{14} → syndrome=1、overall=0、UNCORRECTABLE | 两份测试日志 |
| ECC 实现未改 | PASS：`Phase1Ecc.cpp` SHA256 `2e5f9fa3...` 保持不变；只改/新增测试 | `simulator/src/Phase1Ecc.cpp` |
| 构建 | PASS：`bash scripts/build.sh` exit 0；新二进制 SHA256 `493335a1...`（测试代码变化导致，运行行为未改） | `logs/g11_build.log`、`environment/binary.sha256` |
| 补丁 | G11 未生成独立快照；按补充手册 §9，G16 统一生成 `environment/source_g16.patch` 并更新累计 `environment/source.patch` | 补充手册 §9 |

### G11 结果摘要

- Python 独立参考（不是 C++ 逐行翻译）验证了预注册的 5 种模式在 8 个固定数据字上的状态、返回数据、syndrome 和整体校验。
- C++ 使用 `Phase1Ecc::encodeParity/decode` 验证同一组模式；`Phase1Ecc.*` 2/2 通过。
- 关键对照成立：F3M 与 F4S 交给消费者的坏数据完全相同，都是原值⊕位{0,1,4,10}；F3M 产生 CE 且被错误“纠正”，F4S 为 CLEAN 且无报告。
- `bitInBurst=79` 确实对应 codeword 1 的 data bit 15；其单比特结果为 CORRECTED/原值。
- G11 没有实现 P1-G fixture、评估器、scrub、codeword 粒度或补充矩阵；下一关为 G12。
- 未修改 `Phase1Ecc.cpp`；若任何一行与 §4.1 不符本应 BLOCKED，本次全部符合。

### G11 关键文件

- Python 测试：`tests/test_ecc_reference.py`（`test_escape_patterns`，文件 SHA256 `f0e1a602...`）
- C++ 测试：`simulator/src/tests/Phase1EccTestCases.cpp`（`TEST(Phase1Ecc, EscapePatterns)`，文件 SHA256 `5a576fff...`）
- 日志：`logs/g11_ecc_python.log`、`logs/g11_ecc_cpp.log`、`logs/g11_build.log`
- 新二进制：`environment/binary.sha256`，`493335a1...`

## G10 验收证据

| 检查 | 结果 | 证据 |
|---|---|---|
| Python 环境 | PASS：numpy、pandas、yaml、matplotlib、jsonschema、pytest 全部可导入 | `logs/g10_python_env.log` |
| venv 重建 | PASS：按 `environment/requirements.lock.txt` 重建；`pip check` 无损坏依赖；`pip freeze` 与 lock 逐行一致（diff exit 0） | `logs/g10_python_env.log` |
| 系统工具 | PASS：补装 `python3 3.10.12`、`python3-venv`、`python3-pip`、`scons 4.0.1`、`libgtest-dev 1.11.0` | `logs/g10_apt_install.log` |
| 固定上游 git 元数据恢复 | PASS：`HEAD=3703d1f19c8f027360cc33a3243eb271e3bb6898`，tree `7ac34faf86579aca00f5b357c61b5550d1dff7e7`；94 blobs、17 subtrees + root tree 的哈希逐一与 GitHub API 核对；工作区保留 39 个修改/未跟踪项 | `logs/g10_git_restore.log`、`simulator/.git` |
| 构建 | PASS：`bash scripts/build.sh` exit 0，二进制 SHA256 `ab2165c0...`，与 G8 二进制一致 | `logs/build_20261116T100819Z.log`、`environment/binary.sha256` |
| 冻结哈希记录 | PASS：33,868 个现存冻结文件记录 SHA256，G10 结束时复核 33,868/33,868 一致 | `results/regression_g10/frozen_sha256.txt` |
| 冻结清单差异 | PASS：`environment/source_g0.patch` 在工作区实际不存在（G0 无源码改动）；记录文件已如实注明，未用空文件或伪造哈希补位 | `results/regression_g10/frozen_sha256.txt` 头部 |
| 回归 manifest | PASS：240 cases = P1-A 144 + P1-C seed0 LOCAL_VISIBLE 60 + P1-E positive seed0 2 + P1-F Cost seed0 16 + P1-F Robustness 18 | `configs/generated/regression_g10-manifest.json` |
| 回归执行 | PASS：240/240 SUCCESS，0 FAIL，0 TIMEOUT，0 expected negative | `results/regression_g10/validation.json`、`summary.csv`、`run_metrics.csv` |
| 回归比较 | PASS：`REGRESSION=PASS`，0 差异；metrics 逐键比较，output.bin/reports.csv SHA256 相等 | `results/regression_g10/compare.csv`、`compare_summary.txt` |
| 预注册协议 | PASS：P1-G/P1-H 位模式、分类、矩阵、假设已冻结；G10 原版 SHA256 `8758ead33623453f9eea18cf7d8dd328a97eb19dc07470e9e8a39988ee3e8247`（G13 追加 codeword 公式后当前哈希见 G13） | `docs/g10_supplement_protocol.md` |
| 默认行为保持 | PASS：`scripts/run_suite.py` 的 `--results-dir` 默认仍为 `results/g8`；新增 `--runs-dir` 默认仍为 `runs/paper`，G10 显式重定向以保护冻结目录 | `scripts/run_suite.py` |
| 源码未改 | PASS：`environment/source.patch` 保持 `93daf08c...`；G10 未修改 `Phase1Ecc.cpp`、`Bank.*`、`Phase1Reports.*` 或任何 C++ 行为 | `environment/source.patch` |

### G10 结果摘要

- 补充实验环境已恢复到与 G8 相同的 Python 依赖与二进制；`build.sh` 产出 `ab2165c0...`，与 G8 记录一致。
- 上传步骤曾移除 `simulator/.git`；G10 通过 GitHub API 逐对象重建固定提交的 commit/tree/blob，commit 与 tree 哈希均与锁定值一致，工作区修改状态保持 39 项。该恢复只重建上游历史元数据，不覆盖任何工作区源文件。
- 回归 manifest 覆盖每个 paper group 的确定性代表：P1-A 全部、P1-C seed 0 主矩阵、P1-E positive seed 0、P1-F Cost seed 0、P1-F Robustness 全部，共 240 cases。
- 改代码前回归 240/240 通过，`REGRESSION=PASS`，证明比较脚本本身有效；这是 G13/G16 的基线。
- G10 未实现 scrub、codeword 粒度、P1-G/P1-H fixture 或任何补充矩阵运行；下一关为 G11。
- `results/regression_g10/frozen_sha256.txt` 记录 33,868 个冻结文件哈希；G10 结束时全部一致。
- G10 没有修改 `runs/paper/`、`results/g8/`、`figures/`、`configs/generated/paper*`、`artifact/` 或 `environment/source_g1..g8.patch`。

### G10 关键文件

- 协议：`docs/g10_supplement_protocol.md`
- 回归 manifest：`configs/generated/regression_g10-manifest.json`
- 回归运行：`runs/regression_g10/`（240 个 case 目录）
- 回归结果：`results/regression_g10/run_metrics.csv`、`summary.csv`、`validation.json`、`compare.csv`、`compare_summary.txt`、`frozen_sha256.txt`
- 脚本：`scripts/make_regression_manifest.py`、`scripts/compare_regression.py`、`scripts/run_suite.py`
- 环境日志：`logs/g10_apt_install.log`、`logs/g10_python_env.log`、`logs/g10_git_restore.log`、`logs/g10_regression_run.log`
- 构建日志：`logs/build_20261116T100819Z.log`

## G9 验收证据

| 检查 | 结果 | 证据 |
|---|---|---|
| 统计与 CI | PASS：242 条统计；连续量 bootstrap（10000 次，seed=20261115），比例 Wilson 95% | `results/statistics.csv`、`results/statistics.json` |
| Fig.1 路径覆盖 | PASS：PDF/SVG/PNG，F1/F2 面板，C/R/I/U 计数 | `figures/fig01_path_coverage.*` |
| Fig.2 通知延迟 | PASS：单事件 D 曲线 + 32-read burst 累积交付 | `figures/fig02_report_latency.*` |
| Fig.3 控制窗口 | PASS：2 consumers × 2 sizes 热图，30 seeds/cell | `figures/fig03_control_window.*` |
| Fig.4 静止/独立 channel | PASS：4 synthetic + actual CE + two-channel 进度 | `figures/fig04_quiescence.*` |
| Fig.5 恢复 | PASS：restore/reexec 堆叠 + ACK→publish bootstrap CI + 正负例注记 | `figures/fig05_restore.*` |
| Fig.6 成本 | PASS：配对 overhead + checkpoint/held/parity/metadata 空间 | `figures/fig06_cost.*` |
| 补充 robustness 图 | PASS：Q/I × drop 的交付比例 | `figures/figS1_robustness.*` |
| 图注与数据来源 | PASS：每图数据源、过滤、样本数、单位、模型假设 | `figures/captions.md` |
| Plot manifest | PASS：输入/输出 SHA256、列、过滤、聚合、样本数 | `figures/plot_manifest.json` |
| 视觉检查 | PASS：六张主图 + 补充图逐张检查；灰度副本可读 | `figures/`、`figures/gray/` |
| 论文式报告 | PASS：Markdown 报告 + 英文 LaTeX 源 + BibTeX | `report/phase1_results.md`、`report/phase1_results.tex`、`report/references.bib` |
| Artifact | PASS：36960 files，80 MB tar.gz，SHA256 清单逐文件校验通过 | `artifact/phase1-artifact.tar.gz`、`artifact/MANIFEST.sha256`、`artifact/ARCHIVE.sha256` |
| 补丁重放 smoke | PASS：G8 clean-replay 8/8 selected cases + PIMKernel 5/5 + ECC 3/3 | `runs/g8/clean_replay/`、`logs/g8_clean_smoke.log` |
| G0–G8 回归 | PASS：G0/G1 验证、G8 对账、补丁反向检查 | `logs/g8_final_audit.log` |
| G9 lock | PASS | `environment/g9.lock.json` |

### G9 结果摘要

- P1-A：144/144；F1 在 LOCAL_VISIBLE/LOCAL_HIDDEN 四路径纠正，HOST_ONLY 仅 HREAD；F2 为 UE 或不恢复。
- P1-B：D=0/1 -> 1 cycle，D=16/256/4096 -> 16/256/4096；burst 32/32 交付，I=8 间隔满足。
- P1-C：1800 main 中 TIMELY 1118（62.1%，Wilson 59.8–64.3%）、LATE 682（37.9%，35.7–40.2%）、output_changed 0；48 controls 为 NO_REPORT/FORMAT_UNSUPPORTED。
- P1-D：22/22，ACK 后 data delta 0；quiesce 1–402 cycles；two-channel channel0 0、channel1 56。
- P1-E：20/20 正例；7/7 expected negatives；restore/reexec/recovery 为 557/804/1361 与 4274/4130/8404 cycles。
- P1-F：H0 0.00%、H0.25T 10.21%、H1T 43.96% 模拟周期 overhead；空间 13.6/108.1 KiB；robustness 18/18。
- 报告明确边界：模型 ECC/接口、P1-C direct path、P1-F feeder、一次性故障、无真实 host/互连/OS 成本。

### G9 关键文件

- 统计/绘图：`scripts/plot_paper.py`
- 统计：`results/statistics.csv`、`results/statistics.json`
- 图：`figures/fig01..fig06.*`、`figures/figS1_robustness.*`、`figures/captions.md`、`figures/plot_manifest.json`
- 报告：`report/phase1_results.md`、`report/phase1_results.tex`、`report/references.bib`
- 复现：`scripts/reproduce.sh`
- 打包：`scripts/package_artifact.py`
- Artifact：`artifact/phase1-artifact.tar.gz`、`artifact/MANIFEST.sha256`、`artifact/ARCHIVE.sha256`
- G9 lock：`environment/g9.lock.json`

## G8 验收证据

| 检查 | 结果 | 证据 |
|---|---|---|
| paper matrix 展开 | PASS：2602 cases，7 expected negatives | `configs/generated/paper-manifest.json`、`logs/g8_expand.log` |
| P1-A | PASS：144/144（4 profiles × 4 paths × 3 faults × 3 banks） | `results/g8/summary.csv` |
| P1-B | PASS：63/63（60 D/I/granularity/bank + 3 burst） | `results/g8/summary.csv` |
| P1-C | PASS：1848/1848（1800 main + 48 LOCAL_HIDDEN/HOST_ONLY controls） | `results/figure_data/fig3_control_window.csv` |
| P1-D | PASS：22/22（12 synthetic + 9 actual CE + 1 two-channel） | `results/figure_data/fig4_quiescence.csv` |
| P1-E | PASS：20 positive + 5 status negatives + 2 state/input controls | `results/figure_data/fig5_restore.csv` |
| P1-F cost | PASS：480/480（2 sizes × 2 consumers × 30 seeds × 4 policies） | `results/figure_data/fig6_cost.csv` |
| P1-F robustness | PASS：18/18（Q/I/drop matrix），8 overflow，drop=0.1 固定记录 seq 10/20/30 | `results/figure_data/fig6_robustness.csv` |
| manifest 对账 | PASS：2602 = 2595 SUCCESS + 7 EXPECTED_NEGATIVE + 0 FAIL + 0 TIMEOUT + 0 MISSING | `results/g8/validation.json`、`results/validation_summary.json` |
| P1-C outcome coverage | PASS：TIMELY 1118、LATE 682、NO_REPORT 24、FORMAT_UNSUPPORTED 24 | `results/g8/run_metrics.csv` |
| P1-F cost 结果 | PASS：H0 0.0%、H0.25T 平均 10.21%、H1T 平均 43.96% 模拟周期开销 | `results/figure_data/fig6_cost.csv` |
| T_ref 校准 | PASS：P1-C 731/3339；P1-F 804/4130 | `results/g8/calibration.json`、`logs/g8_calibration.log` |
| G8 runner | PASS 8/8 reconciliation checks，0 failures/timeouts/missing | `results/g8/validation.json`、`logs/g8_run_full.log` |
| 原始 `PIMKernelFixture.*` | PASS 5/5 | `runs/g8/upstream-tests.xml` |
| ECC/Storage 回归 | PASS 3/3；runtime/contract/checkpoint unit PASS | `runs/g8/ecc_regression.log`、`runs/g8/runtime_contract.log` |
| 已知 `PIMBenchFixture.add` 性能失败 | 保留原基线，未修改阈值：1.98597 < 2.0 | `runs/g8/pimbench_add.log` |
| G8 新二进制独立哈希 | PASS | `environment/g8_binary.sha256`：`ab2165c0...`；G7 `0bb330...` 等保留 |
| G8 cumulative source.patch | PASS | `environment/source.patch` SHA256 `93daf08c...`；G7 snapshot `d94c5c...` 保留 |
| 固定提交干净副本应用 G8 补丁并构建 | PASS | `logs/g8_clean_worktree.log`、`g8_clean_apply.log`、`g8_clean_build.log` |
| 干净副本 G8 smoke | PASS：8/8 selected cases（每个 group 至少 1 个）、ECC 3/3、PIMKernel 5/5 | `runs/g8/clean_replay/`、`logs/g8_clean_smoke.log` |
| G0/G1 回归与补丁反向检查 | PASS | `logs/g8_final_audit.log` |

### G8 结果摘要

- 将 paper 矩阵展开为 2602 个 hash-addressed case；每个 case 独立进程、独立输出目录。
- P1-C 使用 PCG64(seed) 在前/中/后三段 A burst 内选择目标；D/H 由 T_ref 校准后取整并写入 config。
- P1-C 覆盖 TIMELY/LATE/NO_REPORT/FORMAT_UNSUPPORTED；LOCAL_HIDDEN 与 HOST_ONLY 控制不计为恢复成功。
- P1-D 22 个 case 的 `data_delta_after_ack=0`；quiesce 1–402 cycles；synthetic 与 actual CE 分开标注。
- P1-E 20 个正例恢复并逐 bit 正确；7 个 expected negative 全部被正确识别。
- P1-F cost 480 个无故障 case 完成配对 overhead；P1-F robustness 18 个 case 完成 Q/I/drop 对账。
- `results/figure_data/` 已生成图源数据；G9 已生成六图、统计、报告和 artifact（`figures/`、`report/`、`results/statistics.csv`）。
- 边界：P1-C 使用 direct memory path（其 T_ref 为 direct 值），不是 bounded feeder；P1-D synthetic 的 L 不适用；P1-F 的成本是模拟周期，不是硬件实测。

### G8 关键文件

- suite：`configs/suite_paper.yaml`
- expander：`scripts/expand_suite.py`
- calibration：`scripts/calibrate_g8.py`
- runner：`scripts/run_suite.py`
- aggregator：`scripts/aggregate.py`
- fixtures：`simulator/src/tests/Phase1G8P1CTestCases.cpp`、`Phase1G8CostTestCases.cpp`、`Phase1G8RobustTestCases.cpp`
- 文档：`docs/g8_paper_matrix.md`
- manifest：`configs/generated/paper-manifest.json`
- 结果：`results/g8/`、`results/`、`results/figure_data/`
- 运行：`runs/paper/`
- 干净重放：`runs/g8/clean_replay/`
- G8 lock：`environment/g8.lock.json`

## G7 验收证据

| 检查 | 结果 | 证据 |
|---|---|---|
| P1-E 正例：n_tiles∈{1,8}，seeds=0..9 | PASS：20/20 report-triggered cases | `results/g7/summary.csv`、`runs/g7/positive/` |
| 显式 `RECOVERY_PROBE` | PASS：4/4 probes（n_tiles=1/8，seeds=0/1），旧输出 `PRODUCING`、无旧报告 | `runs/g7/probe/` |
| 输出恢复前始终暂存 | PASS：`old_published_before_restore=false`；report cases `SUSPECT`，probe cases `PRODUCING` | 每个 case 的 `metrics.json` |
| 旧版本废弃、新版本发布 | PASS：version `1 -> 2`，旧 handle `STALE_HANDLE`，新版本 `PUBLISHED` | `results/g7/summary.csv`、`Phase1Runtime` |
| 可信输入 checkpoint | PASS：A/X 共 8192 B（n_tiles=1）、65536 B（n_tiles=8），带 version 与 checksum | `Phase1CheckpointStore`、`metrics.json` |
| 恢复使用模拟 host writes | PASS：256 写/8192 B（n_tiles=1）、2048 写/65536 B（n_tiles=8）；恢复后 raw A/X 逐 codeword 匹配 | `restore_writes`、`restored_raw_*` |
| 旧未提交工作丢弃 | PASS：report cases 68/446 个 descriptor；probe cases 103/481 个 | `discarded_pending` |
| 暂停触发与 ACK | PASS：report cycle 1305/8938，request 1313/8946，ACK 1354/8987，quiesce 41 cycles | `results/g7/summary.csv` |
| PIM 模式退出/重建 | PASS：report cases `HAB_PIM -> SB`；reset 后 `mode_after_reset=SB` | `mode_before_reset`、`mode_after_reset` |
| CRF/GRF/SRF/中间/控制状态复位 | PASS：已知非默认值存在，reset 后 `zeroPimState()` 为真，`reset_ok=true` | `nondefault_before_reset`、`state_reset_count` |
| 整 W1 重执行 | PASS：`reexec_pending=0`、`reexec_outstanding=0`、`reexec_ce_count=0`、`reexec_ue_count=0` | 每个正例 `metrics.json` |
| 最终逐 bit 正确 | PASS：20/20 report + 4/4 probe；C++ 与独立 Python 比较均 bit-exact | `output.bin`、`results/g7/validation.json` |
| 恢复延迟分解 | PASS：ACK->publish 1361/8404 cycles；restore 557/4274；reexec 804/4130（n_tiles=1/8） | `results/g7/summary.csv` |
| 五类负例状态 | PASS：实际观察到 `CHECKPOINT_MISSING`、`VERSION_MISMATCH`、`CHECKPOINT_CORRUPT`、`ALREADY_COMMITTED`、`STALE_HANDLE` | `runs/g7/negative_statuses/*/metrics.json` |
| 必要初始化负面对照 | PASS：`NO_INPUT_RESTORE` 保留 raw A 错误并产生 2 CE，输出不发布；`NO_STATE_RESET` 停在 `HAB_PIM`，restore 超时被捕获 | `runs/g7/negative_control/` |
| G7 runner | PASS 13/13 aggregate checks，31 cases，0 failures | `results/g7/validation.json`、`logs/g7_run.log` |
| 原始 `PIMKernelFixture.*` | PASS 5/5 | `runs/g7/upstream-tests.xml` |
| ECC/Storage 回归 | PASS 3/3；`Phase1Checkpoint.NegativeStatuses` PASS | `runs/g7/ecc_regression.log`、`runs/g7/checkpoint_unit.log` |
| 已知 `PIMBenchFixture.add` 性能失败 | 保留原基线，未修改阈值：1.98597 < 2.0 | `runs/g7/pimbench_add.log` |
| G7 新二进制独立哈希 | PASS | `environment/g7_binary.sha256`：`0bb330...`；G6 `13cf8a...` 等保留 |
| G7 cumulative source.patch | PASS | `environment/source.patch` SHA256 `d94c5c...`；G6 snapshot `53b188...` 保留 |
| 固定提交干净副本应用 G7 补丁并构建 | PASS | `logs/g7_clean_worktree.log`、`g7_clean_apply.log`、`g7_clean_build.log` |
| 干净副本 G7 smoke | PASS：31/31 cases、ECC 3/3、PIMKernel 5/5、checkpoint 1/1 | `runs/g7/clean_replay/`、`logs/g7_clean_smoke.log` |
| G0/G1 回归与补丁反向检查 | PASS | `logs/g7_final_audit.log` |

### G7 结果摘要

- 实现可信 A/X checkpoint（version + checksum）；恢复源不是 `reference.bin`，runtime 不读取真值。
- report-triggered recovery：W1 的 F1 CE 报告在 1305/8938 cycles 触发暂停，ACK 在 1354/8987，旧输出保持 `SUSPECT` 且从未发布。
- probe recovery：显式 `RECOVERY_PROBE` 在旧工作提交前冻结，旧输出保持 `PRODUCING`，4/4 通过。
- 恢复前设置已知非默认 GRF/SRF/中间/CRF 值；reset 后全部归零，rank 从 `HAB_PIM` 回到 `SB`。
- 通过模拟 host writes 恢复 A/X：n_tiles=1 为 256 writes/8192 B，n_tiles=8 为 2048 writes/65536 B；raw A/X 逐 codeword 与 checkpoint 匹配。
- 旧 kernel 未提交 descriptor 被丢弃（report 68/446；probe 103/481），已接受事务由 G6 drain 完成。
- 新输出 version 从 1 变为 2，旧 handle 失效；整 W1 重执行后 `PUBLISHED`，HOST 读取逐 bit 正确（20/20 + 4/4）。
- 五类负例均返回规定状态；`NO_INPUT_RESTORE` 与 `NO_STATE_RESET` 两个必要初始化负面对照均被捕获。
- 边界：checkpoint 是模型内受保护 host 区域；恢复写入走模拟存储通路但不建模真实 host/互连开销；PIM 模式退出为直接状态复位；一次性故障，不声称持续攻击隔离。

### G7 关键文件

- checkpoint/恢复状态：`simulator/src/Phase1Checkpoint.{h,cpp}`、`simulator/src/Phase1Runtime.{h,cpp}`
- 状态复位/丢弃：`simulator/src/PIMRank.{h,cpp}`、`simulator/src/Phase1Feeder.{h,cpp}`
- fixture：`simulator/src/tests/Phase1G7TestCases.cpp`
- runner：`scripts/run_g7.py`
- 文档：`docs/g7_recovery.md`
- 结果：`results/g7/summary.csv`、`results/g7/validation.json`
- 干净重放：`runs/g7/clean_replay/`
- G7 lock：`environment/g7.lock.json`

## G6 验收证据

| 检查 | 结果 | 证据 |
|---|---|---|
| 整 channel QUIESCE_REQUEST -> drain -> ACK | PASS：16/16 cases | `results/g6/validation.json`、`logs/g6_run.log` |
| ACK 后目标 channel 数据访问 | PASS：全部 `data_delta_after_ack=0` | `results/g6/summary.csv` |
| 外部待提交队列冻结 | PASS：synthetic 2528/2529 左右保持 pending，不丢失 | `runs/g6/synthetic/*/metrics.json` |
| 已接受/在途事务排空 | PASS：ACK 前 outstanding/controller/rank 数据路径清空 | `Phase1PauseController`、`Phase1FeederMemory::channelIdle` |
| 时钟/REF 不被冻结 | PASS：观察窗 8000 cycles，clock_delta=8000；refresh 不倒退并观察到增加 | `results/g6/summary.csv` |
| 四种暂停时点 | PASS：before/queue/inflight/near；K∈{1,8,32} | 12 个 SYNTHETIC cases |
| 实际 CE 触发暂停 | PASS：W1 F1 report -> L=0/8/64 -> request -> ACK | `runs/g6/actual_ce/*/metrics.json` |
| 两 channel 作用域 | PASS：channel0 data 停止，channel1 独立前进 56 次访问 | `runs/g6/two_channel/TWO_CHANNEL/metrics.json` |
| 观察窗长度 | PASS：8000 > max(32,2*tRFC)=700 cycles | 每个 case 的 `observation_cycles` |
| G6 runner | PASS 9/9 checks | `results/g6/validation.json` |
| 原始 `PIMKernelFixture.*` | PASS 5/5 | `runs/g6/upstream-tests.xml` |
| 已知 `PIMBenchFixture.add` 性能失败 | 保留原基线，未修改阈值 | `runs/g6/pimbench_add.log`，1.98597 < 2.0 |
| G6 新二进制独立哈希 | PASS | `environment/g6_binary.sha256`：`13cf8a...`；G5 `1a527f...`；G4 `1d3aa6...`；G3/G2/G1/G0 记录保留 |
| G6 cumulative source.patch | PASS | `environment/source.patch` SHA256 `53b188...`；G5 snapshot `f3d898...`、G4 `9c7331...` 等保留 |
| 固定提交干净副本应用 G6 补丁并构建 | PASS | `logs/g6_clean_worktree.log`、`g6_clean_apply.log`、`g6_clean_build.log` |
| 干净副本 G6 smoke | PASS：16/16 cases、ECC 3/3、PIMKernel 5/5 | `runs/g6/clean_replay/`、`logs/g6_clean_smoke.log` |

### G6 结果摘要

- 实现 whole-channel pause：target channel 停止新的 feeder 提交，已接受/在途事务排空，外部未提交 descriptor 保持冻结；ACK 后目标 channel data access delta 为 0。
- 8000-cycle 观察窗内模拟时钟继续（clock_delta=8000），refresh 计数不倒退并观察到增加；没有冻结模拟器时钟或 REF。
- 4 个暂停时点、K∈{1,8,32}、共 12 个 synthetic control cases 全部通过。
- 实际 CE：W1 F1 报告触发，L=0/8/64 分别在 report+0/8/64 后请求暂停；ACK 后 data delta 0。
- 两 channel smoke 使用独立 host traffic，不是完整 W1/W3 kernel；channel0 停止（8->8），channel1 前进（8->64），明确不声称零性能影响。
- G6 没有实现 G7 的 RESTORING/REEXECUTING 或恢复；暂停为 channel 粒度，不声称 bank 粒度。
- 干净副本二进制 `7f531f...`，16/16 G6、ECC 3/3、PIMKernel 5/5 全部 PASS。

### G6 关键文件

- pause/feeder：`simulator/src/Phase1Pause.{h,cpp}`、`simulator/src/Phase1Feeder.{h,cpp}`
- fixture：`simulator/src/tests/Phase1G6TestCases.cpp`
- runner：`scripts/run_g6.py`
- 文档：`docs/g6_pause.md`
- 结果：`results/g6/summary.csv`、`results/g6/validation.json`
- 干净重放：`runs/g6/clean_replay/`
- G6 lock：`environment/g6.lock.json`

## G5 验收证据

| 检查 | 结果 | 证据 |
|---|---|---|
| 输出版本状态机 | PASS：ALLOCATED/PRODUCING/READY_UNPUBLISHED/PUBLISHED/SUSPECT/POISONED | `simulator/src/Phase1Runtime.{h,cpp}` |
| 未发布 host 读 | PASS：blocked，不产生 host 读数据 | G5 F1/F2 cases；`host_blocked=true` |
| 未发布 PIM consumer | PASS：`canAdmit=false`，不提交消费者 | G5 F1/F2 cases；`pim_blocked=true` |
| producer 内部读写 | PASS：W1 在 output PRODUCING 期间完成输入读取/结果写回 | G5 W1 producer metrics |
| 报告后禁止 publish | PASS：F1 -> SUSPECT、F2 -> POISONED，均 published=false | `results/g5/summary.csv` |
| 旧句柄失效 | PASS：新版本后旧 handle ABORTED/stale、gate 拒绝 | `Phase1Runtime.SameCycleAndStaleHandles` |
| 同周期边界 | PASS：publish 前一 cycle/same cycle 报告锁定；后一 cycle 记为 late | `runs/g5/runtime_tests.log`、`docs/g5_runtime.md` |
| 直接旁路负测 | PASS：`assertConsumerAllowed` 抛出 STALE_HANDLE/UNPUBLISHED_OUTPUT | G5 `bypass_assert=true`、runtime unit test |
| H=0/H=8 | PASS：F0 均在 H 到期后 PUBLISHED | `results/g5/summary.csv` |
| G5 smoke | PASS 12/12 cases | `results/g5/validation.json`、`logs/g5_run.log` |
| 原始 `PIMKernelFixture.*` | PASS 5/5 | `runs/g5/upstream-tests.xml` |
| 已知 `PIMBenchFixture.add` 性能失败 | 保留原基线，未修改阈值 | `runs/g5/pimbench_add.log`，1.98597 < 2.0 |
| G5 新二进制独立哈希 | PASS | `environment/g5_binary.sha256`：`1a527f...`；G4 copy `1d3aa6...`；G3/G2/G1/G0 records preserved |
| G5 cumulative source.patch | PASS | `environment/source.patch` SHA256 `f3d898...`；G4 snapshot `9c7331...`、G3 `933b32...`、G2/G1 preserved |
| 固定提交干净副本应用 G5 补丁并构建 | PASS | `logs/g5_clean_worktree.log`、`g5_clean_apply.log`、`g5_clean_build.log` |
| 干净副本 G5 smoke | PASS 12/12 + runtime test + ECC 3/3 + PIMKernel 5/5 | `runs/g5/clean_replay/`、`logs/g5_clean_smoke.log` |

### G5 结果摘要

- W1 输出版本在 `PRODUCING -> READY_UNPUBLISHED -> PUBLISHED` 上受 host/PIM consumer gate 保护。
- F0：H=0/H=8 均最终 PUBLISHED；HOST 输出与 A+X 独立参考逐 bit 一致；PIM consumer `canAdmit=true`（本关不实现 W2 实际 kernel）。
- F1：报告在 publish 前到达，状态 SUSPECT，host/PIM 均 blocked，输出文件为空；CE=1。
- F2：状态 POISONED，host/PIM 均 blocked；UE=1。
- F1/F2 的 `bypass_assert=true`，直接 consumer bypass guard 抛出异常；旧版本 handle 返回 ABORTED/stale。
- 同周期单元测试覆盖 report 在 publish 前一 cycle、同 cycle、后一 cycle 三种边界；后一 cycle 记录 late report，不撤销已发布状态。
- G5 不实现 G6 的 QUIESCE_REQUEST/ACK、在途排空、暂停或 G7 恢复；SUSPECT/POISONED 在本关为终态。
- 干净副本二进制 `5ca0c6...`，12/12 G5 case、runtime test、ECC 3/3、PIMKernel 5/5 全部 PASS。

### G5 关键文件

- runtime：`simulator/src/Phase1Runtime.{h,cpp}`
- fixture：`simulator/src/tests/Phase1G5TestCases.cpp`
- runner：`scripts/run_g5.py`
- 文档：`docs/g5_runtime.md`
- 汇总：`results/g5/summary.csv`、`results/g5/validation.json`
- 干净重放：`runs/g5/clean_replay/`
- G5 lock：`environment/g5.lock.json`

## G4 验收证据

| 检查 | 结果 | 证据 |
|---|---|---|
| Host/PIM 全路径 ECC hooks | PASS：HOST/PIM_OPERAND/PIM_RESULT/HAB 显式传入 Bank | `Bank.{h,cpp}`、`Rank.cpp`、`PIMRank.cpp`、`BusPacket.h`、`docs/g4_ecc_reports.md` |
| 4 profiles | PASS：NATIVE、LOCAL_VISIBLE、LOCAL_HIDDEN、HOST_ONLY | P1-A summary；profile 行为对照 |
| P1-A smoke | PASS 144/144 | `results/g4/p1a_summary.csv` |
| P1-A 报告中 bank 位置 | PASS：target bank 0/3/15，未用广播 packet bank | `runs/g4/p1a/*/reports.csv`、P1-A summary |
| LOCAL_VISIBLE F1 四路径 | PASS：输出纠正、CE 送达 | P1-A local visible 36/36 |
| LOCAL_HIDDEN F1 | PASS：内部 CE=12，public CE=0，输出纠正 | P1-A local hidden 36/36 |
| NATIVE F1/F2 | PASS：无 ECC/报告，原始输出 | P1-A native 36/36 |
| HOST_ONLY PIM 写回格式缺口 | PASS：PWRITE_HREAD 产生 `UNPROTECTED_FORMAT`，不计 CE/UE | 9 个 format-invalid cases；`format_events=4` |
| P1-B smoke | PASS 63/63 | `results/g4/p1b_summary.csv` |
| P1-B 单事件通知延迟 | PASS：D=0/1/16/256/4096 -> latency 1/1/16/256/4096 | `p1b_summary.csv` |
| P1-B bank/page 粒度 | PASS：报告 region_type/region_id 与实际映射一致 | P1-B reports；page 可由多 bank 共享 |
| P1-B 32 decode burst | PASS：32/32 真实 CE，无 drop，服务间隔 >=I=8 | `P1B_BURST_*`；`report_latencies` |
| truth.csv INJECTION 行 | PASS：target bank/codeword/bit positions/raw before/after | `runs/g4/p1a/*/truth.csv` |
| G4 独立 runner | PASS 6/6 checks，207 cases | `results/g4/validation.json`、`logs/g4_run.log` |
| 原始 `PIMKernelFixture.*` | PASS 5/5 | `runs/g4/upstream-tests.xml` |
| 已知 `PIMBenchFixture.add` 性能失败 | 保留原基线，未修改阈值 | `runs/g4/pimbench_add.log`，1.98597 < 2.0 |
| G4 新二进制独立哈希 | PASS | `environment/g4_binary.sha256`：`1d3aa6...`；G3 copy `6b31ba...`；G2 `f54c8a...`；G1 `be9f6a...`；G0 `f596d0...` |
| G4 cumulative source.patch | PASS | `environment/source.patch` SHA256 `9c7331...`；G3 snapshot `933b32...`、G2/G1 snapshots preserved |
| 固定提交干净副本应用 G4 补丁并构建 | PASS | `logs/g4_clean_worktree.log`、`g4_clean_apply.log`、`g4_clean_build.log` |
| 干净副本 G4 smoke | PASS：7/7 selected cases、ECC 3/3、PIMKernel 5/5 | `runs/g4/clean_replay/`、`logs/g4_clean_smoke.log` |

### G4 结果摘要

- `Bank` 现在使用 `Phase1Storage` 保存 protected codeword；每个 32 B burst 四个 codeword，parity 与 `(channel,rank,actual_bank,row,burst_column,codeword_index)` 绑定。
- `Phase1Reports` 使用 FIFO、Q=64、I 和 D；`reports.csv` 严格只有 public 五字段。`detect_cycle` 仅保留在测试 metrics，不进入 public CSV。
- P1-A 144 个 case 全通过：LOCAL_VISIBLE/LOCAL_HIDDEN 的 F1 输出正确；LOCAL_HIDDEN 的 12 个 CE 不送达 public；NATIVE 无报告；HOST_ONLY PIM 读路径不纠正、PIM 写回格式无效。
- P1-B 63 个 case 全通过：单事件延迟严格等于 `max(1,D)`；bank/page region 与目标真实映射一致；32-read burst 产生 32 条真实 CE，无 drop，服务间隔不小于 I=8。
- P1-A 的 HOST_ONLY PWRITE_HREAD 在 F0/F1/F2、bank 0/3/15 共 9 个 case 产生 `format_invalid`，不冒充 CE/UE。
- G4 不实现输出状态门禁、hold、暂停、恢复或消费者权限；这些属于 G5/G6。
- 干净副本二进制 `0b63dd...`，7 个选定 G4 case、ECC 3/3、PIMKernel 5/5 均 PASS。

### G4 关键文件

- 全路径/报告：`simulator/src/Bank.{h,cpp}`、`Rank.cpp`、`PIMRank.cpp`、`BusPacket.h`、`Phase1Profile.h`、`Phase1Reports.{h,cpp}`、`Phase1G4TestCases.cpp`
- runner：`scripts/run_g4.py`
- 结果：`results/g4/p1a_summary.csv`、`p1b_summary.csv`、`summary.csv`、`validation.json`
- 文档：`docs/g4_ecc_reports.md`
- 干净重放：`runs/g4/clean_replay/`
- G4 lock：`environment/g4.lock.json`

## G3 验收证据

| 检查 | 结果 | 证据 |
|---|---|---|
| SECDED(72,64) 编码/解码模块 | PASS | `simulator/src/Phase1Ecc.{h,cpp}`；`docs/g3_ecc.md` |
| 无故障 encode/decode | PASS：8 个固定 pattern 全部 CLEAN | `runs/g3/ecc_cpp_exhaustive.csv` |
| 单 bit 穷举 | PASS：576/576 CORRECTED，纠正后等于原数据 | 同上；8 patterns × 72 positions |
| 双 bit 穷举 | PASS：20448/20448 UNCORRECTABLE | 同上；8 × C(72,2) |
| 行数核对 | PASS：21032 行 = 8 × (1+72+2556) | `results/g3/validation.json` |
| 独立 Python 参考 | PASS：17 passed，逐行交叉检查 status/corrected/syndrome/overall | `tests/test_ecc_reference.py`、`logs/g3_ecc_python.log` |
| F1 storage witness | PASS：data bit 15，CE/CORRECTED，纠正数据等于原值 | `runs/g3/f1_witness.json` |
| 不自动 scrub | PASS：第二次读取仍 CORRECTED，raw 数据保持翻转 | witness、`Phase1Storage.F1WitnessAndNoScrub` |
| 合法写重新编码 | PASS：写后读取 CLEAN | witness 的 `legal_write_status` |
| Storage key 绑定 | PASS：不同 codeword/key 独立；double-bit UE | `Phase1Storage.DoubleBitAndReencode` |
| G3 独立 runner | PASS 15/15 checks | `results/g3/validation.json`、`logs/g3_run.log` |
| 原始 `PIMKernelFixture.*` | PASS 5/5 | `runs/g3/upstream-tests.xml` |
| 已知 `PIMBenchFixture.add` 性能失败 | 保留原基线，未修改阈值 | `runs/g3/pimbench_add.log`，1.98597 < 2.0 |
| G3 新二进制独立哈希 | PASS | `environment/g3_binary.sha256`：`6b31ba...`；G2 copy `f54c8a...`；G1 hash `be9f6a...`；G0 copy `f596d0...` |
| G3 cumulative source.patch | PASS | `environment/source.patch` SHA256 `933b32...`；G2/G1 snapshots preserved |
| 固定提交干净副本应用 G3 补丁并构建 | PASS | `logs/g3_clean_worktree.log`、`g3_clean_apply.log`、`g3_clean_build.log` |
| 干净副本 G3 smoke | PASS：ECC 3/3、Python 17 passed、F1 witness、PIMKernel 5/5 | `runs/g3/clean_replay/`、`logs/g3_clean_smoke.log` |

### G3 结果摘要

- 码字布局：位置 1..71 中 1/2/4/8/16/32/64 为 Hamming parity，其余 64 个位置按升序放 data bit 0..63；位置 72 是前 71 位的整体偶校验。
- 8-bit parity metadata：bit 0..6 对应 Hamming 位置，bit 7 对应位置 72。该布局是模型假设，不代表真实 HBM codeword 布局。
- 单 bit（including parity position 72）全部 CE 并恢复原数据；所有双 bit 全部 UE。
- F1 witness 使用 codeword `0x0123456789ABCDEF` 的 data bit 15（FP16 lane 0 sign bit）；原始 raw flip 后 parity 不变，读取 CE 并恢复；再次读取仍 CE；合法写后 CLEAN。
- G3 不把 ECC 接入 host/PIM 全路径，不产生 CE/UE 报告，不实现输出门禁/暂停/恢复；这些分别属于后续 G4+。
- 干净副本二进制 `97809a...`，干净 ECC CSV 与主工作区逐字节相同（`clean_csv_matches_main=true`）。

### G3 关键文件

- ECC/Storage：`simulator/src/Phase1Ecc.{h,cpp}`、`simulator/src/Phase1Storage.{h,cpp}`
- C++ 测试：`simulator/src/tests/Phase1EccTestCases.cpp`
- Python 参考：`tests/test_ecc_reference.py`
- runner：`scripts/run_g3.py`
- 证据：`runs/g3/ecc_cpp_exhaustive.csv`、`runs/g3/f1_witness.json`、`results/g3/ecc_summary.json`、`results/g3/validation.json`
- 干净重放：`runs/g3/clean_replay/`
- G3 lock：`environment/g3.lock.json`

## G2 验收证据

| 检查 | 结果 | 证据 |
|---|---|---|
| 配置 schema | PASS：10/10 pytest | `configs/schema.json`、`tests/test_schema.py`、`logs/g2_schema_test.log` |
| events.csv schema | PASS：字段、类型、event_type enum | `scripts/validate_run.py`；每个 run 的 `validation.json` |
| event_seq 严格递增 | PASS：ext_on nt1 1449 条、nt8 10605 条，从 1 连续 | `runs/g2/w1_nt*_seed0_feeder_ext_on/events.csv` |
| cycle 不倒退、同 cycle phase 次序 | PASS | `validation.json` 的 `event_cycle_monotonic` / `event_phase_order` |
| PIM 广播/控制位置不冒充 actual bank | PASS：`actual_bank` 为空并标 `:PIM_BROADCAST` / `:PIM_CONTROL` | `docs/g2_protocol.md`、events.csv |
| reports.csv | PASS：严格 5 字段，G2 仅表头 | `runs/g2/*/reports.csv` |
| truth.csv | PASS：完整未来 header，G2 仅表头 | `runs/g2/*/truth.csv` |
| metadata.json | PASS：run_id、commit、patch/binary/config/env hash、时间、exit、status | `runs/g2/*/metadata.json` |
| metrics.json | PASS：必需字段存在；未定义值 null，不伪造 0 | `runs/g2/*/metrics.json` |
| EXT_OFF vs EXT_ON 周期 | PASS：nt1 2565=2565；nt8 18030=18030 | `results/g2/comparison.csv` |
| EXT_OFF vs EXT_ON 输出 | PASS：逐 bit 相等 | `results/g2/comparison.csv` |
| EXT_OFF/EXT_ON vs G1 feeder | PASS：周期和输出均与 G1 canonical 一致 | `results/g2/comparison.csv` |
| EXT_OFF 事件文件 | PASS：只有 header，无伪造事件 | `runs/g2/w1_nt*_feeder_ext_off/events.csv` |
| 负向序号控制 | PASS：复制 run 并重复 event_seq，validator 失败 | `logs/g2_negative_sequence.log` |
| G2 独立 runner | PASS 8/8 checks，sim failures 0 | `results/g2/validation.json`、`logs/g2_run.log` |
| 原始 `PIMKernelFixture.*` | PASS 5/5 | `runs/g2/upstream-tests.xml` |
| 已知 `PIMBenchFixture.add` 性能失败 | 保留原基线，未修改阈值 | `runs/g2/pimbench_add.log`，1.98597 < 2.0 |
| G2 新二进制独立哈希 | PASS | `environment/g2_binary.sha256`：`f54c8a...`；G1 hash `be9f6a...` 记录保留；G0 copy `f596d0...` |
| G2 cumulative source.patch | PASS | `environment/source.patch` SHA256 `2b7b65...`；G1 snapshot `environment/source_g1.patch` `629a36...` |
| 固定提交干净副本应用 G2 补丁并构建 | PASS | `logs/g2_clean_worktree.log`、`g2_clean_apply.log`、`g2_clean_build.log` |
| 干净副本 G2 smoke | PASS：nt1/nt8 ext_off/on、输出/周期、PIMKernel 5/5 | `runs/g2/clean_replay/`、`logs/g2_clean_smoke.log` |

### G2 结果摘要

- `Phase1Trace` 是新增的被动 CSV writer；`ext_off` 只创建 header-only `events.csv`，`ext_on` 记录事件；两者继续使用同一个 feeder 和存储路径。
- nt1 ext_on 有 1449 条事件，nt8 ext_on 有 10605 条；event_seq 从 1 连续，cycle 非递减，phase 在同 cycle 内按 `ENQUEUE<START<FEEDER<MEMORY<COMPLETE<FINALIZE`。
- G2 明确不实现 ECC、故障注入、CE/UE 报告、输出门禁、暂停或恢复。`reports.csv` 和 `truth.csv` 是 header-only，`metrics.json` 的报告/恢复字段为 null。
- PIM 广播事件把 `actual_bank` 留空并标 `:PIM_BROADCAST`；PIM 控制事件标 `:PIM_CONTROL`；没有把 packet bank 字段冒充所有 bank 的实际身份。
- 干净副本 G2 二进制为 `0d8723...`（路径不同，不要求与工作区 `f54c8a...` 相同），干净 smoke 10/10 checks PASS。

### G2 关键文件

- 数据协议说明：`docs/g2_protocol.md`
- schema：`configs/schema.json`
- trace：`simulator/src/Phase1Trace.{h,cpp}`
- feeder 事件挂接：`simulator/src/Phase1Feeder.{h,cpp}`
- runner/validator：`scripts/run_g2.py`、`scripts/validate_run.py`
- schema 负测：`tests/test_schema.py`
- 运行与协议：`runs/g2/`、`results/g2/`
- 干净重放：`runs/g2/clean_replay/`

## G1 验收证据

| 检查 | 结果 | 证据 |
|---|---|---|
| 源码审计（host/PIM 路径、广播 bank、Scheme8、时钟、完成语义、barrier/burst 生命周期） | PASS | `docs/source_audit.md`，逐项标注固定提交文件/函数/行号；未确认项单列 |
| 新 fixture 的通道/rank/容量/几何显式选择 | PASS | `src/tests/Phase1TestCases.cpp`；1 channel、1 rank、16 banks、8 PIM blocks、256 MiB；2-channel 512 MiB 仅地址检查 |
| 地址 tuple -> address -> tuple | PASS 48/48（1ch），96/96（2ch） | `runs/g1/address_mapping/1ch/address_mapping.csv`、`2ch/address_mapping.csv`、`address_mapping.json` |
| 实际解码器对照（上游 `AddrMapping`） | PASS 48/48、96/96 | 同上 CSV 的 `upstream_decoder_ok` 列 |
| 不碰撞检查 | PASS：48/48 与 96/96 地址唯一 | CSV `unique_address`、summary |
| 地址覆盖 | PASS：bank 0/3/15，row 0/1/8192/16383，burst column 0/1/16/31 | summary JSON |
| 有界 feeder：每 channel 最大 8、每 cycle 1、拒绝重试、外部/内部计数分离 | PASS | `src/Phase1Feeder.{h,cpp}`；W1 `max_accepted_outstanding=8`；专项测试 `logs/g1_feeder_rejection.log`（观测到 rejection 后重试成功） |
| W0 host 写后读 | PASS，direct 54 cycles / feeder 55 cycles，输出逐 bit 相同 | `runs/g1/w0_1ch_direct/`、`w0_1ch_feeder/` |
| W1 n_tiles=1、seed 0/1/2 | PASS，direct 1547 / feeder 2565 cycles；12 个输出与 Python 参考逐 bit 一致 | `runs/g1/w1_1ch_nt1_seed*/`、`results/g1/w1_summary.csv` |
| W1 n_tiles=8、seed 0/1/2 | PASS，direct 9990 / feeder 18030 cycles；12 个输出与 Python 参考逐 bit 一致 | `runs/g1/w1_1ch_nt8_seed*/`、`results/g1/w1_summary.csv` |
| 直接提交与 feeder 输出一致 | PASS，所有 W0/W1 配对 | `comparison.json` 的 `direct_feeder_equal`；`results/g1/validation.json` |
| 独立 Python FP16 参考 | PASS | `scripts/g1_validate.py` 生成 `reference.bin`，按 raw little-endian 16-bit 比较；所有 `bit_exact=true` |
| 元素/burst/tile 换算 | PASS：`dim_bursts=n_tiles*16*channels*ranks*8`，`elements=dim_bursts*16`；C++ 断言整除 | `timing.json`、`docs/implementation_log.md` |
| 原始 `PIMKernelFixture.*` 回归 | PASS 5/5 | `runs/g1/upstream-tests.xml`，tests=5 failures=0 |
| 已知 `PIMBenchFixture.add` 性能失败 | 保留原基线，未修改阈值 | `runs/g1/pimbench_add.log`，speed-up 1.98597 < 2.0 |
| G1 新二进制独立哈希 | PASS | `environment/g1_binary.sha256`：`be9f6a...`；G0 二进制副本 `runs/baseline/sim-g0` 仍为 `f596d0...` |
| `environment/source.patch` 包含新增源码 | PASS，9 个文件（2 个上游最小修改 + 7 个新增） | `environment/source.patch`，SHA256 `629a36...` |
| 固定提交干净副本应用补丁并构建 | PASS | `logs/g1_clean_worktree.log`、`g1_clean_apply.log`、`g1_clean_build.log` |
| 干净副本 G1 smoke | PASS：地址 1/2ch、W1 direct/feeder、原始 PIMKernel 5/5、feeder rejection 测试 | `runs/g1/clean_replay/`、`logs/g1_clean_smoke.log`、`g1_clean_validation.log` |
| G1 独立验收 | PASS 10/10 | `results/g1/validation.json`、`logs/g1_run.log` |

### G1 结果摘要

- W1 的 `dim_bursts` 与 `elements`：n_tiles=1 为 128 bursts / 2048 FP16；n_tiles=8 为 1024 bursts / 16384 FP16。
- 1-channel 地址位宽为 28 bits（256 MiB），2-channel 为 29 bits（512 MiB）。
- feeder 的周期增加来自保守 barrier：当前实现在每个 barrier 前等待前一阶段实际静止，再放行下一阶段。该实现是 Phase1 测试基础设施的模型选择，不是硬件 barrier 测量；周期差异已逐 case 保存。
- W1 的 12 个 case 中 `controller_rejections=0`（outstanding limit 8 小于上游 64 深事务队列）；拒绝/重试路径由专项测试 `Phase1Contract.FeederRetainsOnControllerRejection` 强制触发并验证，未伪造 W1 数据。
- G1 不实现 ECC、故障注入、报告、输出门禁、暂停或恢复；不进入 G2。

### G1 关键文件

- 源码审计：`docs/source_audit.md`
- fixture/feeder/driver：`simulator/src/tests/Phase1TestCases.cpp`、`simulator/src/Phase1Address.*`、`simulator/src/Phase1Feeder.*`、`simulator/src/Phase1Driver.*`
- 配置：`configs/phase1_system_1ch.ini`、`configs/phase1_system_2ch.ini`
- 地址结果：`runs/g1/address_mapping/`
- W0/W1 结果：`runs/g1/w0_1ch_*`、`runs/g1/w1_1ch_*`
- 汇总：`results/g1/summary.csv`、`w0_summary.csv`、`w1_summary.csv`、`address_mapping_summary.csv`、`validation.json`
- G1 锁文件：`environment/g1.lock.json`
- 补丁：`environment/source.patch`
- 干净重放：`runs/g1/clean_replay/`

## G0 验收证据

| 检查 | 结果 | 证据 |
|---|---|---|
| `HEAD` 等于固定提交 `3703d1f19c8f027360cc33a3243eb271e3bb6898` | PASS | `logs/g0_clone.log`、`environment/upstream.lock.json` |
| 校验时 `refs/heads/dev` 指向该提交 | PASS | `logs/g0_ref_check.log`（exit 0） |
| 上游 worktree 未被修改 | PASS | `git status --porcelain` 为空；`environment/upstream-files.sha256` 共 94 个 tracked 文件 |
| 干净构建 `scons NO_STORAGE=0 NO_LIBRARY=1` | PASS | `logs/build_20261115T122721Z.log`，exit 0 |
| 数据存储确实启用 | PASS | 构建命令 `NO_STORAGE=0`；编译行无 `-DNO_STORAGE`；Sconstruct 仅对非零值追加该宏 |
| 规定的上游 `PIMKernelFixture.*` | PASS，5/5 | `runs/baseline/upstream-tests.xml`，tests=5 failures=0 errors=0，exit 0 |
| 额外全量上游测试 | 10/11，1 个性能阈值失败 | `runs/baseline/upstream-all-tests.xml`，tests=11 failures=1；失败项 `PIMBenchFixture.add` |
| 二进制哈希 | PASS | `environment/binary.sha256`，`f596d0a0a8fe5ae0a7859f599aada5c924c8495a876bd7ce5a8a8674b4b12a5e` |
| Python 依赖锁 | PASS | `environment/requirements.lock.txt`，`pip check` 无损坏依赖 |
| 环境锁定 | PASS | `environment/environment.txt`、`environment/environment.json`，`environment_hash=9bd07d55353ada1ef0f6a2f96a41e816caaf806fc048f19f0477e557829af90e` |
| 固定上游配置哈希 | PASS | `upstream.lock.json` 中 `ini/HBM2_samsung_2M_16B_x64.ini` 与三个 `system_hbm*.ini` 的 SHA256 |
| `bootstrap.sh --check-only` | PASS | `logs/bootstrap_20261115T122802Z.log`，exit 0 |
| 独立 G0 验收脚本（G1 开始前） | PASS，18/18 | `scripts/validate_g0.py`；结果 `environment/g0_validation.json`，日志 `logs/g0_validation.log` |
| G1 后 G0 只读产物再校验 | PASS，11/11 | `scripts/validate_g0_artifacts.py`；结果 `environment/g0_artifact_validation.json`，日志 `logs/g1_g0_artifact_validation.log` |

## 非门禁观察：上游性能测试失败

`PIMBenchFixture.add` 执行：

```text
non_pim_cycle_ = 6651
pim_cycle_     = 3349
speed-up       = 6651 / 3349 = 1.985965967...
assert         = speed-up > 2.0  -> false
```

- 结果确定、可重复；单独运行相同失败（`logs/g0_pimbench_add_isolated.log`）。
- 在 `/tmp/opencode/pim-nostorage-diag` 的临时副本中用 `NO_STORAGE=1` 构建后，同一测试仍为 6651/3349 和 1.985965967，说明失败与数据存储开关无关（`logs/g0_diag_nostorage1.log`）。
- 该测试是性能加速比阈值，不是数值正确性测试；未修改测试、阈值或预期结果。
- G0 手册 §11.1 规定的验收命令只运行 `PIMKernelFixture.*`，该规定测试全部通过。若后续以“全部上游测试零失败”为门禁，此项应作为上游基线问题记录，不能通过放宽阈值消除。

## 实际改动

上游 `simulator/` 源码、配置和测试均未修改；没有 build-only compatibility patch，也没有 `source.patch`（无源码变更）。新增文件仅位于 `pim-phase1/` 的脚本、环境记录、日志和文档中。

## 交付位置

- 固定提交与配置/二进制/测试哈希：`environment/upstream.lock.json`
- 构建参数与存储语义：`environment/build_flags.txt`
- 完整环境：`environment/environment.txt`、`environment/environment.json`
- 上游 tracked 文件哈希清单：`environment/upstream-files.sha256`
- 规定测试 XML：`runs/baseline/upstream-tests.xml`
- 全量测试 XML（含失败）：`runs/baseline/upstream-all-tests.xml`
- 首次人工运行副本：`runs/baseline/manual_first_run/`
- 实际命令/退出码/日志：`logs/`

## 下一关唯一任务

**无。** G0–G16 的报告、证据、图和补丁已收尾。

唯一未执行项是 G16 的 clean worktree build/replay：用户明确要求“无需重跑代码”，因此没有重新构建或执行 smoke。该项在 STATUS 和 `docs/g16_supplement_results.md` 中明确标为未执行；没有用旧结果冒充。若后续需要完整 clean replay，可用 `environment/source_g16.patch` 在固定提交干净副本中执行并补记证据。

已有 paper 数字、冻结数据、图和 artifact 均保留。LaTeX 工具链在本环境不可用，`.tex` 已提供但未本地编译；Markdown 报告为最终可读版本。

# G16 补充实验结果与假设判定

- 日期：2026-11-16（UTC）
- 对象：P1-G（escape efficacy）与 P1-H（report-sequence ambiguity）
- 数据源：`results/supplement/`、`configs/generated/supplement-manifest.json`
- 报告：`report/phase1_results.md`、`report/phase1_results.tex`

## 1. 结论摘要

补充矩阵共 4650 cases：P1-G 4320、P1-H 330；全部 SUCCESS，0 FAIL、0 TIMEOUT、
0 MISSING、0 expected negative。P1-G 的总分布为 PREVENTED 2166、
CORRUPT_CONSUMED 1920、MASKED 234。P1-H 的 330 个 case 全部 contract-pass。

## 2. 预注册假设逐条判定

| 假设 | 判定 | 数据与说明 |
|---|---|---|
| H-G1：F3M 的 PREVENTED 格局与 Fig.3 同格 TIMELY 一致；迟到格子变为 CORRUPT_CONSUMED | **SUPPORTED（定性）** | 36 个 F3M 格子中 28 个与 Fig.3 完全相等；8 个差 ≤5.7 个百分点（n_tiles=8 HOST 为 3/9）。差异来自 P1-G 排除 MASKED 且额外要求 held B 损坏，不是参数调整。迟到格子为 CORRUPT_CONSUMED。 |
| H-G2：F4S 在所有 D/H 下都是 CORRUPT_CONSUMED 或 MASKED，PREVENTED=0 | **SUPPORTED** | F4S 1080 cases：CORRUPT_CONSUMED 1008、MASKED 72、PREVENTED 0；无报告、无 UE。 |
| H-G3：F1W_F4S 的 SAME_BANK 与 DIFF_BANK 结果相同；DIFF 的 report_covers_corrupt_bank 全 false | **NOT SUPPORTED as stated（部分支持）** | 990/1080 对相同；90/1080 不同，全部是一侧 MASKED、另一侧传播，原因是两种 placement 选择的 corrupt_burst 不同、FP16 舍入不同；不存在“两侧都传播但一个 PREVENTED、一个 CONSUMED”的 case。DIFF 的 `report_covers_corrupt_bank=false` 为 1080/1080，SAME 为 1080/1080 true。当前 runtime 锁整个 kernel，因此两种 placement 的控制结果在传播 case 上一致；区域覆盖部分支持。 |
| H-H1：scrub=1、bank 粒度下 S2、S3、S4、S6 两两不可区分 | **SUPPORTED** | 10/10 seeds 全部相同，形成 {S2,S3,S4,S6} 等价组；S1 与 S5 可区分。 |
| H-H2：scrub=1、codeword 粒度下 S4 与 S6 仍不可区分 | **SUPPORTED** | S4≡S6 为 10/10；S2、S3 与 S4 为 0/10，可区分。 |
| H-H3：scrub=0 时 S4 出现 UE，因而可区分，但错误已不可纠正 | **SUPPORTED** | S4 在 bank/page/codeword 三种粒度均 10/10 seeds 出现 UE；其他场景 0/10。 |
| H-H4：CoF 式规则在 S2/S3/S4（scrub=1）上同样触发，不能把危险情形与良性情形分开 | **SUPPORTED** | bank：S2/S3/S4/S6 均 10/10 触发；S1/S5 为 0。codeword：S4/S6 10/10，S2/S3/S5 为 0；仍不能单独识别 S4。 |

## 3. P1-G 补充说明

- F3M 与 F4S 交给消费者的坏数据完全相同，都是原值 ⊕ 位 {0,1,4,10}；区别仅在
  是否产生 CE。F4S 无报告，因此 D/H 扫描不能触发控制。
- `MASKED` 是 FP16 舍入导致最终 B/C 字节与无故障结果相同，不是防止成功；它不进入
  PREVENTED/CORRUPT_CONSUMED 的分母，但在 CSV 和图中单独列出。
- 评估器只在运行结束后读 held B 的 raw storage；runtime 不读 truth、golden 或
  corrupt_burst。
- n_tiles=8 HOST 的 270-case 配置：
  - F3M：P 172、C 80、M 18；
  - F4S：P 0、C 252、M 18；
  - F1W SAME：P 180、C 81、M 9；
  - F1W DIFF：P 180、C 81、M 9。

## 4. P1-H 补充说明

- 只使用公开报告字段；signature 为 `error_class:region_id`，不含 `report_cycle`。
- 非单例等价组：
  - scrub=0：bank {S2,S3}；page {S2,S5}；codeword 无。
  - scrub=1：bank {S2,S3,S4,S6}；page {S2,S4,S5,S6}；codeword {S4,S6}。
- CoF 规则（scrub=1，10/10）：
  - bank：S2/S3/S4/S6；
  - page：S2/S4/S5/S6；
  - codeword：S4/S6。
- S6 在 scrub=0 不运行；这不是 0%，图中使用斜线 N/A。
- codeword 粒度是 ASSUMED 接口，不是真实 HBM 报告格式；scrub 是理想化零成本模型。

## 5. 偏离预注册

1. **codeword region_id 公式**：G10 协议只写“确定性线性编码”；G13 实现前在
   `docs/g10_supplement_protocol.md` 中固定了公式
   `(((channel*num_banks+bank)*num_rows+row)*burst_columns+col)*4+cw`。该补充发生在
   G14 冻结之前，未改变假设或结果；协议 SHA256 从 `8758ead...` 变为 `86d2336e...`。
2. **F3M 对照分母**：预注册写 `<k>/27`，但实际 F3M 矩阵是 2 n_tiles × 2 consumer
   × 3 D × 3 H = 36 个格子。报告使用真实 `8/36`，并给出 n_tiles=8 HOST 的 `3/9`；
   没有为了凑 27 删除格子。
3. **G15 图注重排**：用户要求提高可读性，Fig.7/Fig.8/Fig.S2 重新排版，并在 G15
   追加 `figures/captions.md` 条目；数据与统计未变，G16 仅审阅。
4. **run_suite metadata**：沿用 runner 的 `gate="G8"` 字段；本补充以 supplement
   manifest、`results/supplement/validation.json` 和独立复核为准。
5. **clean replay**：按用户“无需重跑代码”的指示，G16 未重新构建/运行干净副本；
   已生成 `environment/source_g16.patch` 并做差异/内容检查，但没有声称 clean replay
   已执行。G10/G13 的最终回归比较仍是只读比较，结果为 `REGRESSION=PASS`。

## 6. 失败 case

无。`results/supplement/failure_cases.csv` 只有表头；4650 cases 全部
SUCCESS/contract-pass。没有删除、隐藏或改写失败样本。

## 7. 回归与冻结检查

- G10 240-case 回归：`REGRESSION=PASS`，0 差异。
- G13 240-case 回归：`REGRESSION=PASS`，0 差异。
- G10 冻结文件：33,868/33,868 SHA256 一致。
- paper 的 `runs/paper/`、`results/g8/`、`figures/fig01..06`、`artifact/` 未被修改。

## 8. 复现

```bash
.venv/bin/python scripts/expand_supplement.py --suite configs/suite_supplement.yaml
.venv/bin/python scripts/run_suite.py \
  --manifest configs/generated/supplement-manifest.json \
  --results-dir results/supplement --runs-dir runs/supplement --jobs 8 --resume
.venv/bin/python scripts/aggregate_supplement.py
.venv/bin/python scripts/plot_supplement.py
```

## 9. 边界

- 不能写成“防住了 Rowhammer”或“防止了 silent corruption”；只能说在构造的漏检模式
  和给定 D/H 下，部分错误输出在消费前被扣留。
- F3M/F4S 是 CoF 式构造位模式，不是概率模型；P1-H 的不可区分结论不推广到真实
  HBM 报告接口。
- codeword 粒度与 scrub 都是 ASSUMED；runtime 锁定粒度是整个 kernel。
- P1-H 每格 10 seeds；`10/10` 不是无限总体的保证。

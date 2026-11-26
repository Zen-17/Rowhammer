# 交付点报告：DP-M3（对应决策门 G4）

- **Git Commit / Config Hash**: `073b3070fd19258c21800a5b2d9537fb75a8db2d` / `a3543320f155da4a83333d9a0fa708ccec6f4a591e45285b320cede614423c08`
- **执行状态**: COMPLETED
- **单元测试通过率**: `1/1`（`sim --gtest_filter=Phase3M3.AblationSwitches`，日志 `phase3_exp/results/m3/ablation_switches.log`）

本次战役目录是 `runs/M3/e5c`、`e5realc`、`e6c`、`refc`。运行日志 `phase3_exp/results/m3/campaign_e5c.log`。`configs/para_p.json` 仍是 p=0.0625；本战役 PARA 使用 p=0.01171875。`gate_state.json` 的 G4 保持 PENDING。

## 1. 产出文件核对清单
| 文件路径 | 行数 / 样本数 | 完整性校验（无 NaN / 无漏格） |
|---|---|---|
| `phase3_exp/results/m3/E5_scale_runs.csv` | 4800 条成功运行 + 表头 | PASS。O1..O6 之和等于 n_success |
| `phase3_exp/results/m3/E5_campaign_summary.csv` | 36 格 | PASS。全部 SUCCESS，n_failed=0 |
| `phase3_exp/results/m3/E5_real_threshold_runs.csv` | 80 条 + 表头 | PASS。与 1/16 主表分开 |
| `phase3_exp/results/m3/E5_real_summary.csv` | 8 格 | PASS |
| `phase3_exp/results/m3/E6_runs.csv` | 999 条成功 + 1 条 FAILED + 表头 | PASS。FAILED 未填成 0 |
| `phase3_exp/results/m3/E6_ablation.csv` | 10 格 | PASS。`OURS-SCRUB A1` 为 99/100 PARTIAL |
| `phase3_exp/results/m3/E5_hbm3_missing.csv` | 4 格 | PASS。STATUS=MISSING，未进入图 6/图 7 |
| `phase3_exp/results/m3/table2_e5_campaign.csv` | 表头 + 36 + 8 | PASS。1/16 与 14.5K 分块 |
| `phase3_exp/figures/fig6_overhead_vs_failure.pdf` | 2×3 | PASS。真实阈值 N=10 不在图上 |
| `phase3_exp/figures/fig7_e5_throughput.pdf` | 2×3 | PASS。纵轴是相对无攻击 NONE 的吞吐 |
| `phase3_exp/figures/fig_e6_ablation.pdf` | 双面板 | PASS |

## 2. 决策门 G4 量化指标核对
| 判据项 | 要求门槛 | 实测结果（含 95% CI） | 判定 |
|---|---|---|---|
| OURS 主战役 O6 | 六格均为 0，N=300 的 3/N 上界 ≤1%，且周期严格低于 FIX | 六格 O6=0/300。3/N=0.010。Wilson 95% 上界均为 0.0126。周期相对 NONE：W2 A1/A3/A4 为 89.8%/57.3%/91.9%，DEC 为 65.7%/67.6%/59.9%；同格 FIX 为 144.2%/112.3%/167.5% 与 137.1%/113.9%/132.9%。六格 OURS 都低于 FIX | PASS |
| 五项消融都在逃逸、误恢复、翻转或开销上变差 | 每一项至少有一个指标可测变差（\|z\|≥1.96） | −CE 翻转 A1 2.00 对 1.03（z=93.1）、A3 2.00 对 1.30（z=26.4）。−SCRUB 残留翻转 A1 1.03 对 0（z=59.5）、A3 1.30 对 0（z=28.2）。G1.5 周期 A1 z=4.38、A3 z=3.39。−DET 的 A1 翻转 z=4.18、残留翻转 z=2.51。−EPOCH 的周期 z=1.02/0.81，翻转、逃逸和误恢复都没有达到 \|z\|≥1.96 | FAIL |

主战役里预防失效是实测的，不是零填充。1800 次 OURS 都至少翻转 1 次（O1=0）。FIX、ADA、NONE 六格都是 O6=100/100。OURS 在翻转之后把该区域升到 L1：W2 的 A1/A3/A4 有 12/67/7 次继续升到 L2 并重放，DEC 为 10/90/10 次；其余次数由这次换行上的单比特清扫把结果恢复，记为 O2。REACT 六格都是 O3，平均翻转约 1.75–2.00，没有 CE。PARA 六格都是 O1、0 次翻转，预防没有失效。

14.5K 单独列表，不并进上表的 1/16 判定。DEC A1：NONE O6=9/10，FIX O6=10/10，OURS O2=10/10，REACT O3=10/10。DEC A3：NONE O6=9/10，FIX O6=9/10，OURS O2=9/10 且 O3=1/10，REACT O3=9/10。

## 3. 异常、偏差与待人工裁决事项
- 被检查的行是区域边界上的冷行：W2 第 768 行，DEC 第 2432 行。内核结果行仍是 W2 640、DEC 2384。攻击行是受害区域外面的相邻行，所以受害区域在 CE 之前不会换行。A1/A4 每步都激活这一个外侧邻居。A3 每步同样激活它，并在该邻居所在区域内再写一行，16 行循环。上限是 `2.6×firstThreshold+64`，分两波。
- CE 把该区域的快周期立即置为到期，从而放出第一次换行。单比特清扫跟着这次换行走。结果仍不一致时，重放会再跑内核，并把原来的主机图案写回当前物理行。
- 换行字节由 `exchangeRows` 搬走。每一对换行另加 `4×tRC`（tRC=47）个周期。没有再把换行做成一轮模拟读写，因为写路径在入队时拷贝缓冲区，空缓冲区会覆盖两行的数据。
- g、n、c、HC 和 HBM2 时序没有改。HBM3 没有 ini，4 格保持 MISSING。
- E6 有 1 次 FAILED：`DEC OURS-SCRUB A1` index 19，seed 1698260322，`m2 watchdog exceeded`。该次没有写入 O1..O6。
- −EPOCH 把 `logical()` 返回值错开一行，但本单元直接用 `physical()` 读被检查行，所以这项消融没有进入逃逸或误恢复。G4 第 2 条因此 FAIL。
- PARA 的邻行刷新在这组攻击上把扰动清掉了，六格都没有进入后续恢复。

## 4. 停机确认
已停止后续任务，等待指令：`GO` / `FALLBACK` / `REVISE`。G4 未写成 GO。未进入 M4。

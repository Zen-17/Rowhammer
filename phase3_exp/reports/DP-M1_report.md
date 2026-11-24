# 交付点报告：DP-M1（对应决策门 G2）

- **Git Commit / Config Hash**: `bd6b99c0ac428927b01a931e61f900fe9cffaa4b` / `22cff5f4f2aecaffe6fb85ad7099eeddc430650c159465d7a3b46d565a192bce`
- **执行状态**: COMPLETED
- **单元测试通过率**: Phase3M1 model/attack tests 4/4; 1/16 self-check A1/A2/A3 x 5 and fault-free NONE/REACT passed; campaign 198/198

## 1. 产出文件核对清单
| 文件路径 | 行数 / 样本数 | 完整性校验（无 NaN / 无漏格） |
|---|---|---|
| `phase3_exp/results/M1_motivation_check.csv` | 180 data rows; 1/16 grid | PASS |
| `phase3_exp/results/M1_motivation_check_real14k.csv` | 18 data rows; real 14.5K, separate file | PASS |
| `phase3_exp/results/E1_attack_flips.csv` | 8 data rows; A1 x NONE curve; scale column keeps 14.5K and 1/16 apart | PASS |
| `phase3_exp/figures/fig2_e1_characterization.pdf` | 1 file; panels a b c | PASS |
| `phase3_exp/mc/e2_preliminary.csv` | 864 data rows; preview, c and g not locked | PASS |

## 2. 决策门 G2 量化指标核对
| 判据项 | 要求门槛 | 实测结果（含 95% CI） | 判定 |
|---|---|---|---|
| W2 NONE A1 O6 | fraction > 0 | 30/30 = 1.0000 Wilson [0.8865, 1.0000] | PASS |
| W2 REACT vs NO_ATK throughput | drop >= 10%, and state O5/O6 | NO_ATK 0.0980962 -> REACT 0.0271363, drop 72.3%; no O5 or O6; outcomes O3:30 | PASS |
| DEC NONE A1 O6 | fraction > 0 | 30/30 = 1.0000 Wilson [0.8865, 1.0000] | PASS |
| DEC REACT vs NO_ATK throughput | drop >= 10%, and state O5/O6 | NO_ATK 0.0921703 -> REACT 0.0351007, drop 61.9%; no O5 or O6; outcomes O3:30 | PASS |

G2 = GO. Wilson z = 1.959964. A zero cell is reported with 3/N and is not written as a rate below 1%.

## 3. 异常、偏差与待人工裁决事项
- G1 记为 GO：实施指令要求保留 W4 Reuse 报警作为动机结果。阈值、复用地址和防御都没有改。
- 锤击阶段直接驱动 ReadDisturbanceModel，并暂时从刷新观察者上摘下，避免旁路 REF 指针在到达锁定阈值前把 D 清零。阈值本身没有改。
- A2 是 8 对双面锤击。第 0 对是目标行的两个邻居，其余对间隔 4 行，避免激活目标行并把 D 清零。
- 1/16 自检：A1 与 A2 各 5 次都出现多比特翻转且输出与 Golden 不一致；A3 每次 1024 次激活，单行份额在 AttackShapes 中小于 1/16。无故障 NONE 与 REACT 与 Golden 逐比特一致且翻转数为 0，分类为 O1。
- e2_preliminary.csv 每格 4 次试验，不锁定 c 或 g。锁定留在 M2.1。
- configure 接受 HBM3_STYLE，本次运行没有使用它。

## 4. 停机确认
已停止后续任务，等待指令：`GO` / `FALLBACK` / `REVISE`。

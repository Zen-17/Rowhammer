# 交付点报告：DP-M2（对应决策门 G3）

当前门控以 `configs/gate_state.json` 为准，G3 = REVISE。本节以下是原始 M2 记录。刷新修复后的对照、图 3r 和图 3s 见 `reports/DP-M2-R_report.md`。

- **Git Commit / Config Hash**: `bd6b99c0ac428927b01a931e61f900fe9cffaa4b` / `22cff5f4f2aecaffe6fb85ad7099eeddc430650c159465d7a3b46d565a192bce`
- **执行状态**: COMPLETED
- **单元测试通过率**: Phase3M2 4/4（`sim --gtest_filter=Phase3M2.*:-Phase3M2.Cell`）；framework 9/9（`python -m unittest phase3_exp.tests.test_framework`）

## 1. 产出文件核对清单
| 文件路径 | 行数 / 样本数 | 完整性校验（无 NaN / 无漏格） |
|---|---|---|
| `phase3_exp/results/E2_mc_security.csv` | 864 格，每格 1000 次 | PASS |
| `phase3_exp/figures/fig4_e2_mttf_heatmap.pdf` | 1 文件；设计点标在 n=64, c=2 | PASS |
| `phase3_exp/results/E3_prevention_timeline.csv` | 144498 行；A1–A4 × 5 方案 ×（1/16 五次 + 14.5K 三次） | PASS |
| `phase3_exp/figures/fig3_e3_attack_timeline.pdf` | 1 文件；1/16 与 14.5K 分轴 | PASS |
| `phase3_exp/results/E4_overhead_scaling.csv` | 165 条 HBM2 成功运行 + 1 条 HBM3 MISSING | PASS |
| `phase3_exp/figures/fig5_e4_overhead_vs_hc.pdf` | 1 文件；只画 HBM2 | PASS |
| `phase3_exp/configs/design_point.json` | 设计点已锁定 | PASS |

## 2. 决策门 G3 量化指标核对
| 判据项 | 要求门槛 | 实测结果（含 95% CI） | 判定 |
|---|---|---|---|
| E3 FIX 与 ADA 在 A1–A4 上的翻转 | 成功运行的总翻转数为 0 | 64/64 条终态记录翻转数为 0；无 L1→L2 | PASS |
| E4 14.5K 正常负载，ADA 与 OURS 预览相对 NONE 的周期开销 | 各自小于 FIX 的 1/3 | 五负载均值 FIX 2.616%，ADA 0.058%，OURS 0.044%；FIX/3 = 0.872%。W1/W2/W4/GEMV/DEC 每一负载都满足 | PASS |

G3 = GO。设计点 n=64、c=2。FIX/ADA 用 g=1.5（r0=954，r1=75），OURS 预览用 g=1.1（r0=1395，r1=103）。刷新窗口 614400 次激活。

## 3. 异常、偏差与待人工裁决事项
- E2 全网格 864 格的 MTTF_flip 与 MTTF_sdc 都是 614400，全部被刷新窗口截尾。n=16384 的 r1=1，FIX 仍被截尾。图 4 因此是均一底色，圆圈标在 n=64、c=2 的格子内。
- 设计点 A1：FIX 不升档（escalated 0/1000）；ADA g=1.5 升档 989/1000；OURS g=1.1 升档 1000/1000。未升档的 ADA 试验在第一个检测窗之前就被“本轮换位到不了阈值”截尾，翻转数仍为 0。没有改 f_th、E_det、c 或 g。
- 无攻击 W2 上 ADA 的换位次数为 0（r0=954 未被这道内核打满）。PARA 的 p 二分到 0.0625，与 ADA 的周期差 0.43%，低于 0.5%，已写入 `configs/para_p.json`。
- 计算侧只加了两个字段：检测时按当前映射把物理行译回逻辑行，报告队列和 OpEntry 记录 mapping_epoch。B2 的恢复判定没有读这两个字段，公开报告 CSV 的列也没有改。
- 换位是 v1：读出一对行，SECDED 纠正单比特后再写入新物理行，两行 D 清零、物理邻居各 +2。周期开销按这对行的 4 次突发计入，不是 v2 的 FILL/MOV。
- OURS 只是预览：g=1.1 的自适应换位、擦洗和 epoch。没有 CE 反馈，也没有 B2。E3 里没有出现 L1→L2。
- `configs/hbm3_style.ini` 不存在，手册也没有给出 HBM3 的 tRC 和 tREFI。HBM3 半格记为 MISSING，没有编造时序，也没有打开“单比特对主机不可见”。
- 随 HC 从 14531 降到 1000，FIX、ADA、OURS 的换位次数和周期开销单调上升。14.5K 无攻击下 ADA 与 OURS 预览保持 L0。
- 自检：连续 100 次换位后 16 个 bank 的同一逻辑行仍落到同一物理行；无故障 FIX/ADA 的 W2 与 DEC 输出与 NONE 逐字节相同；注入 1 bit 后换位，该码字的单比特翻转被擦掉。

## 4. 停机确认
已停止后续任务，等待指令：`GO` / `FALLBACK` / `REVISE`。未进入 M3。

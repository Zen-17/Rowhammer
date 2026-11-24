# 交付点报告：DP-M0（对应决策门 G1）

- **Git Commit / Config Hash**: `bd6b99c0ac428927b01a931e61f900fe9cffaa4b` / `22cff5f4f2aecaffe6fb85ad7099eeddc430650c159465d7a3b46d565a192bce`
- **执行状态**: COMPLETED
- **单元测试通过率**: Phase3M0.Workloads PASSED (78034 ms); OpenedBanks and RefreshWindow unchanged from the prior self-check（日志 `runs/M0/e1/workloads.log`）

## 1. 产出文件核对清单
| 文件路径 | 行数 / 样本数 | 完整性校验（无 NaN / 无漏格） |
|---|---|---|
| `configs/interface_manifest.json` | interfaces | PASS |
| `results/E1_benign_profile.csv` | 2304 data rows | PASS |
| `results/E1_detection_summary.csv` | 10 workloads | PASS |
| `mc/smoke_test.json` | 1000 trials | PASS |
| `figures/fig2a_e1_benign.pdf` | fig2(a) | PASS |

## 2. 决策门 G1 量化指标核对
| 判据项 | 要求门槛 | 实测结果（含 95% CI） | 判定 |
|---|---|---|
| W1 streaming | observed rate meets the reference; 3/N still not below 1% | Nwin=192, min/bank=12, f_max=0.00683594, f_p99=0.00488281, exact_exceed=0, mg_alarm=0, unique_rows=2762-2762, hottest_act=13, 3/N=0.0156 | NOT_PROVEN |
| W1 reuse | observed rate meets the reference; 3/N still not below 1% | Nwin=256, min/bank=16, f_max=0.0136719, f_p99=0.0117188, exact_exceed=0, mg_alarm=0, unique_rows=602-602, hottest_act=145, 3/N=0.0117 | NOT_PROVEN |
| W2 streaming | observed rate meets the reference; 3/N still not below 1% | Nwin=80, min/bank=5, f_max=0.00976562, f_p99=0.00813477, exact_exceed=0, mg_alarm=0, unique_rows=1216-1280, hottest_act=12, 3/N=0.0375 | NOT_PROVEN |
| W2 reuse | observed rate meets the reference; 3/N still not below 1% | Nwin=256, min/bank=16, f_max=0.0214844, f_p99=0.0196289, exact_exceed=0, mg_alarm=0, unique_rows=578-584, hottest_act=288, 3/N=0.0117 | NOT_PROVEN |
| W4 streaming | observed rate meets the reference; 3/N still not below 1% | Nwin=160, min/bank=10, f_max=0.0195312, f_p99=0.010293, exact_exceed=0, mg_alarm=0, unique_rows=4272-4352, hottest_act=27, 3/N=0.0187 | NOT_PROVEN |
| W4 reuse | f_p99 <= 0.02 and exact exceed < 1% | Nwin=160, min/bank=10, f_max=0.133789, f_p99=0.0175781, exact_exceed=1, mg_alarm=1, unique_rows=512-563, hottest_act=1395, 3/N=0.0187 | FAIL |
| GEMV streaming | observed rate meets the reference; 3/N still not below 1% | Nwin=256, min/bank=16, f_max=0.00683594, f_p99=0.00509766, exact_exceed=0, mg_alarm=0, unique_rows=3392-3440, hottest_act=18, 3/N=0.0117 | NOT_PROVEN |
| GEMV reuse | observed rate meets the reference; 3/N still not below 1% | Nwin=256, min/bank=16, f_max=0.0146484, f_p99=0.0136719, exact_exceed=0, mg_alarm=0, unique_rows=572-573, hottest_act=225, 3/N=0.0117 | NOT_PROVEN |
| DEC streaming | observed rate meets the reference; 3/N still not below 1% | Nwin=264, min/bank=16, f_max=0.0166016, f_p99=0.0131152, exact_exceed=0, mg_alarm=0, unique_rows=1520-1552, hottest_act=39, 3/N=0.0114 | NOT_PROVEN |
| DEC reuse | observed rate meets the reference; 3/N still not below 1% | Nwin=264, min/bank=16, f_max=0.0166016, f_p99=0.0136719, exact_exceed=0, mg_alarm=0, unique_rows=572-577, hottest_act=210, 3/N=0.0114 | NOT_PROVEN |
| 钩子前后周期差 | 0 | plain=32000000 hooked=32000000 delta=0 | PASS |
| 32 ms REF 次数与行覆盖 | 8200±200，且 16384 行都被旁路指针覆盖 | refresh=8205, rows=16384 | PASS |
| MC 可复现且降低 r1 不降低 MTTF_flip | 两次一致；r1=75 的均值不低于 r1=20000 | reproducible=True, mean75=614400.0, mean20000=102907.11 | PASS |

## 3. 异常、偏差与待人工裁决事项
- 门控总判据: FAIL。Wilson z 锁定为 1.959964。PASS 要求 Streaming 与 Reuse 都有完整窗口，且 3/N 上界低于 1%。NOT_PROVEN 表示观察到的超限率为 0，但样本还不能证明真实误报率低于 1%。
- 检测器按 bank 做 Misra-Gries（k=15）。每满 1024 次真实 ACT 结算一次，估计计数 > 64 才报警，然后清零。不满 1024 的尾部记为 partial，不进入报警率、f_p99 和 G1。
- exact_exceed 是精确行计数 > 64 的完整窗口比例。mg_alarm 是 Misra-Gries 估计报警比例。二者分开，因为估计值是下界。
- Streaming 的下一批使用不重叠的数据地址。Reuse 重复同一套计算地址：W1/W2 复用输入，W4 的 fan-out 继续读同一个源，GEMV/DEC 复用权重、只改输入向量。没有为了制造或抹掉热点去多插 ACT，也没有强制关行。
- W4 的 Streaming 把同一张 fan-out 图放进连续的 64 行块里。原 stride-128 布局跨到约 6400 行，第二份不重叠副本放不进 bit 13 为 0 的行区。块内 16 路仍读同一源行。Reuse 仍用原来的 stride-128 地址。
- 1024-ACT 检测周期和 32 ms 刷新窗口是两套计数。REF 旁路指针不改内存控制器。
- 防御参数没有改。本报告不把 G1 写成 GO，也不进入 M1。

## 4. 停机确认
已停止在 E1 / G1。未进入 M1。

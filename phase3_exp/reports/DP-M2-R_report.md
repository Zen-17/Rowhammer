# DP-M2-R

当前 M2 结论以本报告为准。G3 仍是 REVISE，没有进入 M3。设计点 n=64、c=2、g=1.5/1.1 和真实阈值 14531 没有改。

## 1. 文件

- E2：`results/m2r/E2_security.csv`，`results/m2r/E2_faithful.json`，图 4r
- E3 当前主结果：`results/m2r_g3_seeds/E3_multiseed.csv`，图 3r
- A3 诊断：`results/m2r_a3_horizon/A3_horizon.csv`。换位敏感性：`results/m2r_swap_sensitivity/A1_swap_sensitivity.csv`，图 3s
- E3 早期记录：`results/m2r/E3_effectiveness.csv`，`results/m2r_attack_fix/E3_real_ref_on.csv`
- E4：`results/m2r/E4_overhead.csv`，图 5r
- 分报告：`reports/M2R_fault_model.md`，`reports/M2R_E2_security.md`，`reports/M2R_E3_effectiveness.md`，`reports/M2R_E4_overhead.md`，`reports/DP-M2R_G3_final.md`，`reports/DP-M2R_a3_swap_diagnostic.md`
- 原始 M2 的 CSV、图 2–5 和 `DP-M2_report.md` 正文未覆盖。git commit `bd6b99c0ac428927b01a931e61f900fe9cffaa4b`。

## 2. 门控

两条机械条件在现有证据下成立，A3 除外。

E3 在刷新正常工作、真实阈值下，FIX、ADA、OURS 于 A1、A2、A4 的 4 个种子注入都是 0。NONE 在 A2 的 4/4、A1 的 3/4、A4 的 1/4 个种子上发生注入。A3 的锁定定义仍是 1024 次访问。另有一次诊断把同一图案从受害行刷新后再跑满 8192 次 REF：NONE 的受害行峰值是 0.726，四种方案注入都是 0。NONE 没有越限，这组不能当成防御有效。

E4 没有重跑。14.5K 无攻击下 ADA 最高约 0.15%，OURS 最高约 0.10%，FIX 约 2.3%–2.7%，ADA 和 OURS 都低于 FIX 的三分之一。开销是 v1 四突发。HBM3 缺失。OURS 没有 CE 反馈、L2 和 B2。

进入 M3 要等人工把 G3 写成 GO。

## 3. E3 怎么读

图 3r 的 (a) 是出现注入的运行比例，柱上的数字是 NONE 的计数。A1、A2、A4 各 4 个种子。A3* 是上面那次一个刷新周期的诊断，不是 1024 次的锁定定义。(b) 是逻辑受害行的峰值扰动除以该行阈值。A2 的注入发生在其他被锤物理行，所以 (b) 里 NONE 的柱子可以低于 1，而 (a) 仍是 4/4。A3* 的 NONE 柱子是 0.726，没有越过阈值。

受害行约每 `8192 × 3900` 个周期刷新一次。出现两次刷新时，间隔是 31,948,800 周期。刷新请求到 REF 的最长等待是 32–34 周期。NONE 不是每个种子都翻转，是因为攻击在随机阈值的 1.25 倍处截断，一次刷新会把该物理行的扰动清掉。

换位清零只作用在 `exchangeRows` 的那一对物理行。bank 0 随后有 4 个真实突发；bank 1–15 没有命令总线激活，清零来自模型记账。图 3s 把这笔记账关掉后重跑 A1。NONE 注入 9 次的种子上，FIX、ADA、OURS 仍然是 0 次注入，峰值不超过 0.057。未参与交换的行不会被清零。

## 4. 未改的部分

E2 的 864 个受保护格子仍是右截尾，P_flip(T)=0，零失效上界 0.015。NONE 在 16 格里有 12 格翻转。图 4r 画的是扰动上界，不是失效时间。E1 没有重跑。

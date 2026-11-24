# Phase 3 实验可执行拆解手册（低自治执行器交付版）

版本：v1.0 · 基于 `phase3_global_experiment_design.md` (v1, 2026-11-22)  
适用对象：负责代码实现、仿真运行、统计汇总与出图的下级执行模型  
核心原则：**严格按序执行、参数零猜测、门控必停机、结果可复现**

## 0. 执行器行为规范与不可违背红线

本手册将 Phase 3 全部实验拆解为 **5 个里程碑（M0–M4）**、**17 个原子任务**与 **4 个硬性决策门（G1–G4）**。执行器定位为**受控实现与运行引擎**，不拥有实验设计变更权。

### 0.1 六条硬性红线

1. **禁止擅自改参或跳步**：所有参数、公式、方案定义、统计口径以第 1 节《唯一事实源》为准；任何未在文中明确授权的改动均视为违规。
2. **决策门强制停机（STOP-AND-REPORT）**：每完成一个里程碑（M0、M1、M2、M3），必须生成标准《阶段交付报告》并**立即停止**，等待人类统筹者审阅并下达 `GO` / `FALLBACK` / `REVISE` 指令后，方可进入下一阶段。严禁自行触发回退分支或越权进入下一里程碑。
3. **先读接口、严禁破坏 Phase 1/2**：修改 `PIMSimulator` 前必须先执行 `M0.1` 固化既有接口清单；严禁改写 Phase 1/2 已验证的 SECDED、报告队列、B2/AUTO 恢复核心逻辑。若实际源码与本手册假设的函数名不一致，立即上报差异并停机，不得自行魔改。
4. **禁止伪造数据与混报阈值**：仿真未完成或崩溃的运行必须标记 `STATUS=FAILED`，严禁补零、插值或估算替代。**缩放阈值（1/16）**与**真实阈值（14.5K）**的结果必须分目录、分文件、分表格存储，严禁混算。
5. **严禁抹除 `ASSUMED` 标注**：本手册为消除执行歧义，对原设计中标为 `ASSUMED` 的项提供了 `[DEFAULT-LOCKED]` 确定性默认实现；所有此类项必须原样登记进**表 1**，不得伪装为硬件实测事实。
6. **严禁实现范围外内容**：不得在模拟器中实现 RRS / AQUA / Rubix / PRAC、恶意 PIM 租户、AB 模式锤击、RowPress / ColumnDisturb / ScaleDisturb、普通 DRAM 平台、DoS 或非命令计数的精细能耗模型。

### 0.2 统一目录结构与可复现规范

所有代码、配置、原始日志与汇总产出必须严格落在以下目录树下：

| 目录路径 | 存放内容 | 约束 |
|---|---|---|
| `phase3_exp/configs/` | YAML/JSON 锁定配置文件（平台、故障、方案、攻击、负载） | 每次运行前校验 SHA256 |
| `phase3_exp/mc/` | Python Monte Carlo 仿真器与 E2 扫描脚本 | 独立于 PIMSimulator |
| `phase3_exp/sim_hooks/` | PIMSimulator 新增钩子、故障模型、映射层补丁与单元测试 | 与主仓隔离或独立分支 |
| `phase3_exp/runs/<M>/<E>/` | 原始单次运行日志（按 `milestone/exp/grid_id/seed.json` 组织） | 禁止覆盖历史成功日志 |
| `phase3_exp/results/` | 汇总 CSV（每组实验一份主表 + 统计汇总表） | 必须包含 `git_commit` 与 `seed` |
| `phase3_exp/figures/` | `plot_paper.py` 生成的 PDF/PNG 论文图表 | 沿用 Phase 1/2 绘图风格 |
| `phase3_exp/reports/` | 每个交付点的 `DP-Mx_report.md` 阶段交付报告 | 供人工快速裁决 G1–G4 |

**随机种子锁定规则**：
- Monte Carlo 基种子：`MC_BASE_SEED = 20260000`，第 `i` 次独立试验种子为 `MC_BASE_SEED + i`。
- PIMSimulator 注错战役基种子：`SIM_BASE_SEED = 20261122`，每个格子 `(workload, attack, scheme, scale, gen)` 内第 `i` 次运行（`0 <= i < N`）的种子由确定性哈希生成：`seed = (SIM_BASE_SEED + crc32(grid_id) + i) % 2^31`。
- 所有随机抽样（$HC_{first}$ 抽样、VRD 窗口波动、翻转码字与比特位置、PARA 概率刷新）必须使用独立的具名 PRNG 流，确保开启/关闭某功能时不扰动其他随机序列。

---

## 1. 唯一事实源（锁定参数、公式与口径）

标记说明：`[SPEC]` 表示直接来自 `phase3_global_experiment_design.md`；`[DEFAULT-LOCKED]` 表示原文档留白或标为 `ASSUMED`、本手册为避免低等级模型自由发挥而锁定的默认实现（人类可在启动前统一调整）。

### 1.1 平台与读干扰故障模型参数

| 参数 | 锁定取值 / 规则 | 来源标记 |
|---|---|---|
| HBM2 时序与几何 | `ini/HBM2_samsung_2M_16B_x64.ini`；每伪通道 16 bank × 16,384 行；$tRC = 47\text{ ns}$；$tREFI = 3.9\ \mu\text{s}$；刷新窗口 $tREFW = 32\text{ ms}$ | `[SPEC]` |
| AB 模式 | `Rank::updateState`：单次激活打开偶数或奇数组共 8 个 bank | `[SPEC]` |
| ECC 与报告/恢复 | 主机侧 SECDED(72,64)；page 粒度报告队列（沿用 Phase 1）；B2 策略（CE 不重放，UE 重放读者）+ AUTO 窗口（沿用 Phase 2 v2.1） | `[SPEC]` |
| 扰动累积公式 | $D(v) = \mathrm{ACT}(v-1) + \mathrm{ACT}(v+1)$，仅计距离 1 的物理相邻行（边界行 0 和 16,383 仅计单侧邻居） | `[SPEC]` / `ASSUMED` |
| 扰动清零规则 | 行 $v$ 被激活或被 REF 刷新时，$D(v) \leftarrow 0$；每个 32 ms 窗口约 8,200 次 REF，每次 REF 顺序刷新 2 行（按模 16,384 轮询指针推进） | `[SPEC]` / `ASSUMED` |
| 首个翻转阈值 $HC_{first}(v)$ | 每行独立抽样，严格下界 $HC_{first,\min} = 14,531$；v1 默认分布：截断对数正态分布（下界截断于 14,531，中位数约 $1.35\times 14,531$，$\sigma=0.25$）；E7 切换为均匀/指数分布做敏感性 | `[SPEC]` 下界 + `[DEFAULT-LOCKED]` 形状 (`ASSUMED`) |
| 多比特累积倍率 | 第 $k \in \{1, 2, 4, 8, 10\}$ 个翻转对应阈值倍率 $m_k \in \{1.00, 1.19, 1.41, 1.66, 1.76\}$，即 $HC_k(v) = m_k \cdot HC_{first}(v)$；判定条件为 $D(v) \ge 2 \cdot HC_k(v)$（相邻翻转数之间按线性插值取整判定第 3、5、6、7、9 位） | `[SPEC]` + `[DEFAULT-LOCKED]` 中间位插值 |
| 翻转位置与注入 | 仅在本行**已写入的有效码字**中均匀随机抽取码字及比特位，调用 `injectDataBit`（64 位数据）或 `injectParityBit`（8 位校验，按 64:8 概率分配）；同一比特重复命中时不重复翻转（或记录为无新增） | `[SPEC]` + `[DEFAULT-LOCKED]` 注入细节 (`ASSUMED`) |
| VRD 时间波动 | 每个刷新窗口对每行有效阈值乘随机因子 $\alpha_w \sim \mathrm{Uniform}(1.0, 3.5)$（最大/最小比 $\le 3.5$，且严格维持下界不低于 $HC_{first,\min}$）；E7 对上下界区间与分布做敏感性 | `[SPEC]` 比值 + `[DEFAULT-LOCKED]` 区间 (`ASSUMED`) |
| 片内 TRR | 视为不存在或已被完全绕过（不做 TRR 抑制） | `[SPEC]` |
| HBM3 风格外推 | 替换 HBM3 时序 ini（锁定于 `configs/hbm3_style.ini`）；并开启片上 ECC 行为模拟：**单比特错误被片上 ECC 静默吞掉（主机不可见、不触发 CE 报告），仅当同一码字出现 $\ge 2$ 位翻转时，主机侧 SECDED 才可见多比特错误** | `[SPEC]` |

### 1.2 预防机制与计算侧改动（设计点 v1）

- **区域映射**：将每个 bank 的 16,384 行划分为 $16,384 / 64 = 256$ 个区域，每区域 $n = 64$ 行。每区域维护两把 6-bit XOR 密钥（`key_old`, `key_new`）和 1 个搬迁指针 `ptr`（$0 \le \text{ptr} < 64$）。**通道内所有 16 个 bank 共用同一组区域映射状态**，以严格保持 AB 模式行对齐。
- **换位节奏与擦洗**：区域内累计每发生 $r$ 次激活（1 次普通激活或 1 次 AB 激活均计 1 次），触发一次成对换位（推进 `ptr` 搬移一对行）。
  - 换位流程（v1 功能性搬移）：读出源/目标行数据 → SECDED 解码纠正单比特错并重新编码（**擦洗**，清零已纠单比特翻转）→ 写入新物理行 → 两行各自扰动计数清零，同时其物理相邻行的扰动计数因该两行各增加 2 次激活而相应累加（`ACT += 2`）。
- **Misra-Gries 检测器**：每个 bank 独立维护 1 个 Misra-Gries 计数器，容量 $k = 15$ 项，检测周期 $E_{det} = 1,024$ 次激活（每满 1,024 次激活清零并结算），报警阈值 $f_{th} = 1/16$（即周期内单行计数 $> 64$ 触发报警）。
- **三级状态机（按区域维护）**：
  - `L0`（正常档）：使用慢速换位周期 $r_0$。
  - `L0 -> L1`（触发升档）：本区域所属 bank 的 Misra-Gries 报警，**或者**（仅在开启 CE 反馈时）本区域收到读路径上报的 CE 报告。升档后使用快速换位周期 $r_1$。
  - `L1 -> L0`（触发降档）：连续 **2 个完整刷新窗口**（64 ms）内无检测器报警且无 CE 报告。
  - `L1 -> L2`（触发软修复）：同一物理行在经历过一次换位与 ECC 擦洗后，**再次**上报 CE，则将该物理行永久重映射到备用行（模拟 HBM3 `SOFT_REPAIR`，每 bank 预留备用行池默认 16 行 `[DEFAULT-LOCKED]`）。
- **换位周期公式与默认数值**（$c$ 初值取 2，待 E2 标定后锁定）：
  $$T_{safe} = \frac{HC_{first,\min}}{g}, \qquad r_1 = \left\lfloor \frac{T_{safe}}{c \cdot n} \right\rfloor, \qquad r_0 = \left\lfloor \frac{T_{safe}/c - E_{det}}{f_{th} \cdot n} \right\rfloor$$

| 方案分组 | 余量 $g$ | 14.5K 真实阈值下的 $r_1$（L1） | 14.5K 真实阈值下的 $r_0$（L0） | 1/16 缩放阈值下的参数缩放规则 |
|---|---|---|---|---|
| FIX、ADA | 1.5 | 75 | 954 | $HC_{first,\min}$、$r_1$、$r_0$、$E_{det}$ 全部乘以 $1/16$ 并向下取整（最小为 1） |
| OURS | 1.1 | 103 | 1395 | 同上，严格保持等比例缩放 |

- **计算侧两处唯一改动（严禁多改）**：
  1. 读路径检测到错误时，按**读取发生时的映射关系**将物理地址翻译回逻辑地址，计算侧依赖账本继续严格使用逻辑地址。
  2. 报告队列条目与依赖账本均附加 `mapping_epoch` 字段（每次区域完成整轮换位 `epoch += 1`），用于校验翻译映射版本一致性。

### 1.3 方案、攻击与负载矩阵锁定

**6 种主方案与 5 项消融**：

| 名称 | 预防开关 | 余量 $g$ | CE 反馈 | 换位擦洗 | 映射 Epoch 校验 | 检测器 | 恢复开关 |
|---|---|---|---|---|---|---|---|
| `NONE` | 关 | — | 关 | 关 | 关 | 关 | 关 |
| `REACT` | 关 | — | 关 | 关 | 关 | 关 | 开 |
| `PARA` | 概率邻行刷新（$p$ 匹配 ADA 同开销） | — | 关 | 关 | 关 | 关 | 开 |
| `FIX` | 固定 $r_1$ | 1.5 | 关 | 开 | 开 | 关 | 关 |
| `ADA` | 自适应 ($r_0/r_1$) | 1.5 | 关 | 开 | 开 | 开 | 关 |
| `OURS` | 自适应 ($r_0/r_1$) | 1.1 | 开 | 开 | 开 | 开 | 开 |
| `OURS-CE` (`−CE`) | 自适应 | 1.1 | **关** | 开 | 开 | 开 | 开 |
| `OURS-SCRUB` (`−SCRUB`) | 自适应 | 1.1 | 开 | **关** | 开 | 开 | 开 |
| `OURS-EPOCH` (`−EPOCH`) | 自适应 | 1.1 | 开 | 开 | **关（故意用错映射）** | 开 | 开 |
| `OURS-G1.5` (`G1.5`) | 自适应 | **1.5** | 开 | 开 | 开 | 开 | 开 |
| `OURS-DET` (`−DET`) | **固定 $r_0$** | 1.1 | 开 | 开 | 开 | **关** | 开 |

**4 种主机侧攻击（均假设知道当前映射、不知未来密钥）**：
- `A1`（双面锤击）：定位目标 PIM 操作数所在物理行 $v$，持续交替激活 $v-1$ 与 $v+1$。
- `A2`（多面锤击）：选定 8 对物理攻击行轮流双面锤击。
- `A3`（躲避检测）：将攻击流量分散到 $\ge 17$ 个不同物理行上，使每个攻击行在 $E_{det}$ 窗口内占比严格 $< f_{th} (=1/16)$，但仍集中扰动目标区域。
- `A4`（利用换位 / 类 Juggernaut）：追踪当前换位指针与映射，选择特定攻击行，使换位搬移产生的额外激活持续叠加到同一受害行上。
- `NO_ATK`：无攻击，仅保留正常主机背景流量，专用于开销测量。
- 攻击强度统一以**占主机内存带宽的比例**表示（默认锁定基准强度 `50%`，并在 E1/E5 报告该值 `[DEFAULT-LOCKED]`）。

**5 种 PIM 负载（均叠加主机背景读写流量交替执行）**：
- `W1`：ADD；`W2`：ADD → MUL → RELU 链；`W4`：fan-out；`GEMV`：`executeGemv`；`DEC`：解码步组合（默认锁定为 3 次顺序 `executeGemv` 接 1 次残差 `ADD` 再接 1 次 `RELU` `[DEFAULT-LOCKED]`）。
- 主机背景读写流量：默认锁定为按均匀随机行地址生成的流式读写，占主机可用带宽的 `20%`，与 PIM 内核交替调度 `[DEFAULT-LOCKED]`。

### 1.4 结果分类与统计公式锁定

每次闭环仿真运行结束后，必须与同配置无故障 Golden Run 逐比特比对输出，归入且仅归入以下 6 类之一，同时额外记录 `latent_flips`（运行结束时内存中残留但未被读出的翻转数）：

| 编号 | 分类名称 | 精确判定条件（按优先级从高到低互斥判定） |
|---|---|---|
| `O6` | 逃逸（Escape） | 最终 PIM 输出与无故障运行存在任意 1 bit 不一致（含 SECDED 误纠正导致的静默错误） |
| `O5` | 扣留（Detected Unrecoverable） | 输出未放行，系统检测到 UE 且恢复失败而主动扣留输出 |
| `O4` | 整体重跑（Full Rerun） | 触发回退至整段任务重跑，且最终输出逐比特正确 |
| `O3` | 选择性恢复（Selective Recovery） | 至少触发 1 次 UE 并通过 B2 选择性重放成功恢复，最终输出逐比特正确 |
| `O2` | 仅 CE 纠正（CE Only） | 发生至少 1 次翻转，但全部表现为 CE 并由读路径 SECDED 直接纠正，未触发重放，输出逐比特正确 |
| `O1` | 无翻转（No Flip） | 全程未发生任何位翻转，输出逐比特正确 |

**置信区间与零逃逸上界公式（强制使用，禁止替换）**：
- **Wilson 95% 置信区间**（$z = 1.959964$，观测次数 $k$，总样本 $N$，$\hat{p} = k/N$）：
  $$\text{center} = \frac{\hat{p} + \frac{z^2}{2N}}{1 + \frac{z^2}{N}}, \qquad \text{half} = \frac{z\sqrt{\frac{\hat{p}(1-\hat{p})}{N} + \frac{z^2}{4N^2}}}{1 + \frac{z^2}{N}}, \qquad \text{CI}_{95\%} = [\max(0, \text{center}-\text{half}),\ \min(1, \text{center}+\text{half})]$$
- **零逃逸上界（三分法则）**：当 `O6` 计数 $k = 0$ 时，额外报告 95% 置信上界 $p_{\text{upper}} = 3 / N$（$N=100 \Rightarrow 3.0\%$，$N=300 \Rightarrow 1.0\%$）。

---

## 2. 阶段 M0：基建钩子、E1 正常刻画与 Monte Carlo 骨架

**阶段目标**：完成代码库只读巡检、无侵入激活/刷新钩子、5 负载无攻击刻画（E1 前半部分）以及 Monte Carlo 骨架，验证核心前提 **G1**。

### 任务 M0.1：代码库只读巡检与接口冻结

- **输入**：现有 `PIMSimulator` 仓库、`ini/HBM2_samsung_2M_16B_x64.ini`、Phase 1/2 恢复模块、`plot_paper.py`。
- **执行指令**：
  1. 只读扫描仓库，定位并记录以下 6 处精确文件路径与函数签名：行激活入口、`Rank::updateState`（AB 模式）、刷新（REF）执行入口、SECDED 编解码与错误注入入口（`injectDataBit` / `injectParityBit`）、Phase 1/2 报告队列与 B2 恢复入口、`plot_paper.py` 数据输入格式。
  2. 核对 `ini/HBM2_samsung_2M_16B_x64.ini` 中的 bank 数、行数、tRC、tREFI 与第 1.1 节是否完全一致。
  3. 输出 `phase3_exp/configs/interface_manifest.json`。若任意入口缺失或 ini 参数冲突，**立即停机上报**。
- **自检测试**：编译现有基线并跑通 1 次 `W1` 无故障仿真，确认基线退出码为 0。

### 任务 M0.2：激活与刷新观测钩子（Activation & Refresh Hook）

- **输入**：`interface_manifest.json`。
- **执行指令**：
  1. 在内存控制器/Rank 侧挂接只读观测钩子，记录每个 `(channel, bank, row)` 在每个 32 ms 刷新窗口内的：激活次数 `act_count`、累计开行时间 `row_open_cycles`、最近一次被 REF 刷新的时间戳。
  2. 确认 AB 模式单次激活正确归因到被打开的 8 个 bank 对应行。
  3. 确认每次 REF 按模 16,384 推进 2 行刷新指针，并记录清零事件。
- **自检测试**：运行 1 个完整刷新窗口（32 ms）的空载/轻载测试，断言 REF 总次数在 $8200 \pm 200$ 范围内、所有 16,384 行均被刷新至少 1 次，且挂接钩子前后仿真周期数误差为 **0**（严格无性能侵入）。

### 任务 M0.3：5 负载基准化与 E1（无攻击刻画部分）

- **输入**：`M0.2` 钩子、5 种负载（`W1`, `W2`, `W4`, `GEMV`, `DEC`）+ 主机背景流量（`20%` 带宽）。
- **执行指令**：
  1. 为每种负载构建标准运行配置并固化至 `phase3_exp/configs/workloads/`。
  2. 在无攻击（`NO_ATK`）条件下，对 5 种负载分别运行完整仿真，输出 `phase3_exp/results/E1_benign_profile.csv`，必填列：`workload, window_id, bank_id, max_row_act, total_bank_act, f_max, f_p95, f_p99, mean_open_cycles, p99_open_cycles`。
  3. 计算每个负载在所有检测周期（$E_{det}=1024$）内的单行激活占比分布 $f$，统计超过 $f_{th}=1/16$ 的窗口比例。
- **自检测试**：确认 5 个负载均正常结束、无超时、无内存越界，且每种负载均存在非零主机背景访存与 PIM 访存交替记录。

### 任务 M0.4：Monte Carlo 仿真器骨架（并行推进起点）

- **输入**：第 1.1–1.2 节故障模型与预防机制公式。
- **执行指令**：
  1. 在 `phase3_exp/mc/mc_sim.py` 中实现离散事件/窗口级 Monte Carlo 仿真器，支持参数化配置：`n, r0, r1, f_th, E_det, g, c, HC_first_min, vrd_ratio, attack_type (A1–A4), scheme (FIX/ADA/OURS)`。
  2. 实现 $\mathrm{MTTF}_{flip}$（首次 $D \ge 2\cdot HC_{first}$）与 $\mathrm{MTTF}_{sdc}$（首次单码字未读出前累计 $\ge 3$ 位翻转）统计器。
  3. 跑通 1 组冒烟测试（$n=64, g=1.5, c=2, \text{A1}$，1,000 次试验），输出 `phase3_exp/mc/smoke_test.json`。
- **自检测试**：相同 `MC_BASE_SEED` 连续运行两次，输出结果必须逐比特一致；降低 $r_1$ 时 $\mathrm{MTTF}_{flip}$ 必须单调不减。

### 交付点 DP-M0 与决策门 G1（强制停机）

- **必须交付文件**：`interface_manifest.json`、`E1_benign_profile.csv`、`mc/smoke_test.json`、`reports/DP-M0_report.md`。
- **决策门 G1 机械判定**：检查 5 个负载无攻击时的 $f_{\text{p99}}$ 与 $f_{\max}$ 是否满足 $f \ll f_{th} (= 0.0625)$（建议参考线：$f_{\text{p99}} \le 0.02$ 且超限比例 $< 1\%$）。
- **门控动作**：**立即停机并提交 `DP-M0_report.md`**。若 G1 未通过，**禁止擅自改设计**，等待人类决策是否将预防重心转向“写入时换位”。

---

## 3. 阶段 M1：读干扰模型、攻击集、NONE/REACT 闭环与 E1 完整产出

**前置条件**：收到人类对 `G1` 的 `GO` 确认。

### 任务 M1.1：读干扰故障模型实现

- **输入**：`M0.2` 钩子、第 1.1 节故障参数。
- **执行指令**：
  1. 在 `PIMSimulator` 中实现 `ReadDisturbanceModel`：维护每行 $D(v)$、每行抽样的 $HC_{first}(v)$、每窗口 VRD 波动因子 $\alpha_w$，以及多比特阈值表 $HC_k(v)$。
  2. 每次物理行 $u$ 激活时，对 $u-1$ 和 $u+1$ 执行 `D += 1`，并检查是否跨越 $2 \cdot HC_k$；每跨越一个档位，在目标行已写入码字中均匀随机抽取 1 个未翻转比特并调用 `injectDataBit` / `injectParityBit`。
  3. 每次物理行 $v$ 被自身激活或被 REF 刷新时，执行 `D(v) = 0`。
  4. 支持配置项 `threshold_scale`（`1.0` 为真实 14.5K，`0.0625` 为 1/16 缩放）和 `gen_mode`（`HBM2` / `HBM3_STYLE`）。
- **自检测试**：编写独立单元测试验证：(a) 连续激活 $v-1$ 与 $v+1$ 至 $D(v) = 2\cdot HC_1(v)-1$ 时零翻转，达到 $2\cdot HC_1(v)$ 时精确触发 1 次翻转；(b) 中途刷新行 $v$ 后 $D(v)$ 归零；(c) 相同种子两次注入的翻转地址与比特位完全一致。

### 任务 M1.2：主机侧攻击生成器（A1–A4）

- **输入**：`M1.1` 故障模型、第 1.3 节攻击定义。
- **执行指令**：
  1. 实现 4 种主机侧攻击流量生成器 `A1`、`A2`、`A3`、`A4`（M1 阶段 `A4` 先实现接口并在 M2 接入映射指针回调），均支持 `host_bw_share` 参数控制带宽占比。
  2. 确保攻击者仅访问主机侧分配的物理行，不直接读写 PIM 操作数行。
- **自检测试**：在 1/16 缩放阈值下，分别运行 `A1`/`A2`/`A3` 针对 `NONE`（无防护）各 5 次，确认 `A1`/`A2` 能稳定触发多比特翻转，`A3` 单行激活占比严格 $< 1/16$。

### 任务 M1.3：NONE / REACT 闭环、O1–O6 分类器与 E1 完整产出

- **输入**：`M1.1`、`M1.2`、Phase 1/2 恢复模块。
- **执行指令**：
  1. 编写自动化闭环验收脚本 `run_closed_loop.py`：先跑 1 次无故障 Golden Run 保存参考输出，再跑注错运行，执行逐比特输出比对并按第 1.4 节规则输出 `outcome (O1–O6)`、`latent_flips`、`effective_throughput`、`total_cycles`。
  2. 补齐 **E1 攻击部分**：在真实阈值 14.5K（及独立记录的 1/16 缩放阈值）下运行 `A1 × NONE`，记录首个翻转时间与翻转累积曲线，输出 `phase3_exp/results/E1_attack_flips.csv` 并生成 **图 2**（`fig2_e1_characterization.pdf`）。
  3. 在 `W2` 与 `DEC` 上运行 `A1 × {NO_ATK, NONE, REACT}`（各 $N=30$ 次，1/16 缩放阈值，外加真实 14.5K 代表性运行 3 次），输出 `phase3_exp/results/M1_motivation_check.csv`。
- **自检测试**：验证无故障运行时 `NONE` 与 `REACT` 的输出与 Golden Run 逐比特 100% 一致，分类恒为 `O1`。

### 任务 M1.4：Monte Carlo 并行网格扫描（为 E2 预热）

- **执行指令**：与 `M1.1–M1.3` 并行在 CPU 多核上运行 E2 第一批 Monte Carlo 扫描（覆盖 $n \in \{16, 32, 64, 128, 256, 16384\}$、$c \in \{1.5, 2.0, 2.5, 3.0\}$、$g \in \{1.1, 1.3, 1.5\}$、A1–A4），输出中间表 `phase3_exp/mc/e2_preliminary.csv`。

### 交付点 DP-M1 与决策门 G2（强制停机）

- **必须交付文件**：`E1_attack_flips.csv`、`fig2_e1_characterization.pdf`、`M1_motivation_check.csv`、`mc/e2_preliminary.csv`、`reports/DP-M1_report.md`。
- **决策门 G2 机械判定**：
  1. `NONE` 在 `A1` 下是否稳定出现 `O6` 逃逸（逃逸率 $> 0$）；
  2. `REACT` 在持续攻击下的有效吞吐是否明显低于无攻击基准（例如吞吐下降 $\ge 10\%$，并同步报告 `REACT` 是否出现 `O5/O6` 风险）。
- **门控动作**：**立即停机并提交 `DP-M1_report.md`**。若 G2 未通过，等待人类决定是否将论文动机调整为“开销”而非“安全”。

---

## 4. 阶段 M2：E2 设计点锁定、预防机制 v1 实现与 E3/E4 实验

**前置条件**：收到人类对 `G2` 的 `GO` 确认。

### 任务 M2.1：完成 E2 安全分析并申请锁定设计点（唯一一次调参窗口）

- **输入**：`mc/e2_preliminary.csv`。
- **执行指令**：
  1. 补齐 E2 全维度 Monte Carlo 扫描：$n$（含全 bank 退化情形 $n=16,384$）、$r$、$f_{th}$、$E_{det}$、$g$、$c$ × A1–A4，每格足够样本以稳定估计 $\mathrm{MTTF}_{flip}$ 与 $\mathrm{MTTF}_{sdc}$。
  2. 输出 `phase3_exp/results/E2_mc_security.csv` 并绘制 **图 4**（`fig4_e2_mttf_heatmap.pdf`，标出设计点位置）。
  3. 核验初值 $c=2$ 与 $(n=64, g=1.5/1.1)$ 是否满足安全目标；若需微调 $c$ 或设计点，**在报告中列出候选参数并等待人工拍板（原设计仅允许在 E2 后改一次）**。
- **自检测试**：验证全 bank 退化情形（$n=16,384$）的换位开销/安全趋势符合理论预期，且无 `NaN`/发散数值。

### 任务 M2.2：区域映射、换位 v1 与计算侧两处改动实现

- **输入**：M2.1 锁定的设计点参数。
- **执行指令**：
  1. 在内存控制器侧实现：区域 XOR 映射表（$n=64$，两把密钥+指针，16 bank 共享保 AB 对齐）、每 bank Misra-Gries（$k=15, E_{det}=1024, f_{th}=1/16$）、L0/L1/L2 状态机、换位 **v1 功能性搬移**（含 ECC 解码重编码擦洗，且搬移两行各给物理邻居 `+2` 激活）。
  2. 在计算侧实现**且仅实现两处改动**：(a) 错误检测时按当时映射将物理地址翻译回逻辑地址；(b) 报告队列与依赖账本记录 `mapping_epoch`。
  3. 实现 `FIX`、`ADA` 与 `PARA`（先在无攻击 `W2` 上二分搜索确定 `PARA` 的概率 $p$，使其周期开销与 `ADA` 相等，误差 $\le 0.5\%$，固化该 $p$ 至配置）。
  4. 补全攻击 `A4` 与映射指针的联动逻辑。
- **自检测试**：
  - **AB 对齐测试**：连续触发 100 次区域换位后，断言通道内 16 个 bank 同一逻辑行映射后的物理行号完全相同。
  - **数据守恒测试**：无故障下开启 `FIX`/`ADA` 跑 `W2` 与 `DEC`，输出必须与 `NONE` 逐比特 100% 一致。
  - **擦洗测试**：向某行注入 1 bit 翻转后强制触发该行换位，断言换位后该物理行单比特翻转已被清除。

### 任务 M2.3：执行 E3 预防有效性实验

- **输入**：`M2.2` 实现。
- **执行指令**：
  1. 运行 `A1–A4 × {NONE, PARA, FIX, ADA, OURS(预览)} × W2`（1/16 缩放与真实 14.5K 分开记录）。
  2. 记录时间序列：最热物理行激活数、累计翻转数、以及精确的 `L0->L1`、`L1->L0`、`L1->L2` 升降档时间戳。
  3. 输出 `phase3_exp/results/E3_prevention_timeline.csv` 并绘制 **图 3**（`fig3_e3_attack_timeline.pdf`）。
- **自检测试**：断言 `FIX` 与 `ADA` 在 A1–A4 下的总翻转数均为 **0**。

### 任务 M2.4：执行 E4 开销与扩展性实验

- **输入**：`M2.2` 实现、HBM2 与 HBM3 风格 ini 配置。
- **执行指令**：
  1. 正常运行开销：5 负载 × `NO_ATK` × `{NONE, PARA, FIX, ADA, OURS}`，记录相对 `NONE` 的周期开销（%）、换位次数、主机平均/P99 延迟。
  2. 阈值扩展性扫描：将 $HC_{first,\min}$ 从 **14.5K 向下扫描至 1K**（锁定扫描点：`14531, 10000, 8000, 6000, 4000, 2000, 1500, 1000`），分别在 HBM2 与 HBM3 风格配置下运行。
  3. 明确计算并标出 **$r_1 < 10$ 时对应的临界阈值**作为方案适用边界。
  4. 输出 `phase3_exp/results/E4_overhead_scaling.csv` 并绘制 **图 5**（`fig5_e4_overhead_vs_hc.pdf`）。
- **自检测试**：验证随阈值降低，换位次数与周期开销单调上升，且在 14.5K 下 `ADA` 与 `OURS` 均处于 `L0` 档位（无误升档）。

### 交付点 DP-M2 与决策门 G3（强制停机）

- **必须交付文件**：`E2_mc_security.csv`、`fig4_e2_mttf_heatmap.pdf`、`E3_prevention_timeline.csv`、`fig3_e3_attack_timeline.pdf`、`E4_overhead_scaling.csv`、`fig5_e4_overhead_vs_hc.pdf`、`reports/DP-M2_report.md`。
- **决策门 G3 机械判定**：
  1. E3 中 `FIX` 与 `ADA` 在 A1–A4 下是否**零超限（零翻转）**；
  2. E4 中 14.5K 正常负载下是否满足 **ADA / OURS 周期开销 ≪ FIX**（例如 ADA/OURS 开销不到 FIX 的 1/3）。
- **门控动作**：**立即停机并提交 `DP-M2_report.md`**。若 G3 未通过，等待人类决定调整 $n, r$ 或收窄适用阈值范围。

---

## 5. 阶段 M3：OURS 完整闭环、E5 主战役与 E6 协同消融

**前置条件**：收到人类对 `G3` 的 `GO` 确认。

### 任务 M3.1：OURS 完整闭环与 5 项消融开关接入

- **输入**：`M2.2` 实现。
- **执行指令**：
  1. 完整启用 `OURS`：$g=1.1$（14.5K 下 $r_1=103, r_0=1395$）+ 读路径 CE 报告反馈至 MC 触发 `L0->L1` + 同一物理行换位擦洗后再次报 CE 触发 `L1->L2`（`SOFT_REPAIR` 映射至备用行）+ 计算侧 B2 选择性恢复。
  2. 实现 5 项独立命令行消融开关：`--disable-ce-feedback`（`−CE`）、`--disable-swap-scrub`（`−SCRUB`）、`--corrupt-epoch-map`（`−EPOCH`）、`--margin-g 1.5`（`G1.5`）、`--disable-detector`（`−DET`）。
- **自检测试**：分别用 5 个针对性微型测试用例验证每个消融开关确实生效（例如 `−SCRUB` 下翻转随行搬移保留、`−EPOCH` 下物理转逻辑地址故意错配、`−CE` 下 CE 不触发 `L0->L1`）。

### 任务 M3.2：执行 E5 闭环主战役（约 4,800 次并行运行）

- **输入**：`M3.1` 实现、服务器并行调度脚本。
- **执行指令**：
  1. **主矩阵（36 格，1/16 缩放阈值，HBM2）**：`{W2, DEC} × {A1, A3, A4} × {NONE, REACT, PARA, FIX, ADA, OURS}`。
     - 样本量锁定：头号 6 格（`OURS` 全部 6 格）取 **$N = 300$**；其余 30 格取 **$N = 100$**（合计 $6\times 300 + 30\times 100 = 4,800$ 次）。
  2. **HBM3 外推子集（4 格，1/16 缩放阈值，HBM3 风格）**：锁定为 `{DEC} × {A1, A3} × {ADA, OURS}`（`OURS` $N=300$，`ADA` $N=100$ `[DEFAULT-LOCKED]`）。
  3. **真实阈值代表性运行（独立分表）**：在 14.5K 真实阈值下，对 `{DEC} × {A1, A3} × {NONE, REACT, FIX, OURS}` 各跑 $N=10$ 次代表性长时运行，单独写入 `E5_real_threshold_runs.csv`，**严禁混入缩放战役统计**。
  4. **算力不足降级预案（仅在收到人类批准后启用）**：攻击缩减为 `{A1, A3}`，普通格降为 $N=50$，**头号格（OURS）必须保持 $N=300$**。
  5. 按第 1.4 节公式汇总每个格子的 `O1–O6` 比例、Wilson 95% 置信区间、零逃逸三分法则上界（$3/N$）、平均潜伏翻转数与持续攻击下的有效吞吐。
  6. 输出 `phase3_exp/results/E5_campaign_summary.csv`、`E5_real_threshold_runs.csv`，生成 **主图 6**（`fig6_overhead_vs_failure.pdf`，联合 E2 的 MTTF 与 E5 的逃逸/开销）、**图 7**（`fig7_e5_throughput.pdf`）与 **表 2**（`table2_e5_campaign.csv/.tex`）。
- **自检测试**：断言总成功运行数等于预期样本数（无静默失败丢弃）；每个格子 `O1+O2+O3+O4+O5+O6 == N`。

### 任务 M3.3：执行 E6 协同消融实验

- **输入**：`M3.1` 消融开关。
- **执行指令**：
  1. 在 `{DEC} × {A1, A3}` 上运行全部 5 项消融（`−CE`, `−SCRUB`, `−EPOCH`, `G1.5`, `−DET`），每格 $N=100$（1/16 缩放阈值），并与 E5 中对应的完整 `OURS` 基线直接对比。
  2. 统计每项消融的 `O6` 逃逸率、误恢复率（`O4+O5` 或错误重放）、总翻转数与周期开销。
  3. 输出 `phase3_exp/results/E6_ablation.csv`（作为 **表 3** 上半部分数据源）。
- **自检测试**：核对 `G1.5` 开销应高于完整 `OURS`，`−EPOCH` 应出现明显误恢复或逃逸，`−SCRUB` 应出现更高翻转累积或逃逸。

### 交付点 DP-M3 与决策门 G4（强制停机）

- **必须交付文件**：`E5_campaign_summary.csv`、`E5_real_threshold_runs.csv`、`fig6_overhead_vs_failure.pdf`、`fig7_e5_throughput.pdf`、`table2_e5_campaign.csv`、`E6_ablation.csv`、`reports/DP-M3_report.md`。
- **决策门 G4 机械判定**：
  1. `OURS` 在全部主战役格子中 **O6 逃逸数为 0**（$N=300$ 下三分法则上界 $\le 1\%$），且周期开销严格低于 `FIX`；
  2. E6 中 5 项消融**每一项都在至少一个核心指标（逃逸/误恢复/翻转数/开销）上可测地变差**。
- **门控动作**：**立即停机并提交 `DP-M3_report.md`**。若 G4 未通过，等待人类决定是否将主张 C4 降级表述为“互补”。

---

## 6. 阶段 M4：E7 敏感性与硬件代价、换位 v2 时序与最终图表交付

**前置条件**：收到人类对 `G4` 的 `GO` 确认。

### 任务 M4.1：执行 E7 参数与 ASSUMED 模型敏感性分析

- **执行指令**：以 `OURS` 在 `{DEC} × {A1, A3}` 为基准，采用**单因素控制变量扫描（OFAT）**，每点 $N=100$（1/16 缩放阈值）：
  1. 区域大小 $n \in \{16, 32, 64, 128, 256\}$；
  2. Misra-Gries 项数 $k \in \{7, 15, 31\}$；
  3. 报告队列延迟（`1x, 2x, 4x` Phase 1 默认值）与报告粒度（`page` vs `row`）；
  4. 代际对比：`HBM2` vs `HBM3_STYLE`；
  5. **ASSUMED 故障模型敏感性（强制）**：$HC_{first}$ 分布形状（截断对数正态 / 均匀 / 移位指数）与 VRD 波动幅度（`1.0x [无波动], 2.0x, 3.5x, 5.0x`）。
  6. 输出 `phase3_exp/results/E7_sensitivity.csv` 并绘制 **图 8**（`fig8_e7_sensitivity.pdf`）。

### 任务 M4.2：硬件 SRAM 代价精确核算

- **执行指令**：编写确定性计算脚本 `phase3_exp/mc/calc_sram_cost.py`，逐项按位宽计算单个伪通道（16 bank × 16,384 行，256 区域）的 SRAM 字节数：
  - 区域映射表（256 区域 × [2 × 6-bit 密钥 + 6-bit 指针 + 2-bit 状态机 + epoch 计数器]）；
  - Misra-Gries 检测器（16 bank × $k=15$ 项 × [14-bit 行号 + 10-bit 计数]）；
  - L2 软修复备用行映射表（16 bank × 16 备用项 × [14-bit 源行号 + 4-bit 备用索引 + 1-bit 有效位]）；
  - 计算侧新增的 `mapping_epoch` 寄存器开销。
- 输出 `phase3_exp/results/E7_hardware_cost.csv`，与 `E6_ablation.csv` 合并生成 **表 3**（`table3_ablation_and_cost.csv/.tex`）。

### 任务 M4.3：换位升级为 v2 真实 `FILL/MOV` 命令时序并复跑开销校验

- **执行指令**：
  1. 将 M2.2 中的换位 v1（功能性搬移）替换为 **v2 真实 `FILL/MOV` PIM 搬移命令时序**（按实际命令队列排队、占用 bank 与总线周期）。
  2. 复跑 **E4 全部正常负载开销与阈值扫描**，以及 **E5 的吞吐与开销子集**（`{W2, DEC} × {A1, A3} × {FIX, ADA, OURS}`），输出 `phase3_exp/results/M4_v2_timing_delta.csv`，明确对比 v1 与 v2 的开销差值，并确认 **OURS 零逃逸与相对排序结论完全不变**。
  3. 使用 v2 时序数据重新刷新 **图 5、图 6、图 7**。

### 任务 M4.4：全套论文图表终版生成与一致性总检

- **执行指令**：
  1. 沿用 Phase 1/2 的 `plot_paper.py` 风格，核对并打包全部 **8 图 3 表**：

| 编号 | 文件名 | 数据源 | 对应主张 |
|---|---|---|---|
| 图 1 | `fig1_system_closed_loop.pdf` | 系统设计（人工/矢量图定稿） | C4 |
| 图 2 | `fig2_e1_characterization.pdf` | `E1_benign_profile.csv` + `E1_attack_flips.csv` | C1 |
| 图 3 | `fig3_e3_attack_timeline.pdf` | `E3_prevention_timeline.csv` | C2 |
| 图 4 | `fig4_e2_mttf_heatmap.pdf` | `E2_mc_security.csv` | C2、C4 |
| 图 5 | `fig5_e4_overhead_vs_hc.pdf` | `E4_overhead_scaling.csv`（v2 时序更新） | C2、C5 |
| **图 6** | `fig6_overhead_vs_failure.pdf` | `E2_mc_security.csv` + `E5_campaign_summary.csv`（v2 更新） | **C4（主图）** |
| 图 7 | `fig7_e5_throughput.pdf` | `E5_campaign_summary.csv`（v2 更新） | C1、C4 |
| 图 8 | `fig8_e7_sensitivity.pdf` | `E7_sensitivity.csv` | C5 |
| 表 1 | `table1_fault_model_assumed.csv/.tex` | 第 1.1 节全部故障参数与 `ASSUMED` 清单 | 全部 |
| **表 2** | `table2_e5_campaign.csv/.tex` | `E5_campaign_summary.csv` + `E5_real_threshold_runs.csv` | **C1、C3、C4** |
| 表 3 | `table3_ablation_and_cost.csv/.tex` | `E6_ablation.csv` + `E7_hardware_cost.csv` | C4、C5 |

  2. 执行最终一致性审计：核对表 1 是否囊括所有 `ASSUMED` 项、缩放与真实阈值是否严格分列、所有零逃逸是否标注了 $3/N$ 上界与 Wilson CI。
- **最终交付点 DP-M4**：提交全套 `figures/`、`results/` 与 `reports/DP-M4_final_report.md`。

---

## 7. 附录：标准阶段交付报告模板与启动前人工确认项

### 7.1 阶段交付报告模板（`DP-Mx_report.md` 强制格式）

下级执行器在每个交付点必须严格按以下模板输出报告，禁止自由发挥长篇散文：

```markdown
# 交付点报告：DP-M<X>（对应决策门 G<Y>）

- **Git Commit / Config Hash**: `<commit>` / `<sha256>`
- **执行状态**: COMPLETED / BLOCKED
- **单元测试通过率**: `<passed>/<total>`（附测试命令与日志路径）

## 1. 产出文件核对清单
| 文件路径 | 行数 / 样本数 | 完整性校验（无 NaN / 无漏格） |
|---|---|---|
| ... | ... | PASS / FAIL |

## 2. 决策门 G<Y> 量化指标核对
| 判据项 | 要求门槛 | 实测结果（含 95% CI） | 判定 |
|---|---|---|---|
| ... | ... | ... | PASS / FAIL |

## 3. 异常、偏差与待人工裁决事项
- （若无异常填“无”；若触发 FAIL 或接口不匹配，仅客观列出事实与原始日志路径，严禁自行修改实验设计）

## 4. 停机确认
已停止后续任务，等待指令：`GO` / `FALLBACK` / `REVISE`。
```

### 7.2 交付给下级模型前的人类 5 项快速确认清单

本手册已将原设计中的 5 处 `ASSUMED` / 留白项锁定为 `[DEFAULT-LOCKED]` 默认值以消除执行器猜测。建议你在把本手册交给下级模型前快速确认一次（如认可默认值则无需修改）：

1. **$HC_{first}$ 分布形状**：默认锁定截断对数正态（下界 14,531，中位数 $1.35\times$，$\sigma=0.25$），E7 再测均匀/指数。
2. **VRD 波动因子区间**：默认锁定 $\mathrm{Uniform}(1.0, 3.5)$ 以保证下界不低于 14,531；若希望按均值居中波动，请改为明确区间。
3. **攻击强度与背景流量**：默认锁定攻击占主机带宽 `50%`，正常主机背景流量占 `20%`。
4. **DEC 解码步组合**：默认锁定 `3 × executeGemv → 残差 ADD → RELU` 顺序链。
5. **E5 的 HBM3 风格 4 格选择**：默认锁定 `{DEC} × {A1, A3} × {ADA, OURS}`。

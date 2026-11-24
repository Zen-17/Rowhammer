# DP-M2R 攻击链路修复

G3 仍是 REVISE。这次没有把它改成 GO，也没有进入 M3。设计点、刷新、真实阈值和 E2 蒙特卡洛都没有改。E4 没有重跑。

git commit `bd6b99c0ac428927b01a931e61f900fe9cffaa4b`。配置哈希 `4d5fb5dd33081e644eb2db5de6adefab880afcbf084b728f0f142abea3182212`，覆盖 `design_point.json`、`system_1ch_quiet.ini` 和 HBM2 ini。新结果在 `phase3_exp/results/m2r_attack_fix/E3_real_ref_on.csv`。16 格全部 SUCCESS。原始 M2 和上一轮 `results/m2r/` 没有覆盖。

## 1. 之前的 Host 写为什么没有变成足够的邻行 ACT

地址映射没有把行号翻译错。`Phase1Address::encode` 和 `PIMAddrManager::addrGen` 都按 Scheme8 从高到低排列 rank、row、column、bank group、bank-within-group、channel。`Phase1FeederMemory::decodeAddress` 调用 `AddrMapping::addressMapping`。测试 `Phase3M2R.HostWriteLandsOnEncodedRow` 把 bank 0、row 100 编码后再解码，得到的还是 bank 0、row 100。

真正丢掉激活的是开放页调度。系统配置是 `ROW_BUFFER_POLICY=open_page`，`QUEUING_STRUCTURE=per_rank`，`TOTAL_ROW_ACCESSES=65535`。`CommandQueue::pop` 先发出已经能发出的同行访问，只有 bank 空闲时才补 ACT（`CommandQueue.cpp` 的 `process_command`）。上一轮 E3 每 32 次攻击才排空一次队列。A1 在这 32 次里交替写两行，调度器把同一行收成一段 row hit。实测 32 次交替写只留下 2 次 ACT，和“每行打开一次”一致。上一轮真实阈值 NONE A1 的邻行 ACT 约 300，攻击写约 4900，也是大约每 32 次写 2 次 ACT。

## 2. 根因归类

不是地址映射错误，也不是故障阈值达不到。根因是 B：攻击请求在开放页队列里被收成 row hit，没有变成足够的 ACT。刷新没有被关掉。A3 另外受到攻击定义本身的限制，见第 6 节。

## 3. 改了什么

只改了刷新开启时的提交方式，文件 `phase3_exp/sim_hooks/Phase3M2Test.cpp`，`Phase3M2.Cell` 的 REF_ON 锤击循环。每一对 Host 写（一行攻击、一行背景）立即 `drain`，再发下一对。这样下一行到达时，上一行还开着，控制器必须先 PRE 再 ACT。没有向 `ReadDisturbanceModel` 直接注入 ACT，没有改 `noteActivate`，没有改阈值、刷新、换位周期或 A1–A4 的选行公式。带宽仍是每步一条 `HOST_ATK` 加一条 `HOST_BG`。

新增两个检查，都走真实的 Memory Controller：

- `Phase3M2R.HostWriteLandsOnEncodedRow`：映射往返一致；32 次写一次排空时只有 2 次 ACT。
- `Phase3M2R.AlternatingDrainProducesOneActPerAccess`：逐次排空后，32 步产生至少 30 次邻行 ACT，受害行扰动超过 16。

已有的 `Phase3M2R.NeighborActAndVictimRefresh` 和 `InjectionRequiresWrittenData` 仍只在单元测试里构造事件，不进入 E3 的攻击计数。

## 4. 修改后 Host 请求有没有打到预期物理行

有。真实阈值、W2、NONE、A1 这一格里，`neighbor_acts` 等于 `attack_transactions`，都是 104009。监视的是攻击开始时受害行两侧的物理行。每一条攻击写都变成了这两行上的 ACT。

换位之后这个计数不再代表当前受害行。FIX、ADA、OURS 的 `neighbor_acts` 仍盯着换位前的那两行，所以比攻击写少。当前受害行的扰动看 `peak_ratio`。

## 5. REF_ON、真实阈值下 NONE 会不会翻转

会，除了 A3。一格、种子见 CSV。

| 攻击 | 注入 | 首次跨阈 | 残留 | 峰值 D/阈值 | 邻行 ACT | 攻击写 | REF 命令 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| A1 | 4 | 2 | 4 | 1.251 | 104009 | 104009 | 5 |
| A2 | 27 | 4 | 27 | 1.250 | 112744 | 901952 | 5 |
| A3 | 0 | 0 | 0 | 0.002 | 122 | 1024 | 6 |
| A4 | 9 | 2 | 9 | 1.251 | 146412 | 146412 | 6 |

A2 的邻行 ACT 是攻击写的 1/8，因为 8 对里只有一对贴着受害行。这和原来的 A2 间距一致。

## 6. 若仍不翻转，限制是什么

A3 仍不翻转。它的长度锁在 1024 次攻击，17 个位置里只有 2 个是邻行，邻行 ACT 只有 122。首次翻转阈值至少是 `2 * 14531`。这是攻击定义本身到不了阈值，不是映射还错着。没有把 A3 加长。

另外，NONE 的 A1 在约 527 万周期里只完成了 5 条 REF。`CommandQueue::pop` 确实优先处理刷新，但开放页让行保持 Active，`process_refresh` 要先把 16 个 bank 都预充电才能发出 REF。锤击不停地把行打开，大多数刷新请求没有变成 REF 命令。这 5 条 REF 按观察器的侧指针每次清两行，从 0 号行起，清不到 640 号受害行。所以这次 NONE 的翻转发生时，刷新开关是开的，受害行自己没有被刷新清掉。这是日志和现有刷新代码能支持的分析，不是新加的刷新推迟。

## 7. 同样条件下防御有没有收益

A1、A2、A4 有。三套方案的注入、首次跨阈、擦洗和残留都是 0，峰值扰动远低于 NONE 的 1.25。擦洗计数是 0，是因为扰动在跨阈前被换位清掉，不是先翻转再擦掉。

| 方案 | A1 峰值 | A2 峰值 | A4 峰值 | A1 换位 | A4 换位附加 ACT |
| --- | --- | --- | --- | --- | --- |
| FIX | 0.029 | 0.072 | 0.001 | 1935 | 132640 |
| ADA | 0.021 | 0.108 | 0.016 | 1576 | 37728 |
| OURS | 0.013 | 0.124 | 0.023 | 1166 | 54208 |

A4 的换位附加 ACT 没有把当前受害行推过阈值。NONE 的 A4 在同一预算下翻转了 9 次。

A3 四套方案都是 0 次注入，峰值约 0.002。这里只能写“当前测试未观察到失效”，不能写防御有效。

每格只有 1 次运行，工作负载只有 W2，阈值是真实 14531。没有和 1/16 缩放混在一张表里。

## 8. 对 E1、E2、E4 的影响

没有。E2 仍是原来的蒙特卡洛。E4 走的是 `NO_ATK`，不进这段锤击循环。E1 的正常负载实验没有改。上一轮 E4 的开销数继续有效。

## 9. 要不要重跑 M1

这次不需要，也没有重跑。M1 的锤击仍是摘掉刷新观察后显式 `noteActivate` 的那条诊断路径，这次没有动它。M1 的检测和恢复数字不能改写成这次的物理 ACT 结果。

## 10. G3 能不能重新验收

可以提交人工重裁，这次不能由实验脚本改成 GO。

已经站住的事实：物理邻行 ACT 可以和 Host 攻击写一一对应；在 REF_ON 和真实阈值下，NONE 的 A1、A2、A4 会翻转；FIX、ADA、OURS 在同一种子和同一预算下注入为 0，峰值扰动低于阈值。

还没有覆盖的部分：每格只有一次运行；A3 不能用来证明防御；NONE 路径上的刷新命令远少于 `tREFI` 对应的次数，受害行本身没有被刷新命中。若要在“受害行按 tREFW 被刷新”的前提下重做安全结论，那是刷新调度行为的改动，需要另外批准。这次没有做。

复现：

```bash
cd /opt/data/data/workspace-dynamic/Rowhammer
LD_LIBRARY_PATH=/opt/data/data/anaconda3/lib \
  ./phase3_exp/build/simulator/sim --gtest_filter='Phase3M2R.HostWrite*:Phase3M2R.Alternating*'
PYTHONPATH=/opt/data/data/workspace-dynamic/Rowhammer \
  /opt/data/data/anaconda3/bin/python3 phase3_exp/scripts/run_m2r_attack_fix.py
```

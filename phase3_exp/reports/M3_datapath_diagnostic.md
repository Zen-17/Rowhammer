# M3 数据路径诊断

正式战役仍暂停。`runs/M3/e5c` 及已有汇总没有改写。本轮只增加 `Phase3M3.DataPath`，不改 ECC、分级防护、性能计费和 M3 单元里的攻击与检查。

诊断命令：`sim --gtest_filter=Phase3M3.DataPath`。结果：1/1 通过，983 ms。记录在 `phase3_exp/results/m3/datapath/diagnostic.json`。

## 实际数据流

W2 使用现有 `runKernel` 的三段 `executeEltwise`，维度 `48×128 = 6144` 个 burst。

1. 输入由 Phase 1 的 `PIMKernel::preloadNoReplacement` 写入：第 0 行、第 128 行、第 384 行。
2. ADD 从第 0 行和第 128 行读操作数，结果写入第 256 行。`PIMRank::readOpd` 对偶/奇 bank 调用 `Bank::read`，`PIMBlock::add` 做 FP16 加法。
3. MUL 从第 256 行和第 384 行读，结果写入第 512 行。
4. RELU 从第 512 行读。`doPIMBlock` 把符号位为 1 的 FP16 车道写成 0，再写入第 640 行。
5. 提交使用 Phase 1 的 `PIMKernel::readData`，从第 640 行线性读回 6144 个 burst。

独立检查在主机上用同一套 `BurstType` FP16 加法和乘法，再按符号位做 RELU。它不运行第二个仿真器，也不把期望结果写回 DRAM。

DEC 的提交结果是第 2384 行，来自 campaign 同一对调用：ADD 读第 2000 行和第 2128 行，写入第 2256 行；RELU 读第 2256 行，写入第 2384 行。维度是 128 个 burst。诊断用 `preloadNoReplacement` 写入这两个操作数行。GEMV 不在这条读链上。

`executeGemv` 的输入向量经 `WRIO_TO_GRF_` 从主机缓冲进入 GRF，不读 DRAM。权重由 `MAC_` 从 DRAM 读出，行号是 `phase3GemvRowBias`，战役里为 0。GEMV 结果写回同一行的另一些列。随后的 ADD 不读第 0 行。战役也没有预加载第 2000 行和第 2128 行。因此现有 DEC 战役的第 2384 行不是 GEMV 的计算结果。

## 诊断结果

无故障时，PIM 读回与主机独立计算一致：W2 6144 个 burst 的差异数为 0，DEC 128 个 burst 的差异数为 0。预加载后读 bank 0、行 0、列 0，与第一个输入 burst 一致。

存储故障是 `Bank::injectRawDataBit`，只翻转已写入操作数的一个数据位，不改攻击参数。翻转第 0 行第 0 个 burst 的符号位后，W2 第 640 行有 1 个 burst 改变。翻转第 2000 行第 0 个 burst 的符号位后，DEC 第 2384 行有 1 个 burst 改变。

恢复只把原来的操作数 burst 写回它自己的地址，然后重新执行内核。两边的结果重新与主机计算一致，差异数为 0。没有把期望输出写回结果行。

对照：向 W2 不读取的第 900 行写入并翻转一位后，第 640 行相对恢复后的结果改变 0 个 burst。

第一次用尾数最低位做 W2 故障时，操作数已改变，但 FP16 加法把它舍入掉，输出差异数为 0。改用符号位后输出才变化。能影响结果的是运算保留下来的存储位，不是任意一位。

## 尚未解决

- M3 正式单元仍比较无计算依赖的主机图案，恢复仍会把该图案写回。这次没有改它，也没有重跑战役。
- DEC 的 GEMV 与第 2384 行之间没有操作数关系。本诊断证明的是 ADD/RELU 这条真实读链，不是 GEMV 保护了解码结果。
- ECC 反馈、区域升级和 188 周期计费没有动。
- 正确性检查还没有接进战役的计分。当前证据只覆盖这一次最小诊断。

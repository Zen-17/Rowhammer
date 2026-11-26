# M3 ECC 与事件反馈诊断

正式战役仍暂停。`runs/M3/e5c`、已有汇总、`Phase3M3.DataPath` 的断言和 `results/m3/datapath/diagnostic.json` 没有改写。SECDED 的纠错能力、计算恢复和性能计费没有改。没有调用 `RegionMap::noteCe` 的故障计数入口，也没有调用按注入位重翻的 `scrubSingleBits`。

诊断命令：`sim --gtest_filter=Phase3M3.EccFeedback`。结果：1/1 通过，86 ms。记录在 `phase3_exp/results/m3/datapath_ecc/ecc_diagnostic.json`。

## 覆盖范围

PIM 操作数读取是 `PIMRank::readOpd`。偶 bank 和奇 bank 都调用 `Bank::read(..., Phase1AccessType::PIM_OPERAND, ...)`，然后把 `packet->data` 交给 `PIMBlock`。

`Bank::eccEnabled` 对三种剖面的 PIM 读取并不相同：

- `NATIVE`：不译码，直接返回 `rowEntries`。战役 `Session` 的默认剖面是这个。
- `HOST_ONLY`：只有 `HOST` 访问译码。PIM 读取走原始 `phase1Storage_`，不经过译码器。
- `LOCAL_VISIBLE` 和 `LOCAL_HIDDEN`：所有访问类型都译码，包括 `PIM_OPERAND`。

受保护读取把 `Phase1Ecc::decode` 的 `result.data` 拷进 `packet->data`。单比特时这是纠正后的数据字，所以 PIM 拿到的是纠正后的操作数。双比特时译码器把状态标成 `UNCORRECTABLE`，数据字保持进入译码器时的损坏值，PIM 拿到的是损坏操作数。

本诊断在预加载之前把该 rank 的每个 bank 设为已有的 `LOCAL_VISIBLE`，并打开已有的 `setPhase1Scrub(true)`。没有新增纠错码。

## 事件从哪里来

故障只调用 `Bank::injectRawDataBit`。非 `NATIVE` 时它只对已编码的数据字做异或，不写事件，也不把比特位置交给译码器或清扫。

`Bank::read` 在状态为 `CORRECTED` 或 `UNCORRECTABLE` 时调用 `reportDetection`。`Phase1Reports::onDetected` 只根据译码状态计数：`CORRECTED` 增加 `internalCe` 并调用 `phase3NoteCe`；`UNCORRECTABLE` 增加 `internalUe`，不调用 `phase3NoteCe`。注入器没有这两条调用。

清扫只在 `CORRECTED && phase1Scrub_` 时执行，写入的是 `writeLegal(key, result.data)`。`result.data` 是译码器输出，随后 `writeLegal` 重新编码校验位。它不读取注入器保存的比特位置，也不读取一份另存的正确原值。代码注释写明这次回写是理想化的：没有总线事务，也没有周期代价。本轮按原样保留，没有补计费。

`Session` 构造时已经执行 `phase3SetCeHook(&bridgeCe, &region)`，`bridgeCe` 调用现有的 `RegionMap::noteCe`。诊断使用 `OURS`，因此 `ceFeedback_` 为真。没有执行 `flushSwaps`，所以没有走到会调用 `scrubSingleBits` 的换行清扫。

## 三种情况

工作负载沿用已通过的 DEC 操作数链：128 个 burst，ADD 读第 2000 行和第 2128 行并写入第 2256 行，RELU 写入第 2384 行。主机期望值用同一套 `BurstType` FP16 加法再按符号位做 RELU。检查的是 PIM 读回，不把期望值写回 DRAM。每次只在 bank 0、第 2000 行、第 0 列的同一个 64 位码字里翻转存储位。单比特翻第 14 位，双比特再翻第 15 位。

| 情况 | 输出差异 | CE | UE | 清扫写 | 区域级别 | 操作数回读 |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| 无故障 | 0 | 0 | 0 | — | 0 | — |
| 单比特 | 0 | 1 | 0 | 1 | 1 | 与原操作数一致；再次读取不再产生 CE |
| 双比特 | 1 | 0 | 1 | 0 | 0 | 仍与原操作数不同；再次读取 UE 从 1 增到 2 |

三种结果对应三种含义：

- 无故障：译码为 `CLEAN`，PIM 结果与主机计算一致，没有事件，级别保持 0。
- 单比特：译码纠正成功。PIM 用纠正后的操作数算出与无故障相同的结果。CE 由这次译码产生，并把第 2000 行所在区域从 0 升到 1。清扫按译码结果重写该码字，随后的主机读回与原操作数一致，且没有新的 CE。
- 双比特：错误被检出但不可纠正。没有 CE，级别保持 0，也没有清扫写。损坏的数据字进入加法，第 2384 行有 1 个 burst 与主机期望不同。这是错误传播到计算结果，不是纠正成功。

## 判定

1. PIM 能否拿到纠正后的数据：通过。`LOCAL_VISIBLE` 下 `PIM_OPERAND` 会译码，单比特的计算结果与无故障一致。`NATIVE` 和 `HOST_ONLY` 的 PIM 读取不译码；战役默认仍是 `NATIVE`。
2. CE、DUE 是否来自译码而不是注入器：通过。无故障为 0/0，单比特为 CE=1、UE=0，双比特为 CE=0、UE=1。计数只出现在 `onDetected`。
3. 清扫是否使用译码结果：通过。单比特产生 1 次 `writeLegal(result.data)`，双比特为 0。回读证明单比特存储已被修复，双比特存储仍坏。没有把注入位或原始正确值交给清扫。
4. 真实 CE 能否进入现有分级状态机：通过。单比特经 `phase3NoteCe` 进入 `RegionMap::noteCe`，级别变为 1。双比特的 UE 不调用该钩子，级别保持 0。
5. 三种小规模诊断：通过。上表即该次运行的记录。

## 尚未解决

- 正式战役单元仍是 `NATIVE`，仍在故障计数大于 0 时直接 `noteCe`，换行清扫仍调用 `scrubSingleBits`。这次没有改战役单元，也没有重跑战役。
- Phase 1 清扫没有总线事务和周期代价。性能计费没有改。
- `HOST_ONLY` 的 PIM 读取仍不译码。
- 双比特目前会把损坏数据送进计算。没有增加隔离或改变纠错能力。
- 本诊断只覆盖 128 个 burst 的 ADD/RELU 和其中一个码字。没有覆盖 W2 全长，也没有覆盖 GEMV。
- 只观察到级别 0 到 1。没有执行换行，因此没有进入备用行。换行会调用按注入位清扫的旧路径，本轮故意不触发它。
- 计算恢复、战役计分和 G4 都没有改。G4 仍是 PENDING。

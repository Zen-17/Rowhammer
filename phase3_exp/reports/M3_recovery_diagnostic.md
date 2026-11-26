# M3 计算恢复诊断

正式战役仍暂停。SECDED 纠错能力、攻击参数、历史结果和性能计费没有改。`Phase3M3.DataPath` 与 `Phase3M3.EccFeedback` 的断言没有改。恢复判定使用 Phase 2 已有的 `RecoveryEngine`，没有新增一套根据注入位或期望输出触发的恢复。

诊断命令：`sim --gtest_filter=Phase3M3.Recovery`。结果：1/1 通过，143 ms。记录在 `phase3_exp/results/m3/datapath_recovery/recovery_diagnostic.json`。

## 事件链路

工作负载仍是已通过的 128 burst ADD/RELU。操作数在第 2000 行和第 2128 行，ADD 写入第 2256 行，RELU 写入第 2384 行。剖面是已有的 `LOCAL_VISIBLE`，清扫开关打开。

1. PIM 经 `PIMRank::readOpd` 调用 `Bank::read(PIM_OPERAND)`。译码器对单比特返回 `CORRECTED`，对双比特返回 `UNCORRECTABLE` 且数据字保持损坏值。
2. `Phase1Reports::onDetected` 把这两种状态分别记成公开报告 `CE` 和 `UE`。注入器只异或存储位，不写报告，也不调用恢复。
3. 内核结束后，诊断把这次真实执行记入已有的 `Ledger`：ADD 读操作数 0 和 1，RELU 读 ADD 的输出。时间用 feeder 在内核前后的时钟，不使用故障模型的翻转名单。
4. 结果引用先进入 `Gate::hold`。此时主机还没有读第 2384 行。
5. 公开报告交给 `RecoveryEngine::analyze`，策略是已有的 `B2_CLASS_AWARE`。CE 不产生重放；UE 才把读过该码字的操作及其下游放进重放计划。
6. 计划要求重放时，只按 `decision.restore_list` 从 `Phase2CheckpointStore` 取出信任输入，用 `preloadNoReplacement` 写回对应基址，再按 `replay_runs` 重新执行真实的 `executeEltwise`。
7. 计划判定没有信任输入时，`planExecution` 给出 poison 列表，`Gate::poison` 把结果标成 `POISONED`。这条路径不调用 `Gate::publish`，也不把结果放进提交缓冲。
8. 只有 `PUBLISHED` 之后，才通过 `phase3LogicalRow` 得到的逻辑行调用 `readData`。本次没有换行，逻辑行等于 2384。

主机上的 `applyRelu(left + right)` 只在读回之后做比较。它不参与是否恢复，也不写回 DRAM。

## 信任输入

来源是预加载前的主机输入向量。保存位置是 `Phase2CheckpointStore`，粒度是一个 tile：128 个 burst，对应基址上的 16 个 bank、8 个列。诊断保存的是操作数 0 和操作数 1，不保存期望输出。

UE 成功那一组实际恢复了 1 个 tile。该 tile 与操作数 0 的主机向量一致，并且与期望输出不同。恢复列表里出现的是 version 0 的初始输入；中间结果靠重放 ADD 重新生成，不从检查点拷贝。

恢复失败那一组对操作数 0 调用已有的 `CheckpointView::setAbsent`，并且检查点里没有这个 tile。这是 Phase 2 表示“没有信任副本”的接口。引擎因此返回 `OUTSIDE_WINDOW_NO_TRUSTED_INPUT`。诊断没有改用保存的输入或期望输出去覆盖这个判定。

## 诊断结果

| 情况 | 提交 | 输出差异 | 重放 tile | 报告 | 引擎状态 |
| --- | --- | ---: | ---: | --- | --- |
| 无故障 | 是 | 0 | 0 | 0 | 无报告，直接放行 |
| 单比特 CE | 是 | 0 | 0 | CE | `SOURCE_REPAIRED_ONLY` |
| 双比特 UE，检查点可用 | 是 | 0 | 2 | UE | `SELECTIVE_RECOVERED`，恢复 1 个输入 tile |
| 双比特 UE，信任输入缺失 | 否 | 未提交 | 0 | UE | `OUTSIDE_WINDOW_NO_TRUSTED_INPUT`，poison 1 项并覆盖结果 |

无故障时没有 ECC 事件，结果与主机计算一致后提交。

CE 由译码产生。B2 不重放。PIM 在读操作数时已经拿到纠正后的数据，提交结果与主机计算一致，检查点没有被写回。

UE 由译码产生。引擎要求重放 2 个 tile，对应 ADD 和它的下游 RELU。恢复的是操作数 0，不是期望输出。重放后的提交结果与主机计算一致，差异数为 0。

信任输入缺失时，结果保持 `HELD` 之后被标成 `POISONED`，`published` 为假，提交缓冲为空。离线探查第 2384 行，有 1 个 burst 与主机计算不同。这次探查发生在拒绝提交之后，不改变门闩状态，也不算提交。

## 实现与假设

实际执行的部分：

- ECC 译码、CE/UE 报告、B2 判定、检查点保存的输入字节、输入写回和第二次 `executeEltwise` 都走现有接口。
- 成功提交的字节来自重放后的 PIM 读回，或者 CE 路径上那次已被纠正的 PIM 计算。

模型假设，本次没有改成硬件强制：

- `Gate` 是 Phase 2 的发布记录。`Bank::read` 不会因为结果仍是 `HELD` 或 `POISONED` 而拒绝主机读取。诊断把 `Gate::publish` 当作唯一提交动作；失败路径不发布。
- 账本由诊断在真实内核前后写入，使用的是 feeder 时钟和这次 ADD/RELU 的操作数关系。它不是 Phase 2 命令观察器自动记下的，也没有因此少跑 PIM。
- Phase 1 清扫仍是译码后的直接回写，没有总线事务和周期代价。本轮没有补计费。

## 判定

1. UE 到达运行时恢复，且不由注入器或 Golden Reference 触发：通过。公开报告类是 `UE`，状态是 `SELECTIVE_RECOVERED`。
2. 错误结果在对外可见前被拦住：通过。失败组 `published=false`、`submitted=false`，poison 覆盖结果引用。拦截点是发布门闩，不是 DRAM 读命令本身。
3. 信任输入、恢复粒度、重放真实内核：通过。恢复 1 个初始输入 tile，重放 2 个 kernel tile，没有写回期望输出。
4. 用逻辑地址和计算结果路径做独立验证：通过。提交读的是逻辑行 2384 的 `readData`。Golden 只比较已提交或被扣留的字节。
5. 四组诊断：通过。上表即这次运行的记录。

验收结论：在这四组里，真实 ECC 事件驱动了 B2。受影响的结果要么在恢复重放后正确提交，要么在没有信任输入时被阻止提交。

## 尚未解决

- 正式战役单元仍用主机图案比较，并在失配后把该图案写回。这次没有改它，也没有重跑战役。
- 发布门闩没有接到内存控制器。一个不经过 `Gate::publish` 的主机读仍能看到 DRAM 里的结果。
- 账本不是命令观察器在线记录的。战役规模的多 tile 依赖还没有用这条诊断覆盖。
- 只覆盖一个 tile 的 ADD/RELU。没有覆盖 W2 全长，也没有覆盖 GEMV。
- 清扫的周期代价和 G4 没有改。G4 仍是 PENDING。

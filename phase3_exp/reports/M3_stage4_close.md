# Stage 4 收尾与正式实验集成前检查

本文接在 `M3_consistency_fix.md` 之后。只做了小规模联合诊断 `Phase3M3.Stage4Close`，并复跑 DataPath、EccFeedback、Recovery、MapFix、EpochFix、PublishFix、LedgerFix、JointFix。九项均通过。验收依据是 JSON 读数，不是 gtest 本身。

没有改故障模型、攻击参数、SECDED 能力或 G4 原标准。没有覆盖 e5c 历史 CSV，没有重画 fig6/fig7，没有启动 4800 次战役，没有改 `TEST(Phase3M3, Cell)`。`configs/gate_state.json` 仍是 G4=PENDING。

## 1. 发布控制 — PASS

被拦截的逻辑读把缓冲区清零，置 `phase3_operand_denied`，并且立刻返回，不拷贝存储。PIM 在同一次 HAB_PIM 里记住这个拒绝。随后的 ADD 不再写 GRF，`GRF_TO_BANK` 对应的 NOP 写回和 `writeHab` 的寄存器堆搬移也不再落存储。因此既不会把全零当成操作数算出一个新结果，也不会把上一轮内核留在 GRF 里的值写进目标行。

`Phase1Runtime` 未重写。诊断直接驱动它：

- 暂停：`quiesceOutput` 后状态为 `QUIESCED`，`consume` 返回假。
- 重试：`beginRestore`、`beginReexecute`、`producerComplete` 之后，`tick(7)` 仍是 `READY_UNPUBLISHED`，`tick(8)` 可消费。门闩 `publish` 后，同一条 ADD 把目标行写成真实翻倍值。
- 取消：发布前收到 UE，状态为 `POISONED`，`consume` 返回假。被扣留或投毒时，目标行保持预填的哨兵，不是全零，也不是翻倍和。
- 失效句柄：再次 `allocateOutput` 后旧句柄 `isStale`，不能再消费。

边界仍在：`HELD` 期间写可以进入存储；未绑定行可读；`HOST_ATK` 不走这道门。

读数：

- `results/m3/datapath_stage4/stage4_close.json`：`zero_not_stored`、`paused`、`retried`、`retry_matches`、`cancelled`、`cancel_not_stored`、`stale_handle` 均为 true。
- `results/m3/datapath_publishfix/publishfix.json`：`held_pim_blocked` 与 `poisoned_pim_blocked` 均为 true，拦截计数 896。`published_pim` 为 true。

## 2. 代际校验 — PASS

`Region.epoch` 是地址映射的代际，不是数据版本。一个区域把一轮换行走完、`nextPair` 折返时加一。逻辑命令在执行时用 `phase3PhysicalRow` 翻译。

请求在进入 feeder 时盖章，盖的是当时 `phase3LogicalEpoch(bank, logicalRow)`。控制器接受时原样抄到总线包上，不再按接受时刻重盖。执行时若印章和当前代际不同，这次读被取消：缓冲区清零，不拷贝存储，PIM 不写回结果，并计一次过期请求。换行之后新发出的逻辑读使用新印章，仍然读到原数据。

报告要同时满足两件事才可恢复：逆映射能往返，且报告上的 `mapping_epoch` 等于该物理行当前代际。缺一则 `RecoveryEngine::analyze` 返回 `STALE_EPOCH`，恢复列表和重放都为空。代际已经前进、逆映射仍能往返的旧报告会被拒绝。把印章改成当前代际加 5 的伪造报告同样被拒绝。换行之后新产生的报告不是 `STALE_EPOCH`。

读数：

- `stage4_close.json`：`stale_denied`、`stale_zeroed`、`rebound_matches`、`old_report_rejected`、`forged_report_rejected`、`fresh_report_usable` 均为 true。
- `results/m3/datapath_epochfix/epochfix.json`：`old_epoch_accepted` 为 false，`advanced_status` 为 `STALE_EPOCH`。逆映射被破坏的访问仍是 `access_withheld`，引擎状态 `STALE_EPOCH`，没有恢复。

这覆盖了真实过期请求和错误报告，不再只检查正反向地址是否一致。`M3_consistency_fix.md` 第 2 节里“版本号前进仍接受”的说法作废。

## 3. Ledger 接口 — PASS

预声明依赖是合理的软件运行时接口，本阶段不实现自动依赖推断。

假设：

- 发出 PIM 命令的软件已经知道操作数、块号和基行。运行时接口就是 `planOp` 声明边，以及 `setBaseRow` 声明基行。
- 命令路径只做两件事：`recordIssue` 每条操作只计一次；`observeAccess` 记下实际碰到的行。
- `completeObserved` 仅当观察到的读行集合、写行集合分别等于声明的输入基行、输出基行时，才把该操作标成完成并填写版本。
- 指令流不发明边。声明错了就保持未完成。
- `addCompleted` 仍能在没有命令时写入完成记录。它只留给既有 Phase 2 单测，不能当作执行证据。诊断和联合恢复不用它充当命令已执行的证明。

`results/m3/datapath_ledgerfix/ledgerfix.json`：真实 ADD 观察到 2 个读行和 1 个写行，RELU 观察到 1 个读行和 1 个写行，版本能接上，重复 issue 只计一次。把基行故意写成 1000/1128/1256 时，`lie_rejected` 为 true。`stage4_close.json` 的 `ledger_predeclared` 只标记这条接口约定，不是推断结果。

## 4. 正式实验集成前检查 — PASS

小规模诊断用的是已通过的 Stage 1–4 路径，逻辑结果行是 DEC 的 2384。它没有调用战役单元里的四条旧路径：

- 不比较主机图案。单比特纠正后，逻辑行 2384 与 `applyRelu(left+right)` 一致；内部 CE 增加，`cumulativeFlips` 仍为 0。
- 不调用 `flipsOn` 再 `noteCe`。CE 只来自译码。
- 不调用 `scrubSingleBits` 作为这条诊断的清除手段。单比特由 SECDED 写回纠正后的码字。
- 不把主机图案写回结果行。双比特时内部 UE 增加，`cumulativeFlips` 仍为 0，逻辑结果与期望不同。检查点里的 tile 0 等于输入 `left`，不等于期望输出。把这份输入写回逻辑行 2000 后重跑 ADD 和 RELU，逻辑行 2384 与期望一致。

`stage4_close.json`：`ecc_not_oracle`、`corrected_matches`、`uncorrectable_differs`、`restored_input`、`recovered_matches` 均为 true。

这些机制足够替换战役单元里的主机图案比较、Oracle CE、伪清扫和伪重放。`TEST(Phase3M3, Cell)` 本身还没有换上。它仍然用 `HOST_BG` 访问 `physical()` 返回的行号，用 `flipsOn` 调用 `noteCe`，用第二份 NONE 会话的主机图案做 32 字节比较，恢复时把原图案写回物理行，换行费用仍是 `moves.size() * 4 * 47`。换行回调里的 `scrubSingleBits` 也还在。因此这次检查不能当作战役已经切到新路径。

## 是否进入 Stage 5

可以进入 Stage 5。Stage 5 只做两件事：把战役单元的结果检查、CE、恢复接到上面已验证的路径，并按真实总线占用计算换行。先做一次小的单元冒烟，再决定是否把 E5 写入新目录。

现在不能做的事：不启动 4800 次战役，不重画 fig6/fig7，不把 G4 写成 GO，不进入 M4。G4 的两条原标准都还没有用新路径重测。

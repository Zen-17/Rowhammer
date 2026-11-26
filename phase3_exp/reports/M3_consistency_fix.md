# Stage 4 一致性修复

修复前的测量仍保留在 `M3_consistency_diagnostic.md` 和 `results/m3/datapath_consistency/consistency_diagnostic.json`。本文只记录修复后的独立诊断。没有重跑 E5/E6，没有改攻击参数、SECDED 译码、性能计费或历史 CSV，也没有把 G4 写成 GO。

第 2 节和第 4 节的结论已被 `M3_stage4_close.md` 取代：过期 epoch 现在会被拒绝，预声明依赖作为软件接口予以接受。下面正文保留当时的测量说明。

`Phase3M3.Consistency` 会覆盖那份修复前的 JSON，因此改为跳过。下面五项是新的测量。gtest 通过只说明断言和记录一致；验收依据是各 JSON 里的机制读数。

## 1. 统一地址映射 — PASS

依据：换行原先把码字解码后再 `writeLegal`，可纠正错误被重新编码成干净数据，不可纠正错误则留在旧行。`RegionMap` 的 L2 只搬一个 bank，但 `physical()` 对所有 bank 指向同一备用行。Host、PIM 和 ECC 都按报文里的行号直接访问。

修改：

- `Phase1Storage::moveCodeword` 连同数据、校验位和 `ecc_valid` 一起搬，并删掉源键，不重新编码。
- `Bank::exchangeRows` 改走这条搬迁。
- 逻辑数据标签（空标签预加载、`HOST_BG`、`output`、PIM 运算标签）经 `phase3PhysicalRow` 翻译。`HOST_ATK` 仍是物理行。
- 报告里的 codeword `region_id` 用逻辑行，`noteCe` 仍用实际读到的物理行。
- L2 对 16 个 bank 使用同一备用行。

诊断 `results/m3/datapath_mapfix/mapfix.json`：

- 换行后逻辑行 0 的 Host、`HOST_BG` 和 PIM ADD 都与原数据一致；物理行是 1。源码字不在，目标码字的原始数据和校验位与搬迁前相同。
- `HOST_ATK` 读逻辑行号 0 对不上原数据，读物理行 1 才对上。锤击地址没有被当成逻辑地址。
- 双比特错误搬迁后原始数据和校验位都保留。逻辑读两次，内部 UE 计数从 1 增到 2，读回值不是干净原值。
- 两次真实 CE 后进入 L2，备用行 16368。逻辑行整块读回与原数据一致，bank 15 的码字在备用行而不在旧行。报告的 `logical_row` 和 `region_id` 都是 0。

`Phase3M3.DataPath`、`EccFeedback`、`Recovery` 在接入翻译之后重新通过。无换行时正映射和逆映射都是恒等，这三项的读数没有被改写。

## 2. 真实代际校验 — PARTIAL

依据：`OURS-EPOCH` 只改 `logical()` 的返回值。访问、报告和 `RecoveryEngine::analyze` 都不比较逆映射，所以仍把错误逻辑行上的原数据当成有效读，并给出 `SOURCE_REPAIRED_ONLY`。

修改没有改 `logical()`。`phase3MappingAccepted` 要求 `logical(physical(row)) == row` 且正向再映射回来。逆映射对不上时，逻辑读把返回缓冲区清零，不把存储字节交给调用者；ECC 仍照常解码、报告和擦洗。报告保留错误的 `logical_row`，并记下 `mapping_consistent=false`。引擎在恢复前重新做同一次校验，失败则返回 `STALE_EPOCH`，恢复列表和重放都为空。版本号已经前进、但逻辑行仍能往返时，按当前正映射重新翻译，不拒绝。

诊断 `results/m3/datapath_epochfix/epochfix.json`：

- 过期逆映射下，逻辑读拿不到原数据；PIM ADD 写进结果行的原始字也不是正确的和。
- 报告 `logical_row=1`，`mapping_consistent=false`。引擎状态是 `STALE_EPOCH`，没有恢复。
- 正常 OURS 完成一轮换行后 epoch 从 0 变为 1。旧报告仍是 `SOURCE_REPAIRED_ONLY`，逻辑行 0 的再读与原数据一致。epoch 编号前进本身不会被拒绝：`phase3MappingAccepted` 在逆映射仍能往返时直接返回真。能识别的是被 `OURS-EPOCH` 破坏的逆映射，不是单独的版本号。

## 3. 真实发布控制 — PASS

依据：`Gate::hold` / `poison` 不被 `Bank::read` 查阅，Host 和下游 PIM 仍读到被扣留或已投毒的结果。

修改：`phase3BindPublication` 把逻辑行绑到门闩里的 `DataRef`。绑定行处于 `HELD` 或 `POISONED` 时，逻辑读清零返回并计数；写和未绑定行不受影响。`HOST_ATK` 不是消费路径，不拦截。`PUBLISHED` 之后逻辑读恢复。

诊断 `results/m3/datapath_publishfix/publishfix.json`：

- 结果行在 `HELD` 期间仍能被 ADD 写上正确结果。Host 逻辑读被挡住，下游 ADD 的物理结果不是翻倍值。未绑定的输入行仍可读。`HOST_ATK` 仍能看到结果单元。拦截计数 896。
- `publish` 之后 Host 和下游 PIM 都读到正确结果。
- `poison` 之后两者再次被挡住。投毒期间重写能进入存储，逻辑读仍拿不到新值。

## 4. 账本一致性 — PARTIAL

依据：`recordComplete` 不核对命令行号，`recordIssue` 每次调用都增加 `total_logged_ops_`。`addCompleted` 可以在没有命令的情况下写成已执行。

修改：`recordIssue` 只在第一次记录时计数。新的 `observeAccess` / `completeObserved` 要求观察到的读行集合、写行集合分别等于声明输入、输出的基行；对不上就保持未完成。`addCompleted` 保留给既有 Phase 2 单测，本阶段的诊断和联合恢复都不调用它。

诊断 `results/m3/datapath_ledgerfix/ledgerfix.json`：

- 真实 ADD 观察到 2 个读行和 1 个写行，完成后输出版本等于该操作编号。RELU 观察到 1 个读行和 1 个写行，其输入版本等于 ADD 的操作编号。两次 `recordIssue` 只各计 1 条。
- 故意把基行声明成 1000/1128/1256 时，同样的 ADD 命令跑完后操作仍未完成，但 issue 时间已经记下。输入输出边仍来自诊断里的 `planOp`。完成回调只核对行集合并填写时间和版本，不会从指令流推断依赖。

## 5. 联合诊断 — PASS

依据：修复前 L2 只搬一个 bank，PIM 看不到 UE；选择性恢复之后按逻辑结果行比较，128 个突发全都不对。

诊断 `results/m3/datapath_jointfix/jointfix.json`，方案为 OURS，行号全程使用逻辑地址：

- 换行后再注入两次单比特 CE，区域进入 level 2，备用行 16368。逻辑行 0 仍与原输入一致，最后一个 bank 的码字在备用行。
- 在备用行注入双比特后，Host 逻辑读和随后的 PIM ADD 都使内部 UE 增加。
- 账本由命令观察完成，RELU 的输入版本等于 ADD 编号。没有调用 `addCompleted`。
- UE 报告的逻辑行是 0。B2 状态 `SELECTIVE_RECOVERED`，恢复 1 个版本为 0 的输入块，重放 2 个操作。恢复内容是检查点里的输入，不是期望输出。
- 重放后的结果在发布前被 `HELD` 挡住。物理读能看到正确的首个突发。发布后按逻辑行 384 比较，失配为 0。

## 回归

同一次运行里，`Phase2Engine` 25 项、`Phase3M3.DataPath`、`EccFeedback`、`Recovery`、`MapFix`、`EpochFix`、`PublishFix`、`LedgerFix`、`JointFix` 均为通过。这不能代替上面的 JSON 读数。

## 剩余问题

- G4 仍是 PENDING，原有两条标准都没有在本次重测。E6 的 `-EPOCH` 历史结果仍然不显著；小规模诊断只说明访问路径现在会拒绝不一致的逆映射。没有因此把 G4 写成 GO，也没有降低验收标准。
- 暂停的战役单元仍用 `HOST_BG` 访问 `physical()` 返回的行号。逻辑翻译打开之后，这些访问会被再翻译一次。锤击用的 `HOST_ATK` 保持物理地址。这次没有运行战役单元，重新开跑之前必须先把那些 Host 数据访问改成逻辑行。
- 门闩只拦截被 `phase3BindPublication` 绑到逻辑行的引用。只调用 `hold` / `poison`、不绑定行，读路径仍不会停。战役代码目前没有做这个绑定。
- `Ledger::addCompleted` 仍能在没有命令的情况下写入完成记录。它只留给 Phase 2 单测，不能当作执行证据。
- 换行回调里原有的 `scrubSingleBits` 还在。本次 ECC 故障写在码字存储里，不由那条扰动位图路径清除。它仍然不是这次联合诊断的验收依据。

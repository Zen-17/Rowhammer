# M3 运行时一致性与地址映射诊断

正式战役仍暂停。ECC 纠错能力、故障模型、攻击参数、性能计费和历史结果没有改。`gate_state.json` 的 G4 仍是 PENDING。

诊断命令：`sim --gtest_filter=Phase3M3.Consistency`。结果：1/1 通过，105 ms。记录在 `phase3_exp/results/m3/datapath_consistency/consistency_diagnostic.json`。gtest 通过表示这些测量可以重复，不表示下面五个问题都成立。

## 1. 发布门闩：FAIL

`Gate` 只保存在 Phase 2 对象里。`Bank::read` 在找到行节点后直接拷贝数据，不读取 `HELD` 或 `POISONED`。

测量：ADD 的结果先 `hold`。主机 `readData` 仍读到与计算一致的 128 个 burst，下游 ADD 仍把该结果当成操作数，输出等于两倍。随后 `poison`。主机读和又一次下游 ADD 仍然成功。诊断自己的提交包装能看见 `POISONED`，所以包装层的拒绝是真的；Host 和 PIM 的数据通路都不看这道门。

控制边界：门闩只约束主动调用 `Gate::publish` 的提交包装。它不能阻止主机读取，也不能阻止后续 PIM 消费未恢复的结果。

## 2. 地址映射：PARTIAL

第一次 `forceSwap` 把逻辑行 0 映到物理行 1，并用现有的 `exchangeRows` 搬走该 bank 的行节点。

| 读取 | 新物理行 | 旧行号 |
| --- | --- | --- |
| `NATIVE`，走 `rowEntries` | 与原数据一致 | 不一致 |
| `LOCAL_VISIBLE`，走可达的 ECC 读 | 与原数据一致 | 不一致 |

旧行的码字键还在，内容仍是原数据。`Bank::read` 先按行节点查找，节点已经被搬走，所以这些残留码字不会被这次读使用。

不可纠正的双比特不会被 `exchangeRows` 复制到新行：`ue_ecc_copied=false`，旧键仍在。可纠正的数据会在搬运时用 `writeLegal` 重编码，等于在换行里静默纠正了一次，不产生报告。

因此：行节点和一次新的合法码字会跟着物理行走；残留的旧码字和不可纠正码字没有与行节点保持同一映射。Host 的 `NATIVE` 读、ECC 读和 PIM 读在换行后并不自动使用 `RegionMap::physical()`。调用者如果仍传旧行号，就会错过已经搬走的行节点。

## 3. 代际校验与 G4 第二条：FAIL

`OURS-EPOCH` 让 `logical(0)` 返回 1。同一行的普通读仍返回原来的存储字节。公开报告把 `logical_row` 记成 1、`mapping_epoch` 记成 0。`RecoveryEngine::analyze` 不读取这两个字段，仍按码字物理地址给出 `SOURCE_REPAIRED_ONLY`。

正常 Host 读、PIM 读和 B2 都没有“拿当前 epoch 与记录 epoch 比较”的步骤。过期映射不会被拒绝，也不会变成一次误恢复或逃逸。

G4 第二条要求五项消融各自至少有一个核心指标可测变差。DP-M3 里 −CE、−SCRUB、G1.5、−DET 达到了，−EPOCH 的周期 z 只有 1.02 和 0.81，翻转、逃逸和误恢复都没有达到 `|z|≥1.96`。战役单元比较的是 `physical()` 上的字节，不调用被破坏的 `logical()`。这条诊断说明根因还在访问路径本身：即使比较逻辑行，现有读路径和 B2 也不会因为 epoch 不一致而改变结果。所以 −EPOCH 不可能在原始指标上变差。

## 4. 运行时账本：PARTIAL

联合诊断没有调用 `addCompleted`。它先 `planOp` 声明 ADD 和 RELU 的输入输出，再把 `Phase1FeederMemory` 的完成回调接到 `recordIssue` / `recordComplete`。

测量：两个操作都有来自命令完成事件的 issue 和 complete，RELU 输入版本等于 ADD 的 op id。执行状态和版本可以由命令完成路径写入。

依赖边本身仍来自 `planOp` 的声明。完成事件只提供 tag、行号和周期，不会从指令流里推断谁是谁的生产者。

## 5. 换行之后的选择性恢复：FAIL

两次真实 CE 读把区域升到 L2。逻辑行 0 的物理行是备用行 16368。报告里的逻辑行是 0，物理行是 16368。

随后在备用行注入双比特：

- 主机按 Phase 1 地址读取，产生 UE。
- 紧接着的 PIM ADD/RELU 没有再产生 UE。`joint_pim_ue=false`。
- 用这条主机 UE 和命令账本调用 B2，得到 `SELECTIVE_RECOVERED`，恢复 1 个输入 tile，重放 2 个 tile。
- 发布后的 PIM 结果 128 个 burst 全部与主机 FP16 计算不同。

真实 ECC 事件能进入 B2，账本也能让它选中这次 ADD/RELU。备用行上的主机地址、PIM 命令地址和搬走行节点之后的存储没有合成同一次正确重算。旧行读回也已经不是原来的输入。

## 实现与假设

实际存在的行为：门闩不在读路径上；`exchangeRows` 搬走行节点，可纠正码字另写一份，不可纠正码字不复制；`logical()` 的错位只影响主动调用它的报告字段；完成回调能给已声明的操作填写时间和版本。

诊断没有新增硬件拦截，也没有改 SECDED。换行用的是现有 `forceSwap` 和 CE 钩子里的 `noteCe`，没有跑攻击，也没有改扰动模型参数。

## G4

原始标准两条都要成立。

1. OURS 主战役六格 O6 为 0，且周期低于 FIX。DP-M3 按 3/N=0.010 写成 PASS。Wilson 95% 上界是 0.0126。
2. 五项消融都要在逃逸、误恢复、翻转或开销上可测变差。−EPOCH 没有达到。本诊断确认这不是采样噪声，而是读路径和 B2 都不消费 epoch。

第 2 条不成立，G4 不满足原始验收标准。G4 保持 PENDING，没有写成 GO，也没有进入 M4。

## 尚未解决

- 发布门闩没有接到 Host 或 PIM 的读。
- 换行后的 ECC 键和行节点会分开。不可纠正码字留在旧键上。
- 没有访问期的 epoch 比较。
- 命令回调不能自动生成依赖边。
- L2 备用行上，主机 UE 能触发 B2，但 PIM 重算结果不正确。
- 战役单元和历史汇总没有改。

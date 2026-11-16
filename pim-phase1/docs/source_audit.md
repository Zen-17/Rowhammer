# G1 Source Audit — Fixed Commit 3703d1f19c8f027360cc33a3243eb271e3bb6898

本审计只描述固定提交的源码事实与可验证的调用路径。行号取自
`simulator/src/...` 的固定提交工作副本；所有结论标注为源码审计结论，不冒充
运行时测量。尚未由运行验证的语义明确写为“尚未确认”。

## 1. Host 普通读写与 PIM 内部读写

### 1.1 Host 事务提交到存储

调用链：

```text
PIMKernel / fixture
  -> MultiChannelMemorySystem::addTransaction(...)
       MultiChannelMemorySystem.cpp:459-470
     -> MultiChannelMemorySystem::findChannelNumber(...)
       MultiChannelMemorySystem.cpp:414-440
        -> AddrMapping::addressMapping(...)
     -> MemorySystem::addTransaction(...)
       MemorySystem.cpp:175-221
        -> MemoryController::addTransaction(...)
          MemoryController.cpp:595-608
```

- `MultiChannelMemorySystem::addTransaction` 先根据地址映射选择通道，再把
  `Transaction` 交给该通道的 `MemorySystem`；见
  `MultiChannelMemorySystem.cpp:459-470`。
- `MemorySystem::addTransaction` 在控制器接受时立即提交；控制器拒绝时放入
  `pendingTransactions` 并在后续 `MemorySystem::update` 中每次搬运一个；见
  `MemorySystem.cpp:210-221` 和 `MemorySystem.cpp:241-246`。
- `MemoryController::addTransaction` 在真正进入 `transactionQueue` 时执行
  `parentMemorySystem->numOnTheFlyTransactions++`，并记录 `timeAdded`；见
  `MemoryController.cpp:595-608`。这是“控制器接受”的计数点，不是存储访问点。
- `MemoryController::updateTransactionQueue` 每周期最多把一个事务转换成
  `BusPacket` 并放入 `CommandQueue`；见 `MemoryController.cpp:305-375`，尤其是
  `commandQueue.enqueue(command)` 的 `MemoryController.cpp:351`。
- `CommandQueue::pop` 选择可发出的命令；`updateCommandQueue` 设置
  `outgoingCmdPacket` 和 `cmdCyclesLeft`；见 `CommandQueue.cpp:323-348` 和
  `MemoryController.cpp:169-303`。命令到达 rank 的入口是
  `MemoryController.cpp:463-471` 对 `Rank::receiveFromBus` 的调用。
- `Rank::receiveFromBus` 先检查/更新 bank 状态，再调用 `sendToBank`；见
  `Rank.cpp:98-115`。普通数据读写最终到
  `Rank::readSb`/`Rank::writeSb`，再调用 `Bank::read`/`Bank::write`；见
  `Rank.cpp:316-374` 和 `Bank.cpp:79-132`。

### 1.2 Host 写的完成点

关键事实：写数据总线完成回调发生在实际 `Bank::write` 之前。

- `MemoryController::update` 在写数据包完成时先调用
  `MemorySystem::WriteDataDone`，随后执行
  `numOnTheFlyTransactions--`，最后才调用 `Rank::receiveFromBus`；见
  `MemoryController.cpp:474-490`（回调在 480-485，计数在 486，实际下送在 487）。
- `Rank::receiveFromBus` 进入 `Rank::sendToBank`，普通模式下
  `writeSb` 调用 `banks[...].write(packet)`；见 `Rank.cpp:391-399` 和
  `Rank.cpp:356-374`。

因此：

- `WriteDataDone` 是“数据已到数据总线并即将进入 rank”的完成通知，不是
  bank 存储已完成的通知。
- `hasPendingTransactions()` 在写回调处就可能减到 0；不能把该回调直接当作
  “物理写已完成”。本轮 feeder 的 barrier/结束检查因此额外检查
  `MemorySystem::pendingTransactions` 并等待一个完整 update 周期；具体运行时
  证据见 G1 结果，不在源码审计中先行断言。

### 1.3 Host 读的完成点

- `Rank::sendToBank` 对 READ 先执行 `readSb`，把数据写入 `packet->data`，再把
  packet 放入 `readReturnPacket` 并设置 `readReturnCountdown=RL`；见
  `Rank.cpp:376-390`。
- `Rank::update` 在倒计时到 0 时把读返回包送到控制器；见
  `Rank.cpp:478-518`。
- `MemoryController::receiveFromBus` 把返回包转成 `RETURN_DATA` 事务；见
  `MemoryController.cpp:103-124`。
- `MemoryController::update` 处理 `returnTransaction`，调用
  `returnReadData`，再删除 `pendingReadTransactions`；见
  `MemoryController.cpp:540-577`。读回调发生在 `Bank::read` 已经填充
  `packet->data` 之后，因此读回调可以作为“用户读缓冲区已写入”的观测点；
  但仍不是“事务从所有上游队列消失”的证明。

## 2. PIM 广播命令与真实 bank 对象身份

### 2.1 广播事务的地址字段

- `PIMKernel::addTransactionAll` 对每个 PIM channel/rank 调用
  `pim_addr_mgr_->addrGenSafe(ch_idx, ra_idx, bg_idx, bank_idx, ...)`；见
  `PIMKernel.cpp:88-108`。PIM 逐元素路径传入的 `bank_idx` 是 0 或 1
  （`PIMKernel::computeAddOrMul`，`PIMKernel.cpp:528-541`）。
- `PIMAddrManager::addrGen` 把 `bank` 参数写入 Scheme8 的 bank 位；见
  `KernelAddrGen.cpp:25-55`。因此 `BusPacket::bank` 只是广播命令中的
  even/odd 偏移字段，不是所有真实 bank 的编号。
- `PIMRank::readHab` 遍历所有 PIM block，对 `pb=0..NUM_PIM_BLOCKS-1` 访问
  `rank->banks[pb * 2 + packet->bank]`；见 `PIMRank.cpp:144-162`。实际 bank
  身份是 `pb*2 + packet->bank`。
- `PIMRank::writeHab` 的 PIM-to-bank 路径同理：`packet->bank==0` 写
  `banks[pb*2]`，`packet->bank==1` 写 `banks[pb*2+1]`；见
  `PIMRank.cpp:207-227`。
- `PIMRank::readOpd`/`writeOpd` 的 `EVEN_BANK`/`ODD_BANK` 分支使用
  `rank->banks[pb*2]`/`rank->banks[pb*2+1]`；见 `PIMRank.cpp:243-254` 和
  `PIMRank.cpp:287-303`。代码在 `packet->bank` 奇偶与 CRF 编码不一致时打印
  警告（偶数分支 `PIMRank.cpp:244-245`，奇数分支 `251-252`），但访问仍由
  `pb` 决定。
- `PIMRank::doPIM` 对每个 PIM command 遍历 `pimblock_id=0..NUM_PIM_BLOCKS-1`
  并调用 `doPIMBlock`；见 `PIMRank.cpp:395-406`。`doPIMBlock` 的读写操作数
  都带 `pimblock_id`；见 `PIMRank.cpp:417-488`。

### 2.2 结论与边界

- G1 的 W1 使用的是 `ALL_BANK`/`pimBankType::ALL_BANK`，由
  `PIMKernel::getToggleCond` 映射为 `crf_toggle_cond=0`；见
  `PIMKernel.h:123-137`。该广播会访问所有 PIM block 的真实 bank。
- 不能把 `packet->bank`、`BusPacket::physicalAddress` 或
  `PIMKernel::addTransactionAll` 的 `bank_idx` 直接当作故障/观测位置。实际
  bank 身份必须由 `pb` 和偶/奇偏移共同确定。
- 对于 `parkIn`/`parkOut` 和模式切换控制地址，`PIMKernel.cpp:35-86` 和
  `121-149` 使用保留行；这些是控制访问，不应与普通数据 codeword 混同。
- 本轮没有实现 ECC、故障注入或报告，因此没有把上述 bank 身份用于报告；
  G1 只在源码审计和后续地址检查中记录该关系。

## 3. Scheme8 地址编码/解码关系

### 3.1 上游解码

`AddrMapping::AddrMapping` 计算 transaction size、各字段位宽；见
`AddressMapping.cpp:43-61`。在默认 HBM2 配置下：

```text
JEDEC_DATA_BUS_BITS=64, BL=4
transactionSize = 64/8*4 = 32 bytes
byteOffsetWidth = log2(64/8) = 3
colLowBitWidth = log2(32)-3 = 2
NUM_COLS=128 -> colBitWidth=7, colHighBitWidth=5
NUM_BANKS=16 -> bankBitWidth=4
NUM_BANK_GROUPS=4 -> bankgroupBitWidth=2
NUM_ROWS=16384 -> rowBitWidth=14
```

`Scheme8` 的字段顺序为 `rank:row:col:bg:bank:chan`，其中 `bank` 分为
bank-within-group 和 bankgroup 两段；见 `AddressMapping.cpp:184-194`：

```text
newTransactionChan    = low channel bits
newTransactionBank    = low bank bits | (bankgroup bits << (bankBitWidth-bankgroupBitWidth))
newTransactionColumn  = next colHighBitWidth bits
newTransactionRow     = next rowBitWidth bits
newTransactionRank    = remaining rank bits
```

`PhysicalAddress` 在映射前先右移 `byteOffsetWidth` 与 `colLowBitWidth`，
即 3+2=5 位；见 `AddressMapping.cpp:84-107`。因此 32 B 事务的最低 5 位是
偏移/事务内列低位，之后才是可映射字段。

### 3.2 PIM 地址生成器

`PIMAddrManager` 使用与 Scheme8 相同的字段顺序进行编码；见
`KernelAddrGen.cpp:25-55`：

```text
addr = rank
addr = (addr << row_bits) | row
addr = (addr << col_bits) | col
addr = (addr << bg_bits)  | bankgroup
addr = (addr << bank_bits)| bank_low
addr = (addr << chan_bits)| chan
addr = (addr << offset_bits)
```

其中 `col_bits = log2(NUM_COLS/BL) = 5`，`offset_bits =
log2(BL*JEDEC_DATA_BUS_BITS/8) = 5`，`bank_bits =
log2(NUM_BANKS)-log2(NUM_BANK_GROUPS) = 2`；见 `KernelAddrGen.h:45-68`。
`addrGenSafe` 在 `col >= NUM_COLS/BL` 时执行 `row++` 和
`col -= NUM_COLS/BL`；见 `KernelAddrGen.cpp:57-71`。它只在 `row>=NUM_ROWS`
时打印 `row overflow`，不抛异常、不截断，也不回绕。

### 3.3 G1 编码器假设

G1 新增 `Phase1Address` 使用同一 Scheme8 布局，提供 tuple -> address 与
address -> tuple。它是本研究的测试/地址工具，**不是**声称上游已暴露的
通用 API。它的 round-trip 必须同时与上游 `AddrMapping::addressMapping`
的实际解码结果对照；结果见 `runs/g1/address_mapping/` 与 G1 STATUS。

## 4. CPU tick、DRAM cycle 与 PIMKernel 局部计数

- `MultiChannelMemorySystem::setCPUClockSpeed(0)` 将 clock1/clock2 都设置为
  DRAM 频率，形成 1:1；见 `MultiChannelMemorySystem.cpp:103-108`。
- `MultiChannelMemorySystem::update` 只调用 `clockDomainCrosser.update()`；
  见 `MultiChannelMemorySystem.cpp:381-384`。1:1 情况下，
  `ClockDomainCrosser::update` 直接调用回调一次；见 `ClockDomain.cpp:66-73`。
- 回调是 `MultiChannelMemorySystem::actual_update`；它遍历所有
  `channels[i]->update()`，然后执行 `currentClockCycle++`；见
  `MultiChannelMemorySystem.cpp:386-412`。这是 DRAM cycle 的唯一推进点。
- 每个 `MemorySystem::update` 先更新所有 rank，再搬运 pending transaction，
  再更新 controller，最后对 rank/controller/self 调用 `step()`；见
  `MemorySystem.cpp:229-258`。`SimulatorObject::step` 只做
  `currentClockCycle++`；见 `SimulatorObject.cpp:36-39`。
- `PIMKernel::runPIM` 的循环是：

```text
while (mem_->hasPendingTransactions()) {
    cycle_++;
    mem_->update();
}
```

见 `PIMKernel.cpp:21-28`。因此：

  - `PIMKernel::cycle_` 是“该循环执行的 update 次数”，不是独立于模拟器的
    设备时钟；
  - 在 1:1 配置下它通常与 DRAM cycle 推进次数一致，但它本身不读取
    `MultiChannelMemorySystem::currentClockCycle`，也没有处理 clock ratio；
  - 它不覆盖 feeder 的外部待提交队列，也不覆盖 `MemorySystem` 的
    `pendingTransactions`；以它作为 G1 的直接/feeder 周期比较会低估/截断，
    本轮由 `Phase1Driver` 使用实际 `mem->update()` 次数计数。
- `PIMKernel` 的局部 `cycle_` 从 0 开始，每个 update 前加一；如果 update
  内部某个回调/完成发生在同一 cycle，事件顺序仍由 `mem->update()` 内部顺序
  决定；G1 只记录总 update 次数，不声称更细的逐事件时序。

## 5. 事务阶段与完成语义

| 阶段 | 源码位置 | 说明 |
|---|---|---|
| Fixture 调用 addTransaction | `MemorySystem.cpp:175-221` | 创建/接收 `Transaction`；可能进入 `pendingTransactions` |
| 控制器接受 | `MemoryController.cpp:595-608` | `transactionQueue.push_back`，`numOnTheFlyTransactions++` |
| 控制器转换成命令 | `MemoryController.cpp:305-375` | 每周期最多一个，`CommandQueue::enqueue` |
| 命令队列发出 | `CommandQueue.cpp:323-348` | 选择 READ/WRITE/ACTIVATE/PRECHARGE/REF |
| 总线到达 rank | `MemoryController.cpp:463-471` | `Rank::receiveFromBus` |
| 实际存储访问 | `Rank.cpp:376-399` + `Bank.cpp:79-132` | `readSb/writeSb` -> `Bank::read/write` |
| 读数据返回 | `Rank.cpp:478-518` -> `MemoryController.cpp:103-124` -> `MemoryController.cpp:540-577` | `returnReadData` 回调 |
| 写数据完成通知 | `MemoryController.cpp:474-490` | 回调/计数在 `Rank::receiveFromBus` 之前 |
| PIM 控制 | `PIMRank.cpp:68-114` | `controlPIM` 设置 HAB/HAB_PIM 模式 |
| PIM 执行 | `PIMRank.cpp:323-415` | `doPIM` 遍历 CRF 和 PIM block |

### 5.1 `hasPendingTransactions()` 的实际覆盖

`MultiChannelMemorySystem::hasPendingTransactions()` 仅累加每个 channel 的
`MemorySystem::numOnTheFlyTransactions`；见
`MultiChannelMemorySystem.cpp:579-587`。`numOnTheFlyTransactions` 的增减点：

- 增加：控制器接受事务时（`MemoryController.cpp:599`）。
- 减少：读的 `returnReadData` 回调时（`MemoryController.cpp:132`）。
- 减少：写的数据总线完成回调时（`MemoryController.cpp:486`），早于实际
  `Rank::receiveFromBus`/`Bank::write`（`MemoryController.cpp:487`）。

因此它**覆盖**：已进入控制器、尚未完成读返回或写数据总线发送的已接受事务。
它**不覆盖**：

- `MemorySystem::pendingTransactions` 中尚未被控制器接受的事务
  （`MemorySystem.cpp:210-221`）；
- feeder 的外部待提交描述符；
- 控制器/命令队列中不来自用户事务的刷新命令；
- PIM 的 CRF/GRF/SRF 控制状态；
- 未来 G2 的报告队列、G5 的输出状态、G6 的 pause 定时器；
- 实际写回完成（因为写回调早于 `Bank::write`）。

所以 G1 不能只用 `PIMKernel::runPIM()` 判断结束；G1 的 Phase1 driver 还检查
feeder 待提交/在途计数、`MemorySystem::pendingTransactions`，并在最后等待
一个完整 update 周期。该额外检查是新增测试基础设施，不是上游原有语义。

## 6. Barrier、模式切换与 BurstType 生命周期

### 6.1 Barrier

- `MemorySystem::addBarrier` 在控制器接受时调用
  `MemoryController::addBarrier`，否则把 `BAR` 追加到
  `pendingTransactions.back()->tag`；见 `MemorySystem.cpp:190-208`。
- `MemoryController::addBarrier` 只给 `transactionQueue.back()` 的 tag 追加
  `BAR`；如果 `transactionQueue` 为空则返回 false；见
  `MemoryController.cpp:135-143`。
- `CommandQueue::process_command` 识别 tag 中的 `BAR`，并阻止后续命令越过
  它；见 `CommandQueue.cpp:224`、`239-243`。barrier 不是单独的队列对象，
  而是附着在某个事务上的 tag。
- `PIMKernel::addBarrier` 遍历所有 PIM channel 调用
  `mem_->addBarrier(ch_idx)` 并忽略返回值；见 `PIMKernel.cpp:116-119`。
  因此当 `transactionQueue` 为空时 barrier 可能静默无效；这是源码事实，
  也是 G1 feeder 采用保守 barrier 的原因。

### 6.2 模式切换

- `PIMKernel::changePIMMode` 的 SB->HAB 写入 ABMR 保留行，HAB->HAB_PIM 写
  `PIM_REG_RA` 控制字，HAB_PIM->HAB 写 HAB 控制字，HAB->SB 写 SBMR 保留行；
  见 `PIMKernel.cpp:121-149`。
- `Rank::sendToBank` 对 ACTIVATE 检查 ABMR 行/列并在 4 个 bank 满足后进入
  HAB；见 `Rank.cpp:400-425`。对 PRECHARGE 检查 SBMR 行/列并在 bank 0/1
  满足后回到 SB；见 `Rank.cpp:426-456`。
- `PIMRank::controlPIM` 读取控制字的 `pimOpMode_`、toggle 条件并设置
  `rank->mode_`；见 `PIMRank.cpp:68-114`。
- 模式切换顺序由 `PIMKernel` 的 barrier 分隔；G1 的 feeder 不在 barrier
  之前提交后续描述符，因此保持该顺序。具体控制命令的 bank 字段和保留行
  属于控制路径，不应作为普通数据位置。

### 6.3 BurstType 与 payload 生命周期

- `BurstType` 是 32 B union（16×FP16、32×uint8、8×uint32 等）；见
  `Burst.h:31-60` 和 `Burst.h:343-347`。默认构造清零 16 个 FP16；见
  `Burst.h:33-39`。
- `Transaction` 保存原始 `BurstType* data`，不复制；见
  `Transaction.h:55-66`。`MemoryController::updateCommandQueue` 创建
  `BusPacket` 时也传同一指针；见 `MemoryController.cpp:343-351`。
- 写路径的 payload 必须在下游 `BusPacket` 完成前保持有效：
  `writeDataToSend` 保存指针，直到 `outgoingDataPacket` 发送完成；见
  `MemoryController.cpp:492-525`。
- 读路径的接收缓冲区由调用者提供；`Rank::readSb`/`PIMRank::readHab` 写入
  `packet->data`，随后 `readReturnPacket` 保留 packet 直到返回；见
  `Rank.cpp:376-390`、`Rank.cpp:478-518`。
- PIMKernel 的持久缓冲区：`null_bst_`、`bst_hab_pim_`、`bst_hab_`、
  `crf_bst_[4]` 是成员；见 `PIMKernel.h:112-113`。`operand->bData` 是
  `NumpyBurstType` 的 vector，调用者必须保活；见 `Burst.h:350-357` 和
  `PIMKernel.cpp:325-335`。
- 临时 `BurstType` 不应把地址交给异步事务。G1 feeder 对写描述符复制
  payload 到描述符自身，对读描述符保存调用者提供的稳定接收缓冲区指针；
  在 feeder 生命周期内不提前释放。该所有权策略是新增测试基础设施，不是
  上游 `Transaction` 的默认行为。

## 7. 尚未确认的事项

以下内容没有在源码中取得充分证据，G1 不据此下结论：

1. **写回调之后的物理写可见性**：源码显示写回调早于 `Rank::receiveFromBus`
   /`Bank::write`（`MemoryController.cpp:480-487`），但“回调后下一次用户读
   一定看到新值”需要运行时验证；G1 通过 W0 和 feeder barrier 测试，不把
   源码顺序当成已测量的数据可见性证明。
2. **`Bank::read` 未命中 row 时的数据内容**：`Bank::read` 在找不到 row 时
   不写入 `busPacket->data`；见 `Bank.cpp:79-90`。调用者缓冲区的初值取决于
   构造/复用，不能依赖未初始化读取。G1 的所有被测数据必须先初始化。
3. **barrier 在空 transactionQueue 时的行为**：源码显示
   `MemoryController::addBarrier` 返回 false 且不创建 barrier
   （`MemoryController.cpp:135-143`），但这是否会在所有上游路径造成重排
   尚未做穷举验证。G1 feeder 采用“先等待之前描述符完成，再通过 barrier”
   的保守策略，不声称等价于完整硬件 barrier。
4. **多 channel 并发和暂停**：G1 只做 1-channel W0/W1 和 2-channel 地址
   映射检查；没有验证多 channel 并发、暂停或独立 channel 进度。
5. **ECC/故障/报告/输出门禁**：本轮没有实现，不应从地址检查推断其行为。

## 8. 关键源码位置索引

| 主题 | 文件:行 |
|---|---|
| Host addTransaction | `MultiChannelMemorySystem.cpp:459-470`、`MemorySystem.cpp:175-221` |
| 控制器接受/队列 | `MemoryController.cpp:595-608`、`305-375` |
| 命令发出 | `CommandQueue.cpp:323-348`、`MemoryController.cpp:169-303` |
| 到达 rank | `MemoryController.cpp:463-471`、`Rank.cpp:98-115` |
| 普通读 | `Rank.cpp:316-354`、`Rank.cpp:376-390`、`Bank.cpp:79-90` |
| 普通写 | `Rank.cpp:356-374`、`Rank.cpp:391-399`、`Bank.cpp:92-132` |
| 读返回 | `Rank.cpp:478-518`、`MemoryController.cpp:103-124`、`540-577` |
| 写完成 | `MemoryController.cpp:474-490` |
| PIM 广播读 | `PIMRank.cpp:144-162`、`230-254` |
| PIM 广播写 | `PIMRank.cpp:164-228`、`274-303` |
| PIM 执行 | `PIMRank.cpp:323-415`、`417-488` |
| 地址解码 | `AddressMapping.cpp:43-61`、`74-208`（Scheme8 184-194） |
| PIM 地址编码 | `KernelAddrGen.cpp:25-71`、`KernelAddrGen.h:45-68` |
| 时钟 | `MultiChannelMemorySystem.cpp:103-108`、`381-412`、`ClockDomain.cpp:66-73` |
| 完成计数 | `MultiChannelMemorySystem.cpp:579-587`、`MemoryController.cpp:132`、`486`、`599` |
| barrier | `MemorySystem.cpp:190-208`、`MemoryController.cpp:135-143`、`CommandQueue.cpp:224`、`239-243` |
| 模式切换 | `PIMKernel.cpp:121-149`、`Rank.cpp:400-456`、`PIMRank.cpp:68-114` |
| Burst 生命周期 | `Burst.h:31-60`、`343-347`、`Transaction.h:55-66`、`PIMKernel.h:112-113` |

# Implementation Log

本文件按关卡记录实际执行的命令、退出码、日志、失败原因和处置。日志中的路径相对于 `pim-phase1/`。G0 的逐条命令索引另见 `logs/g0_command_index.md`。

## G0 — 固定源码、构建、原功能测试、环境锁定

执行日期：2026-11-15（UTC）。执行环境：Ubuntu 22.04.5 LTS，x86-64，GCC/G++ 11.4.0，SCons 4.0.1，Python 3.10.12。

### 1. 系统依赖

```bash
sudo apt-get update
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y \
  git build-essential scons libgtest-dev python3 python3-venv python3-pip poppler-utils
```

- 退出码：`0`（`APT_INSTALL_EXIT=0`）
- 日志：`logs/g0_apt_install.log`
- 安装结果：`scons 4.0.1+dfsg-2`、`libgtest-dev 1.11.0-3`、Python 3.10.12。`poppler-utils` 仅用于阅读论文 PDF，不参与仿真。

### 2. 固定上游提交

第一次尝试完整 clone：

```bash
git clone https://github.com/SAITPublic/PIMSimulator.git simulator
```

- 结果：超过 180 s，外层命令以 SIGTERM/143 终止；终端输出为 `fetch-pack: unexpected disconnect while reading sideband packet`，目录未完成并已删除。该次失败输出未单独落盘；`logs/g0_clone.log` 对应下面的成功重试，不是这一次。
- 处置：不切换到 fork，不改变固定提交；改用浅克隆后检查同一提交。

实际成功命令：

```bash
git clone --depth 1 --branch dev https://github.com/SAITPublic/PIMSimulator.git simulator
git -C simulator checkout --detach 3703d1f19c8f027360cc33a3243eb271e3bb6898
git -C simulator rev-parse HEAD
```

- 退出码：clone `0`、checkout `0`、rev-parse `0`
- 输出 HEAD：`3703d1f19c8f027360cc33a3243eb271e3bb6898`
- tree：`7ac34faf86579aca00f5b357c61b5550d1dff7e7`
- 日志：`logs/g0_clone.log`
- 远端 `refs/heads/dev` 校验（2026-11-15T12:27:17Z）：

```bash
git ls-remote https://github.com/SAITPublic/PIMSimulator.git refs/heads/dev
# 3703d1f19c8f027360cc33a3243eb271e3bb6898	refs/heads/dev
```

- 退出码：`0`；日志：`logs/g0_ref_check.log`
- 克隆为 shallow clone，但固定提交和完整 tree 哈希已记录；这不是对上游完整历史的声明。

### 3. Python 环境

```bash
python3 -m venv .venv
.venv/bin/python -m pip install --upgrade pip
.venv/bin/python -m pip install numpy pandas matplotlib pyyaml jsonschema pytest
.venv/bin/python -m pip freeze > environment/requirements.lock.txt
.venv/bin/python -m pip check
```

- 退出码：全部 `0`；`pip check` 输出 `No broken requirements found.`
- 日志：`logs/g0_python_env.log`、`logs/g0_bootstrap_check.stdout.log`
- 关键版本：numpy 2.2.6、pandas 2.3.3、matplotlib 3.10.9、PyYAML 6.0.3、jsonschema 4.26.0、pytest 9.2.0。
- `requirements.lock.txt` SHA256：`cc8a039b8a554f58d43775adaab01ff5cba5a20543d374360aadbfe43e0e7bf7`。

### 3.5 构建入口与测试调用的源码检查（G0 范围）

在运行构建和测试前实际读取了以下入口，没有按文件名猜 API：

- `simulator/Sconstruct`：确认构建收集 `src/*.cpp` 与 `src/tests/*.cpp`，`NO_STORAGE` 仅在非零时追加 `-DNO_STORAGE`，默认并行度为 32；因此 `NO_STORAGE=0` 表示启用存储。
- `simulator/src/tests/MainTest.cpp`：确认无参数时设置过滤 `-*bw*.*`；显式 `--gtest_filter` 会覆盖该默认值；测试从 `simulator/` 工作目录运行以找到 `data/` 和 `system_hbm*.ini`。
- `simulator/src/Rank.cpp`、`simulator/src/PIMRank.cpp`、`simulator/src/Transaction.cpp`：确认数据存储路径受 `#ifndef NO_STORAGE` 控制，构建未定义该宏时路径启用。
- `simulator/src/tests/PIMBenchTestCases.h` 与 `.cpp`：定位 `expectPIMBench` 的 `> 2.0` 阈值和 `PIMBenchFixture.add` 的输入规模，仅用于诊断，不修改测试。
- `simulator/ini/HBM2_samsung_2M_16B_x64.ini`、`system_hbm_1ch.ini` 等：只计算哈希并在 `upstream.lock.json` 中锁定，没有修改几何或时序。

G0 没有修改上述文件；完整的调用链、bank 身份和时钟语义审计属于 G1，不在本关提前完成。

### 4. 构建

首次构建（未修改 `Sconstruct`）：

```bash
cd simulator
scons NO_STORAGE=0 NO_LIBRARY=1
```

- 退出码：`0`（`SCONS_EXIT=0`）
- 日志：`logs/g0_build.log`
- 没有触发内存不足：本机 112 个逻辑 CPU、约 503 GiB 内存，上游默认 32 jobs 可完成。因此没有修改 `Sconstruct` 的并行度，也没有 build-only compatibility patch。

随后用可复现脚本做一次干净重建，作为 canonical G0 构建：

```bash
./scripts/build.sh --clean
```

- 退出码：`0`（`EXIT_CODE=0`）
- 日志：`logs/build_20261115T122721Z.log`
- 脚本实际执行 `scons -c NO_STORAGE=0 NO_LIBRARY=1` 后执行 `scons NO_STORAGE=0 NO_LIBRARY=1`。
- 两次构建的二进制 SHA256 相同：`f596d0a0a8fe5ae0a7859f599aada5c924c8495a876bd7ce5a8a8674b4b12a5e`（13,878,728 bytes）。
- 存储语义：`NO_STORAGE=0`；`Sconstruct` 只在 `int(no_storage) != 0` 时追加 `-DNO_STORAGE`。canonical 构建日志中该宏出现 0 次，故 `src/Rank.cpp`、`src/PIMRank.cpp`、`src/Transaction.cpp` 中的 `#ifndef NO_STORAGE` 数据路径处于启用状态。
- 证据：`environment/build_flags.txt`、`environment/binary.sha256`、`environment/binary.json`。

### 5. 上游测试

#### 5.1 规定的 `PIMKernelFixture.*`

```bash
cd simulator
./sim --gtest_list_tests
./sim --gtest_filter='PIMKernelFixture.*' \
  --gtest_output=xml:../runs/baseline/upstream-tests.xml
```

- `--gtest_list_tests` 退出码 `0`；日志 `logs/g0_gtest_list.log`。列出 11 个测试：5 个 `PIMKernelFixture`、2 个 `MemBandwidthFixture`、4 个 `PIMBenchFixture`。
- `PIMKernelFixture.*` 退出码 `0`；5/5 通过，`tests=5 failures=0 errors=0`。
- 首次人工运行的 XML 和 stdout/stderr 保存在 `runs/baseline/manual_first_run/`，原始日志为 `logs/g0_upstream_tests.*`。
- 通过 `scripts/run_baseline.sh` 再次执行，生成 canonical 产物：
  - XML：`runs/baseline/upstream-tests.xml`，SHA256 `e7be608f8df6d78204b32e3de8bf7cd71877bce25218b2f32069633747dd40a2`
  - summary：`runs/baseline/upstream-tests.summary.json`
  - exit code：`runs/baseline/upstream-tests.exit_code`，内容 `0`
  - 日志：`logs/baseline_20261115T122735Z.log`
- 测试通过，不是用新 fixture 替代旧测试；G0 尚未添加任何新 fixture。

#### 5.2 额外全量上游测试（非门禁）

```bash
./sim --gtest_output=xml:../runs/baseline/upstream-all-tests.xml
```

- 退出码：`1`；11 个测试中 10 通过、1 失败。
- XML：`runs/baseline/upstream-all-tests.xml`，`tests=11 failures=1 errors=0`；stdout/stderr：`logs/g0_upstream_all.stdout.log`、`logs/g0_upstream_all.stderr.log`；退出码记录：`logs/g0_upstream_all.exit.log`。
- 失败项：`PIMBenchFixture.add`。
- 实际数值：

```text
non_pim_cycle_ = 6651
pim_cycle_     = 3349
speed-up       = 1.9859659671783447
assertion      = speed-up > 2.0  -> false
```

- 单独运行该测试同样失败，退出码 `1`，cycle 数相同；日志 `logs/g0_pimbench_add_isolated.log`。说明不是测试顺序或状态污染。
- 在 `/tmp/opencode/pim-nostorage-diag` 的临时副本中构建 `NO_STORAGE=1` 后，同一测试仍是 6651/3349、`1.985965967`、退出码 `1`；日志 `logs/g0_diag_nostorage1.log`。故该失败与 `NO_STORAGE=0` 无关，且不是本项目的数据路径问题。
- 处置：未修改测试、阈值或预期结果；未删除失败样本；未把全量测试写成 11/11 通过。G0 的规定验收命令是 §11.1 的 `PIMKernelFixture.*`，该命令全部通过。全量测试的结果保留为非门禁的上游基线观察，供后续复核。

### 6. 环境锁定与哈希

实际命令：

```bash
# 固定提交 tree 中的所有 tracked 文件
cd simulator && git ls-files -z | sort -z | xargs -0 sha256sum \
  > ../environment/upstream-files.sha256

# 环境
.venv/bin/python scripts/capture_environment.py

# upstream.lock.json
.venv/bin/python scripts/lock_upstream.py

# 校验
./scripts/bootstrap.sh --check-only

# 独立 G0 验收
.venv/bin/python scripts/validate_g0.py
```

- 全部退出码 `0`；独立验收 18/18 PASS。
- 日志：`logs/g0_source_manifest.log`、`logs/g0_environment.log`、`logs/g0_upstream_lock.log`、`logs/bootstrap_20261115T122802Z.log`、`logs/g0_validation.log`；验收结果：`environment/g0_validation.json`。
- tracked 文件数：94；清单 SHA256：`e6f7c9c4fb84eb9c991e406fd88e3d94df106c55511ec831c56c8f6b79c68dee`。
- `environment_hash`：`9bd07d55353ada1ef0f6a2f96a41e816caaf806fc048f19f0477e557829af90e`。
- `upstream.lock.json` SHA256：`a0c06e96bb2edfba1cf1a8306c12cdcf60fa2def7f583fca60f017f8a77f4691`。
- 上游配置哈希：
  - `ini/HBM2_samsung_2M_16B_x64.ini`：`a770f3d6e1347eef0396d438859e84b7b32001dab5513e662f9bb24e4e6a3ecd`
  - `system_hbm.ini`：`a22b185f32341a1082607ce89ad67c3b38f90f7e5ce26ba6ec12f972236e7170`
  - `system_hbm_1ch.ini`：`06554c0b1fe72c491ee617ccc05147d052b0acd2a66d4cba669161970c0720e2`
  - `system_hbm_64ch.ini`：`d969b82501925fb5c26297feefb22a1aaa5986b87edefeee35ef8068aaff79be`
- `git status --porcelain` 为空；上游源码、配置和测试未被修改。

### 7. `source.patch` 的特殊情况

G0 没有修改上游源码，因此没有新增源码补丁。曾创建空的 `environment/source.patch`，但 `git -C simulator apply --check ../environment/source.patch` 返回 `128`（`error: unrecognized input`）；空补丁不是可应用补丁。为避免伪造空补丁或添加无意义文件，已删除该空文件，并在本日志中记录：`source.patch` 从 G1 产生实际源码变更时再生成，届时必须通过 `git apply --check` 和干净目录重放验证。

### 8. 实际改动的文件

上游 `simulator/` 没有改动。`pim-phase1/` 新增：

```text
README.md
HANDOFF.md
scripts/bootstrap.sh
scripts/build.sh
scripts/run_baseline.sh
scripts/capture_environment.py
scripts/lock_upstream.py
scripts/validate_g0.py
environment/upstream.lock.json
environment/environment.txt
environment/environment.json
environment/requirements.lock.txt
environment/upstream-files.sha256
environment/build_flags.txt
environment/binary.sha256
environment/binary.json
environment/g0_validation.json
runs/baseline/upstream-tests.xml
runs/baseline/upstream-tests.stdout.log
runs/baseline/upstream-tests.stderr.log
runs/baseline/upstream-tests.exit_code
runs/baseline/upstream-tests.summary.json
runs/baseline/upstream-all-tests.xml
runs/baseline/gtest-list.log
runs/baseline/binary.sha256
runs/baseline/binary.json
runs/baseline/manual_first_run/*（首次人工运行副本）
logs/*（G0 实际命令与诊断日志）
docs/STATUS.md
docs/implementation_log.md
```

### 9. 通过/失败检查汇总

| 检查 | 结果 |
|---|---|
| 固定提交 `HEAD` 校验 | PASS |
| `refs/heads/dev` 与固定提交一致 | PASS |
| 上游 worktree 干净 | PASS |
| `scons NO_STORAGE=0 NO_LIBRARY=1` | PASS |
| 数据存储启用（无 `-DNO_STORAGE`） | PASS |
| `PIMKernelFixture.*` 5/5 | PASS |
| 额外全量上游测试 11/11 | FAIL：`PIMBenchFixture.add` 性能阈值 1.985966 < 2.0（非功能性、非门禁） |
| 环境/依赖锁定 | PASS |
| `bootstrap.sh --check-only` | PASS |
| `source.patch` 可应用性 | N/A：G0 无源码变更；空补丁验证失败后未伪造 |

### 10. 下一关唯一任务

**G1：审计路径、时钟、布局与完成语义，建立 Phase 1 fixture 与 feeder，在无故障条件下完成实际 bank 映射 round-trip 与独立 FP16 参考一致。**

只有用户明确指示后才开始 G1。G0 的构建、测试、环境与日志产物必须保留。

## G1 — 源码审计、Phase1 fixture、地址检查、有界 feeder、无故障正确性

执行日期：2026-11-15（UTC）。G0 的 `validate_g0.py` 在开始时重新执行，18/18 PASS；G0 的 `runs/baseline/` 未被覆盖。G1 完成后，由于源码已按 G1 修改，`validate_g0.py` 的 live worktree 检查不再适用；改用 `scripts/validate_g0_artifacts.py` 对只读 G0 二进制/XML/配置证据做 11/11 再校验，结果 PASS（`environment/g0_artifact_validation.json`、`logs/g1_g0_artifact_validation.log`）。

### 1. 本关范围与顺序

严格按用户给出的 A–E 顺序执行：

1. 先完成源码审计，写入 `docs/source_audit.md`，记录固定提交中的文件、函数和行号；未确认项单列。
2. 再实现 `Phase1Address`、`Phase1Feeder`、`Phase1Driver` 和 `Phase1Fixture`。
3. 做 1/2-channel 地址映射检查。
4. 实现有界 feeder（每 channel 8、每 cycle 1、拒绝重试、payload 稳定所有权）。
5. 运行 W0/W1 无故障 case，保存 direct/feeder/Python 参考输出和周期。
6. 重新执行原 `PIMKernelFixture.*`，保留已知 `PIMBenchFixture.add` 失败。
7. 生成 `environment/source.patch`，在固定提交的干净副本应用、构建并执行 G1 smoke。

本关没有实现 ECC、故障注入、报告、输出门禁、暂停或恢复逻辑，也没有进入 G2。

### 2. 源码审计结论（详见 docs/source_audit.md）

- Host 路径：`MultiChannelMemorySystem::addTransaction` -> `MemorySystem::addTransaction` -> `MemoryController::addTransaction` -> `transactionQueue` -> `CommandQueue` -> `Rank::receiveFromBus` -> `Rank::readSb/writeSb` -> `Bank::read/write`。
- PIM 广播路径：`PIMKernel::addTransactionAll` 的 `bank` 字段是 even/odd 偏移；`PIMRank::readHab/writeHab/readOpd/writeOpd` 用 `pb*2 + packet->bank` 访问实际 bank。不能把 `packet->bank` 当作唯一真实 bank。
- Scheme8：字段顺序为 `rank:row:col:bg:bank_within_group:chan:offset`；默认 32 B 事务的 offset+colLow 共 5 位；`PIMAddrManager::addrGen` 的编码与 `AddrMapping::addressMapping` 的 Scheme8 解码对应。
- 时钟：`MultiChannelMemorySystem::actual_update` 是 DRAM cycle 推进点；`setCPUClockSpeed(0)` 建立 1:1。`PIMKernel::cycle_` 只是其 `runPIM` 循环的 update 计数，不是独立设备时钟。
- 写完成：`WriteDataDone` 回调和 `numOnTheFlyTransactions--` 发生在 `Rank::receiveFromBus`/`Bank::write` 之前（`MemoryController.cpp:480-487`），不能把写回调直接当作物理写完成。
- `hasPendingTransactions()` 只累计每 channel 的 `numOnTheFlyTransactions`；不覆盖 `MemorySystem::pendingTransactions`、feeder 外部队列、refresh/control 定时器、PIM CRF/GRF/SRF 状态或未来报告/输出状态。
- barrier 是附加在事务 tag 上的字符串（`MemoryController::addBarrier`），空 `transactionQueue` 时可能无效；`PIMKernel::addBarrier` 忽略返回值。G1 feeder 因此采用保守的阶段性静止 barrier，而不声称等价于完整硬件 barrier。
- `BurstType` 为 32 B；`Transaction`/`BusPacket` 只保存原始 `BurstType*`，不复制 payload。feeder 对写描述符复制 payload，对读描述符保存调用者稳定接收缓冲区；描述符在运行结束前不提前释放。

### 3. 新增/修改的源码

新增：

```text
simulator/src/Phase1Address.{h,cpp}
simulator/src/Phase1Feeder.{h,cpp}
simulator/src/Phase1Driver.{h,cpp}
simulator/src/tests/Phase1TestCases.cpp
```

上游最小修改：

```text
simulator/src/MultiChannelMemorySystem.h
  - addBarrier(int) 改为 virtual（供 feeder 拦截）
  - hasPendingTransactions() 改为 const（供 const 查询）
simulator/src/MultiChannelMemorySystem.cpp
  - hasPendingTransactions() 定义改为 const
```

没有修改上游测试、阈值、原始 fixture 或故障/ECC 路径。

### 4. 新 fixture 与配置

- `configs/phase1_system_1ch.ini`：NUM_CHANS=1，DEBUG_CMD_TRACE=false；SHA256 `0f1dfa16924f74e924a36f0a86ccaf502a3922ce35358741db23582e03b68f9c`。
- `configs/phase1_system_2ch.ini`：NUM_CHANS=2，其余设备几何不变；SHA256 `841c94cab634c8a2f3bd4e247fae7f0e35042d1d885e52c48e9b8fc25c36c2f0`。
- 1-channel 容量 256 MB = 1 rank；2-channel 容量 512 MB = 每 channel 1 rank。均保持上游 16 banks、8 PIM blocks、FP16、Scheme8。
- 元素/burst/tile：
  - `dim_bursts = n_tiles * NUM_BANKS * channels * ranks * 8`
  - `elements = dim_bursts * 16`
  - 1ch：n_tiles=1 -> 128 bursts / 2048 elements；n_tiles=8 -> 1024 bursts / 16384 elements。
  - `Phase1Driver`/fixture 断言 `dim_bursts % (16*channels*ranks*8) == 0`，不存在整数截断。
  - n_tiles=8 的 1024 bursts 在每 bank 32 个 burst-column 后由 `addrGenSafe` 进位到下一 row；实际范围是 A 从 row0 起跨 row0-1，X 从 row128 起跨 row128-129，B 从 row256 起跨 row256-257。这里记录的是实际映射，不把“起始 row”误写成唯一的 row。

### 5. 地址映射检查

实际运行：

```bash
PHASE1_CONFIG=.../runs/g1/address_mapping/1ch/params.ini \
PHASE1_OUT=.../runs/g1/address_mapping/1ch \
./sim --gtest_filter=Phase1Fixture.RunCase
# 1ch exit 0
# 2ch 同命令 exit 0
```

结果：

- 1-channel：48 个 tuple，48 个唯一地址，round-trip 48/48，上游 decoder 48/48；address_bits=28。
- 2-channel：96 个 tuple，96 个唯一地址，round-trip 96/96，上游 decoder 96/96；address_bits=29。
- 覆盖 bank 0、3、15；row 0、1、8192、16383；burst column 0、1、16、31。
- CSV 每行保留 tuple、encoded address、decoded tuple、round-trip/上游 decoder/唯一性标志；不是只打印一句 PASS。
- 证据：`runs/g1/address_mapping/1ch/address_mapping.{csv,json}`、`2ch/...`。

### 6. 有界 feeder

实现：

- `Phase1FeederMemory` 继承 `MultiChannelMemorySystem`，拦截 `addTransaction`/`addBarrier`，把描述符放入外部 `pending_` 队列。
- `tick()` 每 cycle 最多提交一个；每 channel `outstanding_` 上限 8；`willAcceptTransaction(address)` 为 false 时保留描述符并增加 `controller_rejections_`，下一 cycle 重试。
- `accepted_` 保留写 payload 的稳定所有权；读描述符保存调用者提供的稳定接收缓冲区。
- 外部 `pending_` 与内部已接受/在途 `outstanding_` 分开计数；`isIdle()` 还检查 `MemorySystem::pendingTransactions`、controller `transactionQueue`、rank 的返回/数据总线状态。
- barrier 采用保守策略：等待前一阶段 `outstanding==0`、`hasPendingTransactions==0`、pending/controller 队列和 rank 数据路径静止后，再调用上游 barrier 并放行下一阶段。该策略导致 feeder 周期高于 direct，属于显式模型选择。
- 专项测试 `Phase1Contract.FeederRetainsOnControllerRejection` 通过 64 个直接写入填满上游 64 深事务队列，再让 feeder 提交读；第一次 tick 观测到 controller rejection，描述符保留，随后更新并成功完成。日志：`logs/g1_feeder_rejection.log`。

W1 中观测到：

| case | max external pending | max accepted outstanding | controller rejections |
|---|---:|---:|---:|
| n_tiles=1 | 487 | 8 | 0 |
| n_tiles=8 | 3553 | 8 | 0 |

W1 未触发拒绝是因为 8 < 上游 64 深事务队列；拒绝/重试路径由专项测试强制验证，没有伪造 W1 的 rejection 数值。

### 7. W0/W1 无故障结果

命令由 `scripts/run_g1.py` 生成独立 params 并逐 case 启动进程：

```bash
PHASE1_CONFIG=<case>/params.ini PHASE1_OUT=<case> \
  ./sim --gtest_filter=Phase1Fixture.RunCase
```

`run_g1.py` 退出码 `0`；`g1_validate.py` 最终 10/10 checks PASS。日志：`logs/g1_run.log`。

- W0 direct：54 updates；feeder：55 updates；输出逐 bit 相同。
- W1 n_tiles=1：direct 1547 updates；feeder 2565 updates。
- W1 n_tiles=8：direct 9990 updates；feeder 18030 updates。
- seed 0/1/2 的 cycle 数相同（输入不同，访问序列相同）；每个 direct/feeder 输出逐 bit 相同。
- 每个 W1 输出与独立 Python `numpy.float16` 参考逐 bit 相同；`input_a.bin`/`input_x.bin` 也与 Python 输入逐 bit 相同。
- 每个 case 保存 `params.ini`、`metadata.json`、`stdout.log`、`stderr.log`、`exit_code`、`timing.json`、`output.bin`、`input_a.bin`、`input_x.bin`、`reference.bin`、`comparison.json`。
- 汇总：`results/g1/summary.csv`、`w0_summary.csv`、`w1_summary.csv`、`validation.json`。
- **周期差异必须保留**：feeder 的 `+1018`（n_tiles=1）和 `+8040`（n_tiles=8）来自保守 barrier/提交节奏，不是硬件测量，也不是与原始 64-channel fixture 的相等性声明。

### 8. 原始测试回归与已知失败

```bash
./sim --gtest_filter='PIMKernelFixture.*' --gtest_output=xml:../runs/g1/upstream-tests.xml
# exit 0，5/5 PASS，tests=5 failures=0 errors=0

./sim --gtest_filter='PIMBenchFixture.add'
# exit 1，speed-up 1.98597 < 2.0
```

- `runs/g1/upstream-tests.xml`、`runs/g1/pimbench_add.log`。
- `PIMBenchFixture.add` 的已知基线失败保留，没有修改阈值、删除测试或写成全量通过。G0 的 `runs/baseline/` 未被覆盖。

### 9. 补丁与干净副本重放

生成 `environment/source.patch`（包含 2 个上游修改和 7 个新增文件）：

```bash
git -C simulator add -N src/Phase1Address.* ... src/tests/Phase1TestCases.cpp
git -C simulator diff --binary > environment/source.patch
git -C simulator reset -q
```

- 补丁 SHA256：`629a36c016b3a5df2fa00dcadceec6b1bb00e29b1b4564342f7a16c5f96e19db`
- 干净副本：`git worktree add --detach /tmp/opencode/pim-g1-clean 3703d1f...`。
- `git apply --check` 与 `git apply`：均 exit 0；日志 `logs/g1_clean_apply.log`、`logs/g1_patch.log`。
- 干净构建：`scons NO_STORAGE=0 NO_LIBRARY=1` exit 0；日志 `logs/g1_clean_build.log`；干净二进制 SHA256 `1eca462c...`（由于构建路径不同，不要求与工作区二进制相同）。
- 干净副本 smoke：
  - 地址 1ch/2ch exit 0；
  - W1 nt=1 seed=0 direct/feeder exit 0；
  - 原始 `PIMKernelFixture.*` 5/5 exit 0；
  - feeder rejection 专项 exit 0；
  - 独立验证 9/9 checks PASS。
  - 证据：`runs/g1/clean_replay/`、`logs/g1_clean_smoke.log`、`logs/g1_clean_validation.log`。

### 10. G1 二进制与环境

- G1 工作区二进制：`be9f6a57812b41b490d166acbd9f498c31282f9c9e4624711114713c50e2ad26`，`environment/g1_binary.sha256`。
- G0 二进制副本：`runs/baseline/sim-g0`，`f596d0a0a8fe5ae0a7859f599aada5c924c8495a876bd7ce5a8a8674b4b12a5e`；没有覆盖 G0 记录。
- G1 lock：`environment/g1.lock.json`（上游 commit、补丁、配置、源码、测试/汇总哈希）。
- 环境 hash 仍为 G0 的 `9bd07d55353ada1ef0f6a2f96a41e816caaf806fc048f19f0477e557829af90e`；编译器和 Python 环境未改变。

### 11. 实现过程中的失败与修正

1. 首次 G1 编译 exit 2：`Phase1Feeder.cpp` 误用了 `MultiChannelMemorySystem::pendingTransactions`（实际在 `MemorySystem`），且 const 方法调用非 const `hasPendingTransactions()`。修正为逐 channel 查询 `MemorySystem::pendingTransactions`，并仅把 `hasPendingTransactions()` 改为 const。失败日志被后续成功构建覆盖；最终成功日志为 `logs/g1_build.log`。
2. W0 初次运行被 `loadPhase1Config` 错误地要求 n_tiles=1/8；W0 无 tile。修正为仅对 W1 检查。失败 stdout 被后续成功运行覆盖；最终 W0 日志在 `runs/g1/w0_1ch_*`。
3. 地址 CSV 最初把配置的 channel 数写在 `channels` 列，丢失 tuple channel；已改为 `channel` 列并重跑 1/2ch。修正后的 CSV 与 Python 独立编码检查一致。
4. 验证脚本的一个 run 路径拼接错误（用 case_id 当目录名）已修正；最终 `logs/g1_run.log` 中 10/10 PASS。
5. 没有删除测试、放宽阈值、伪造 failure，也没有把 `PIMBenchFixture.add` 写成通过。

### 12. G1 交付文件

```text
docs/source_audit.md
simulator/src/Phase1Address.{h,cpp}
simulator/src/Phase1Feeder.{h,cpp}
simulator/src/Phase1Driver.{h,cpp}
simulator/src/tests/Phase1TestCases.cpp
configs/phase1_system_1ch.ini
configs/phase1_system_2ch.ini
environment/source.patch
environment/g1.lock.json
environment/g1_binary.sha256
runs/g1/address_mapping/...
runs/g1/w0_1ch_*...
runs/g1/w1_1ch_*...
runs/g1/clean_replay/...
results/g1/summary.csv
results/g1/w0_summary.csv
results/g1/w1_summary.csv
results/g1/validation.json
logs/g1_*.log
```

### 13. 下一关唯一任务

**G2：建立事件记录与数据协议；不改变执行行为。要求 EXT_OFF 同 feeder 的周期/输出一致，并通过 schema、事件序号和 cycle 单调性检查。**

只有用户明确指示后才进入 G2。G0/G1 的基线、构建、测试、环境、运行、补丁和 clean replay 产物必须保留。

## G2 — 事件记录与数据协议

执行日期：2026-11-15（UTC）。G1 结束时重新执行 `g1_validate.py` PASS；G0 只读产物 `validate_g0_artifacts.py` PASS。本关没有实现 ECC、故障注入、报告、输出门禁、暂停或恢复。

### 1. 范围与设计

G2 只增加被动事件记录和 run-directory 数据协议：

- 新增 `Phase1Trace`：写 `events.csv`，不读取或修改模型状态。
- `Phase1Feeder` 在 descriptor enqueue、controller reject/accept、barrier retire、read/write completion 处调用 trace。
- `Phase1Fixture` 在 W1 前后写 `RUN_START`/`RUN_END`。
- `ext_off` 仍创建 header-only `events.csv`；`ext_on` 写事件。二者使用同一 feeder 和存储路径。
- `reports.csv` 严格 5 列、G2 仅表头；`truth.csv` 使用未来兼容 header、G2 仅表头。
- 新增 `configs/schema.json`、`scripts/validate_run.py`、`scripts/run_g2.py`、`tests/test_schema.py`、`docs/g2_protocol.md`。

没有把 trace 传给 runtime；G2 没有 runtime/report 对象。

### 2. 事件协议

- header 严格为 `event_seq,cycle,phase,event_type,kernel_id,version_id,access_id,channel,rank,actual_bank,row,column,access_path,bytes,status`。
- `event_seq` 从 1 连续递增；`cycle` 使用 `MultiChannelMemorySystem::currentClockCycle`，不倒退。
- phase 顺序：`ENQUEUE < START < FEEDER < MEMORY < COMPLETE < FINALIZE`。
- 事件类型：`DESCRIPTOR_ENQUEUE`、`BARRIER_ENQUEUE`、`CONTROLLER_ACCEPT`、`CONTROLLER_REJECT`、`BARRIER_RETIRED`、`READ_COMPLETE`、`WRITE_DATA_DONE`、`RUN_START`、`RUN_END`。
- 普通 descriptor 写入 address-mapped channel/rank/bank/row/column。
- PIM 广播 tag（`BANK_TO_GRF_`、`ADD`、`GRF_TO_BANK` 等）的 `actual_bank` 留空，`access_path` 加 `:PIM_BROADCAST`；PIM 控制（`PROGRAM_CRF`、`PIM`）加 `:PIM_CONTROL` 且 bank/row/column 留空。这样不会把 packet bank 字段或保留控制行冒充为实际数据 bank。
- `WRITE_DATA_DONE` 对应上游写数据总线回调，不宣称物理 `Bank::write` 已完成；这是源码审计中的已知边界。
- `kernel_id=1`、`version_id=1` 是单 kernel G2 的协议占位值，不是 Phase1 输出版本状态。

### 3. EXT_OFF / EXT_ON 对照

运行：

```bash
.venv/bin/python scripts/run_g2.py
```

结果：

| case | ext_off cycles | ext_on cycles | G1 feeder cycles | 输出 |
|---|---:|---:|---:|---|
| W1 nt=1 seed=0 | 2565 | 2565 | 2565 | 三者逐 bit 相同 |
| W1 nt=8 seed=0 | 18030 | 18030 | 18030 | 三者逐 bit 相同 |

- `results/g2/validation.json` 8/8 checks PASS。
- ext_on 事件：nt1 1449 条，nt8 10605 条；ext_off 两个都只有 header。
- `scripts/validate_run.py` 对每个 run 检查 config schema、events schema、event_seq、cycle、phase、位置范围、reports/truth 空表、output bit-exact、inputs bit-exact、metadata 字段/hash、timing 队列终态。
- `tests/test_schema.py` 10 passed（有效配置 + 9 个无效配置）。
- 负向控制：复制一个 ext_on run，将第二个 event_seq 改成 1，validator 返回 exit 1，`event_seq_strict` FAIL；日志 `logs/g2_negative_sequence.log`。

### 4. 运行目录协议

每个 G2 run 保存：

```text
params.ini, config.json, metadata.json, stdout.log, stderr.log, exit_code,
events.csv, reports.csv, truth.csv, output.bin, reference.bin,
timing.json, metrics.json, validation.json,
validation_stdout.log, validation_stderr.log, validation_exit_code
```

- `metadata.json` 含 schema_version、run_id（16 位截断 + 完整值）、experiment、seed、upstream_commit、patch_sha256、binary_sha256、config_sha256、reference_id、environment_hash、start_time_utc、wall_seconds、exit_code、run_status。
- `metrics.json` 含必需字段；报告/恢复/未建模字段为 `null`，不伪造 0；no-fault 的 `output_changed=false`、`changed_elements=0`、`invalid_outputs=0`。
- `validation.json` 每条 assertion 有 name/pass/expected/actual/event_seq。
- `reports.csv` 和 `truth.csv` 在 G2 只有表头；没有伪造报告或 truth 行。

### 5. 构建、测试、补丁与干净重放

- 构建：`scons NO_STORAGE=0 NO_LIBRARY=1` exit 0；日志 `logs/g2_build.log`。
- G2 工作区二进制：`f54c8abded3c9a373e79a7798f6c1234b5bd1c006ddd6907c4127d053499a8d8`（`environment/g2_binary.sha256`）。
- G1 二进制 hash 仍记录为 `be9f6a...`；G0 副本仍为 `f596d0...`；没有把 G2 二进制写成 G0/G1。
- 累计补丁：`environment/source.patch` SHA256 `2b7b65bab048e44160c9ef04ef8cd59e68ea2acc6a7cd950ec35da23d40dc0ec`；G1 snapshot 保留为 `environment/source_g1.patch`，SHA256 `629a36...`。
- 干净副本 `/tmp/opencode/pim-g2-clean`：`git worktree add --detach` 固定提交；`git apply --check` / `git apply` exit 0；`scons` exit 0；clean binary `0d8723...`。
- 干净 G2 smoke：nt1/nt8 ext_off/ext_on 全部 exit 0，输出/周期与主工作区对应项相同；干净 `PIMKernelFixture.*` 5/5 PASS；`runs/g2/clean_replay/validation.json` 10/10 PASS。
- 干净重放日志：`logs/g2_clean_worktree.log`、`g2_clean_apply.log`、`g2_clean_build.log`、`g2_clean_smoke.log`；干净 PIMKernel XML：`runs/g2/clean_replay/upstream-tests.xml`。

### 6. 原始测试与已知失败

```bash
./sim --gtest_filter='PIMKernelFixture.*' --gtest_output=xml:../runs/g2/upstream-tests.xml
# exit 0，5/5 PASS
./sim --gtest_filter='PIMBenchFixture.add'
# exit 1，speed-up 1.98597 < 2.0
```

`PIMBenchFixture.add` 的 G0 已知失败保持不变；未修改阈值或测试。G2 的 trace 不改变 PIMKernel 的数值结果。

### 7. 失败与修正

1. 首次 G2 schema validator 把 CSV 中的 `event_seq`/`cycle` 当作字符串，导致 events_schema FAIL；在验证前把这两个字段转换为整数后，ext_on run 全部 PASS。
2. 最初把 PIM 广播 descriptor 的 address-mapped bank 写进 `actual_bank`，会误导为单一实际 bank；增加 `PIM_BROADCAST`/`PIM_CONTROL` 分类，广播事件 `actual_bank` 留空并标 `:PIM_BROADCAST`，控制事件标 `:PIM_CONTROL`。validator 同时检查该规则。
3. G1 的 `environment/source.patch` 被 G2 累计补丁替换；已将 G1 快照保存为 `environment/source_g1.patch`，并在 `environment/g1.lock.json` 的 source_patch 字段指向该快照，避免 G1 证据失效。
4. 没有删除测试、放宽阈值、伪造事件或把 reports/truth 填成零事件。

### 8. G2 交付文件

```text
docs/g2_protocol.md
configs/schema.json
simulator/src/Phase1Trace.{h,cpp}
simulator/src/Phase1Feeder.{h,cpp}（G2 trace hooks）
environment/source.patch（累计）
environment/source_g1.patch（G1 快照）
environment/g2_binary.sha256
scripts/run_g2.py
scripts/validate_run.py
tests/test_schema.py
runs/g2/...
results/g2/comparison.csv
results/g2/validation.json
runs/g2/clean_replay/...
logs/g2_*.log
```

### 9. 下一关唯一任务

**G3：实现独立 SECDED 模块与存储注入。要求 ECC 单元测试穷举单/双 bit、C++/Python 交叉检查，并完成一次 F1 witness。**

只有用户明确指示后才进入 G3。G0/G1/G2 的基线、构建、测试、environment、runs、patch 和 clean replay 产物必须保留。

## G3 — SECDED 独立模块、存储注入与 F1 witness

执行日期：2026-11-15（UTC）。G2 的 `results/g2/validation.json`、G1/G0 只读产物在开始前核对 PASS。本关未接入全路径 ECC，未实现 CE/UE 报告、输出门禁、暂停或恢复。

### 1. 实现范围

新增：

```text
simulator/src/Phase1Ecc.{h,cpp}
simulator/src/Phase1Storage.{h,cpp}
simulator/src/tests/Phase1EccTestCases.cpp
tests/test_ecc_reference.py
scripts/run_g3.py
docs/g3_ecc.md
```

`Phase1Ecc` 是显式 extended Hamming SECDED(72,64) 研究模型：

- 位置 1..71 中 1/2/4/8/16/32/64 为 Hamming parity；其余 64 个位置按升序承载 data bit 0..63。
- 位置 72 是前 71 位的整体偶校验。
- 8-bit parity byte：bit 0..6 对应 Hamming 位置，bit 7 对应位置 72。
- 数据按 little-endian bit 顺序：data bit j = byte j/8 的 j%8 位。
- 这是模型布局，不声称真实 HBM 使用相同 codeword。

`Phase1Storage` 以 `(channel, rank, actual_bank, row, burst_column, codeword_index)` 为 key，保存 `(data64, parity8)`；正常写重新编码，读取不自动 scrub；`injectDataBit`/`injectParityBit`/raw 读取是 test/evaluator-only 接口。

### 2. 穷举测试

C++ `Phase1Ecc.ExhaustiveSingleAndDoubleBit` 使用 8 个固定数据 pattern：

```text
0x0000000000000000, 0xFFFFFFFFFFFFFFFF,
0x5555555555555555, 0xAAAAAAAAAAAAAAAA,
0x0123456789ABCDEF, 0xFEDCBA9876543210,
0x0F1E2D3C4B5A6978, 0x8877665544332211
```

每个 pattern：

- 无故障 encode/decode -> CLEAN；
- 72 个单 bit 翻转子 -> 全部 CORRECTED，纠正数据等于原数据；
- C(72,2)=2556 个双 bit 翻转子 -> 全部 UNCORRECTABLE。

总行数 `8 × (1 + 72 + 2556) = 21032`。C++ CSV 写入 `runs/g3/ecc_cpp_exhaustive.csv`（`PHASE1_ECC_OUT`）。

`tests/test_ecc_reference.py` 用 72-bit list 与显式位置集合独立实现编码/解码，不复用 C++ 代码；17 个 pytest 全部通过，并逐行检查 C++ CSV 的 `status`、`corrected_hex`、`syndrome`、`overall_parity`。核对结果：

```text
statuses: CLEAN=8, CORRECTED=576, UNCORRECTABLE=20448
```

### 3. F1 witness

`Phase1Storage.F1WitnessAndNoScrub`：

- key：ch0/rank0/bank0/row0/burst_col0/cw0；
- data：`0x0123456789ABCDEF`；
- F1 raw data bit 15（FP16 lane 0 sign bit），parity 不变；
- 读取：`CORRECTED`，corrected data 等于原值；
- 第二次读取仍 `CORRECTED`，raw data 保持翻转，证明读取不自动 scrub；
- 合法写后 `CLEAN`。

witness 文件：`runs/g3/f1_witness.json`。

`Phase1Storage.DoubleBitAndReencode` 还验证同一 key 双 bit -> UNCORRECTABLE、合法写后 CLEAN，以及不同 codeword key 互不影响。

### 4. 命令与结果

```bash
scons NO_STORAGE=0 NO_LIBRARY=1              # exit 0, logs/g3_build.log
PHASE1_ECC_OUT=... PHASE1_G3_WITNESS=... \
  ./sim --gtest_filter='Phase1Ecc.*:Phase1Storage.*'   # exit 0, 3 tests PASS
.venv/bin/python -m pytest -q tests/test_ecc_reference.py  # 17 passed
.venv/bin/python scripts/run_g3.py            # 15/15 checks PASS
```

G3 runner 结果：`results/g3/validation.json`、`results/g3/ecc_summary.json`、`runs/g3/run_manifest.json`。

原始回归：

```bash
./sim --gtest_filter='PIMKernelFixture.*' ... # 5/5 PASS
./sim --gtest_filter='PIMBenchFixture.add'    # 已知 1.98597 < 2.0 失败，保留
```

### 5. 二进制、补丁与干净重放

- G3 工作区二进制：`6b31ba8a06ab08f746cfb9ec32a25f8534ad9168976845f244fc99412c41ec89`（`environment/g3_binary.sha256`；副本 `runs/g3/sim-g3`）。
- 累计 G3 补丁：`environment/source.patch` SHA256 `933b32d7f4ef8168882a95963b14516c3d497dc0e8fe54ac8f91455e64b68c8d`。
- G2 补丁快照：`environment/source_g2.patch` `2b7b65...`；G1 快照 `environment/source_g1.patch` `629a36...`。
- 干净 worktree：`/tmp/opencode/pim-g3-clean`；`git apply --check`/`git apply` exit 0；构建 exit 0；clean binary `97809a7aa0944b14bf0a4f6cef4b68ef0f4cc65cd7e6f0053af57bd02885def5`。
- 干净副本：ECC 3/3 PASS、Python 17 passed、F1 witness 正确、PIMKernel 5/5 PASS；clean ECC CSV 与主工作区逐字节相同。
- 证据：`logs/g3_clean_worktree.log`、`g3_clean_apply.log`、`g3_clean_build.log`、`g3_clean_smoke.log`、`runs/g3/clean_replay/`。

### 6. 失败与修正

1. 第一版 G3 测试运行前没有单独保留 G2 补丁；已立即复制为 `environment/source_g2.patch`，再生成累计 G3 补丁。
2. 没有修改 C++/Python 的预期结果来绕过失败；穷举第一次运行即通过。未发现 decoder 越界或错误纠正。
3. G3 只做 storage-level F1 witness，没有声称 PIM operand path 已覆盖；全路径集成留给 G4。

### 7. G3 交付文件

```text
docs/g3_ecc.md
simulator/src/Phase1Ecc.{h,cpp}
simulator/src/Phase1Storage.{h,cpp}
simulator/src/tests/Phase1EccTestCases.cpp
tests/test_ecc_reference.py
scripts/run_g3.py
environment/source.patch（累计）
environment/source_g1.patch
environment/source_g2.patch
environment/source_g3.patch
environment/g3_binary.sha256
runs/g3/ecc_cpp_exhaustive.csv
runs/g3/f1_witness.json
runs/g3/clean_replay/
results/g3/ecc_summary.json
results/g3/validation.json
logs/g3_*.log
```

### 8. 下一关唯一任务

**G4：实现全路径 ECC 与粗粒度报告。要求 P1-A/P1-B smoke 通过，报告位置来自真实 bank/page 映射且无广播混淆。**

只有用户明确指示后才进入 G4。G0/G1/G2/G3 的基线、构建、测试、environment、runs、patch 和 clean replay 产物必须保留。

## G4 — 全路径 ECC 与粗粒度报告

执行日期：2026-11-15（UTC）。G3 的 `run_g3.py`、G0/G1 只读验证在开始时 PASS。本关未实现 G5 输出状态门禁/hold，也未实现 G6 暂停或恢复。

### 1. 全路径接入

- `Bank` 新增 `Phase1Profile`、`Phase1AccessType`、报告 sink、actual bank identity 和 `Phase1Storage`。
- `Rank::readSb/writeSb` 显式传 `HOST`；`PIMRank` 的 HAB、operand、result 分别传 `HAB`、`PIM_OPERAND`、`PIM_RESULT`。
- `BusPacket` 增加 `phase1_status`（OK/CORRECTED/UNPROTECTED_FORMAT/UNCORRECTABLE）。
- `Phase1Storage` 增加 `writeUnprotected` 与 `ecc_valid`；HOST_ONLY 的 PIM 写回不沿用陈旧 parity。
- `Phase1Profile.h` 定义四个 profile；`Phase1Reports.{h,cpp}` 实现 Q=64、I、D、bank/page region 的 FIFO 报告队列。
- `reports.csv` 只有 `report_seq,report_cycle,error_class,region_type,region_id`；detect cycle 只在测试 metrics 中。

实现文件：

```text
simulator/src/Phase1Profile.h
simulator/src/Phase1Reports.{h,cpp}
simulator/src/Bank.{h,cpp}
simulator/src/BusPacket.h
simulator/src/Rank.cpp
simulator/src/PIMRank.cpp
simulator/src/Phase1Storage.{h,cpp}
simulator/src/tests/Phase1G4TestCases.cpp
scripts/run_g4.py
docs/g4_ecc_reports.md
```

### 2. P1-A smoke

矩阵：4 profiles × 4 paths × {F0,F1,F2} × {bank 0,3,15} = 144 case，n_tiles 不需要（使用 1-channel W0/短 PIM storage 访问），seed=0。所有 144/144 通过。

| profile | cases | internal CE | internal UE | delivered | format-invalid cases |
|---|---:|---:|---:|---:|---:|
| NATIVE | 36 | 0 | 0 | 0 | 0 |
| LOCAL_VISIBLE | 36 | 12 | 12 | 24 | 0 |
| LOCAL_HIDDEN | 36 | 12 | 12 | 12（仅 UE） | 0 |
| HOST_ONLY | 36 | 3 | 3 | 6 | 9 |

- LOCAL_VISIBLE F1 四路径输出纠正并送达 CE。
- LOCAL_HIDDEN F1 内部 CE 12、public 0、输出纠正；F2 UE 仍作为 public。
- NATIVE F1/F2 无 ECC/report，原始输出改变。
- HOST_ONLY HREAD 的 host 读取仍纠正；PREAD/PWRITE_PREAD 的 PIM 路径不纠正、不报告；PWRITE_HREAD 的 PIM 写回产生 4 个 `format_events`，`UNPROTECTED_FORMAT`，不是 CE/UE。
- 报告 bank region 对应实际 target bank 0/3/15，不使用广播 packet bank。

### 3. P1-B smoke

63 case：LOCAL_VISIBLE，W0 单次 protected read，bank 0/3/15，granularity bank/page，D∈{0,1,16,256,4096}，I∈{1,8}，另加 3 个 32-read burst（I=8）。

- 所有单事件 case：internal CE=1、delivered=1、dropped=0、输出正确。
- 实测通知延迟严格为 `max(1,D)`：D=0/1 -> 1；D=16 -> 16；D=256 -> 256；D=4096 -> 4096。
- bank/page region 与实际映射检查通过；page 可能包含多个 bank。
- 32-read burst：32 条真实 CE、无 drop，report_seq 连续，服务间隔不小于 I=8。
- 63/63 runner checks PASS。

### 4. 命令与文件

```bash
scons NO_STORAGE=0 NO_LIBRARY=1              # exit 0, logs/g4_build.log
.venv/bin/python scripts/run_g4.py            # 207 cases, exit 0
./sim --gtest_filter='PIMKernelFixture.*' ... # 5/5 PASS
./sim --gtest_filter='Phase1Ecc.*:Phase1Storage.*' # 3/3 PASS
./sim --gtest_filter='PIMBenchFixture.add'    # known 1.98597 < 2.0
```

结果：`results/g4/validation.json` 6/6 checks；`results/g4/p1a_summary.csv` 144 行；`p1b_summary.csv` 63 行；`summary.csv` 207 行；`runs/g4/**/truth.csv` 写 INJECTION 行（raw before/after、bank/codeword/bit 位置）。runtime/public path 不读取 truth。

### 5. 二进制、补丁与干净重放

- G4 工作区二进制：`1d3aa682a2f91740e755a52be3d71976ea790ce91d463903ce91a74e83b19ef3`（`environment/g4_binary.sha256`；副本 `runs/g4/sim-g4`）。
- 累计 G4 补丁：`environment/source.patch` SHA256 `9c73317883455b888f76367db2649bdc1783d630febc3c4dab9b5ce3c552b492`。
- G3 snapshot `environment/source_g3.patch` `933b32...`；G2/G1 snapshots 保留。
- 干净 worktree `/tmp/opencode/pim-g4-clean`：`git apply --check`/`git apply` exit 0；构建 exit 0；clean binary `0b63dd05...`。
- 干净 smoke：7 个选定 G4 case（LOCAL_VISIBLE HREAD/PREAD、HOST_ONLY format、NATIVE、LOCAL_HIDDEN、page D16、32-read burst）全部 PASS；ECC 3/3；PIMKernel 5/5。证据 `runs/g4/clean_replay/`、`logs/g4_clean_*.log`。

### 6. 失败与修正

1. G4 初次 runner 校验把 P1-A F0 case 误套 P1-B 的单事件延迟检查，导致空 `report_latencies` 索引错误；已将额外延迟检查限定为 `P1B_` case。
2. page granularity 的 region_id 最初被错误按 bank 期望校验；已按 `Phase1Address::encode(...) / 4096` 计算 page，并区分 bank/page。
3. truth.csv 的 `storage_key` 含逗号但未加 CSV 引号，已加双引号；truth 仍只由 evaluator 写出。
4. 干净副本最初使用 `../configs` 硬编码路径失败；G4 fixture 改为从 params 读取 `device_ini`/`system_ini`，main 和 clean replay 均通过。
5. 未删除测试、放宽阈值或把 `PIMBenchFixture.add` 写成通过。

### 7. G4 交付文件

```text
docs/g4_ecc_reports.md
simulator/src/Phase1Profile.h
simulator/src/Phase1Reports.{h,cpp}
simulator/src/Bank.{h,cpp}
simulator/src/BusPacket.h
simulator/src/Rank.cpp
simulator/src/PIMRank.cpp
simulator/src/Phase1Storage.{h,cpp}
simulator/src/tests/Phase1G4TestCases.cpp
scripts/run_g4.py
environment/source.patch（累计）
environment/source_g4.patch（G4 snapshot）
environment/g4_binary.sha256
runs/g4/...
results/g4/...
logs/g4_*.log
```

### 8. 下一关唯一任务

**G5：实现输出状态、host/device 门禁与未发布版本保护。要求 P1-C smoke、同周期边界与旁路访问负测通过。**

只有用户明确指示后才进入 G5。G0/G1/G2/G3/G4 的基线、构建、测试、environment、runs、patch 和 clean replay 产物必须保留。

## G5 — 输出状态、host/device 门禁与未发布版本保护

执行日期：2026-11-15（UTC）。G4 的 P1-A/P1-B smoke、G3 ECC、G0/G1 只读验证在开始时 PASS。本关未实现 G6 的 pause/quiesce 或 G7 的 restore。

### 1. 实现

新增 `Phase1Runtime`：

```text
ALLOCATED -> PRODUCING -> READY_UNPUBLISHED -> PUBLISHED
                   \              \
                    -> SUSPECT <---
                    -> POISONED
```

- CE public report -> SUSPECT；UE -> POISONED；两者都不能超时自动 PUBLISHED。
- H cycles 从 producer complete 起算；H=0 在第一次 tick 允许发布。
- 新版本分配使旧 handle stale/ABORTED。
- host consumer 只在 PUBLISHED 可读；PIM consumer 只在 PUBLISHED 可 `canAdmit`。
- producer 内部 W1 读写不受自己的 output gate 阻塞。
- `assertConsumerAllowed` 是 test/evaluator bypass 的 fail-fast guard，抛 `STALE_HANDLE` 或 `UNPUBLISHED_OUTPUT:<state>`，不在 `Bank::read()` 内等待。
- 结果仍写在 HBM result 区域；runtime 只保存版本/状态元数据，不复制整份输出。

新增/修改：

```text
simulator/src/Phase1Runtime.{h,cpp}
simulator/src/tests/Phase1G5TestCases.cpp
scripts/run_g5.py
docs/g5_runtime.md
```

### 2. G5 smoke

12 个 W1 case：F0/F1/F2 × H=0/8 × consumer HOST/PIM，n_tiles=1、seed=0、LOCAL_VISIBLE。

| fault | state after hold | published | host/PIM gate | reports |
|---|---|---|---|---|
| F0 | PUBLISHED | yes | admitted | none |
| F1 | SUSPECT | no | blocked | one CE |
| F2 | POISONED | no | blocked | one UE |

- F0 HOST 输出与独立 A+X 参考逐 bit 一致；F0 PIM 仅验证 admission，不实现 W2，因此不产生消费者数据。
- F1/F2 的 output.bin 为空；host/PIM 均 blocked；`bypass_assert=true`。
- F1/F2 的 `truth.csv` 写 INJECTION 行（A 的 bank0/row0/cw0/bit 15 或 0|1），runtime 不读取 truth。
- `Phase1Runtime.SameCycleAndStaleHandles`：report 在 publish 前一 cycle -> SUSPECT；same cycle 在 tick 前 -> SUSPECT；publish 后一 cycle -> late report 且保持 PUBLISHED；旧 handle stale；unpublished host/PIM gate false 且 bypass 抛异常。
- G5 runner 9/9 checks PASS；runtime test PASS。

### 3. 命令、哈希与干净重放

```bash
scons NO_STORAGE=0 NO_LIBRARY=1              # exit 0, logs/g5_build.log
.venv/bin/python scripts/run_g5.py            # 12/12, exit 0
./sim --gtest_filter='PIMKernelFixture.*'     # 5/5 PASS
./sim --gtest_filter='Phase1Ecc.*:Phase1Storage.*' # 3/3 PASS
./sim --gtest_filter='PIMBenchFixture.add'    # known 1.98597 < 2.0
```

- G5 工作区二进制：`1a527f95cd01af19a6306cdf35c8a55cde3be29b3bea7b56ff7230289315cf3a`（`environment/g5_binary.sha256`；副本 `runs/g5/sim-g5`）。
- 累计 G5 补丁：`environment/source.patch` SHA256 `f3d89838ce51df16d0eea8c197f2947b36cbad9298aa4fdd3deff86e5e999120`。
- G4 snapshot `environment/source_g4.patch` `9c7331...`；G3/G2/G1 snapshots preserved。
- 干净 worktree `/tmp/opencode/pim-g5-clean`：apply check/apply exit 0；build exit 0；clean binary `5ca0c661...`。
- 干净 smoke：12/12 G5 case、runtime same-cycle test、ECC 3/3、PIMKernel 5/5 PASS。证据 `runs/g5/clean_replay/`、`logs/g5_clean_*.log`。

### 4. 失败与修正

1. G5 首次 F1/F2 运行在 preload transaction 尚未执行时注入，Phase1Storage key 不存在；改为先运行 preload、再注入、再执行 W1。
2. producer report 可以在 producerComplete 之前把状态置 SUSPECT/POISONED；`producerComplete` 现在保留锁定状态，不再错误要求 PRODUCING。
3. clean 副本最初使用 `../configs` 硬编码路径失败；G5 fixture 改为从 params 读取 `device_ini`/`system_ini`。
4. G5 truth CSV 的 storage_key 已加引号，避免逗号破坏列。
5. 未删除测试、放宽阈值或把 `PIMBenchFixture.add` 写成通过。

### 5. G5 交付文件

```text
docs/g5_runtime.md
simulator/src/Phase1Runtime.{h,cpp}
simulator/src/tests/Phase1G5TestCases.cpp
scripts/run_g5.py
environment/source.patch（累计）
environment/source_g5.patch
environment/g5_binary.sha256
runs/g5/...
results/g5/...
logs/g5_*.log
```

### 6. 下一关唯一任务

**G6：实现暂停、排空与两 channel 验证。要求 P1-D smoke，ACK 后数据访问为 0，REF 正常。**

只有用户明确指示后才进入 G6。G0/G1/G2/G3/G4/G5 的基线、构建、测试、environment、runs、patch 和 clean replay 产物必须保留。

## G6 — 暂停、排空与两 channel 验证

执行日期：2026-11-15（UTC）。G5 输出门禁、G4 P1-A/P1-B、G3 ECC、G0/G1 只读验证均 PASS。本关未实现 G7 的恢复/重执行。

### 1. 实现

新增：

```text
simulator/src/Phase1Pause.{h,cpp}
simulator/src/tests/Phase1G6TestCases.cpp
scripts/run_g6.py
docs/g6_pause.md
```

- `Phase1FeederMemory` 增加 `freezeChannel`/`unfreezeChannel`/`tickChannel`/`channelIdle`；冻结只阻止目标 channel 的新提交，外部 pending 不丢弃。
- `Phase1PauseController` 实现 `QUIESCE_REQUEST -> drain -> QUIESCE_ACK`，支持 synthetic `request()` 和 report-triggered `notifyReport()` + handler delay L。
- `Bank` 增加 data access 计数；`Rank` 增加 refresh 计数；用于 ACK 后数据访问与 REF 检查。
- 时钟/REF 不冻结；观察窗为 8000 cycles（要求至少 max(32,2*tRFC)=700）。

### 2. G6 smoke

16 cases：

- 4 个暂停时点：before/queue/inflight/near；
- K∈{1,8,32}，共 12 个 synthetic cases；
- actual CE L∈{0,8,64}（W1 F1 report trigger）；
- two-channel smoke。

结果：

- 16/16 PASS，全部 ACK，无 timeout。
- 所有 case `data_delta_after_ack=0`；观察窗 clock_delta=8000；refresh 不倒退并观察到增加。
- synthetic external pending 保持冻结（约 2528/2529；near 16）。
- actual CE：
  - report cycle 1305；
  - L=0/8/64 -> request 1305/1313/1369；
  - ACK 1354/1354/1422；
  - ACK 后数据访问 0；外部 pending 68/68/59。
- two-channel：channel0 8->8，channel1 8->64；external pending 56；ACK 后 channel0 无新数据访问。
- two-channel 使用独立 host traffic，不是完整 W1/W3 kernel；不声称零性能影响。
- 暂停为 channel 粒度；不声称 bank 粒度。

### 3. 命令与哈希

```bash
scons NO_STORAGE=0 NO_LIBRARY=1              # exit 0, logs/g6_build.log
.venv/bin/python scripts/run_g6.py            # 16/16, exit 0
./sim --gtest_filter='PIMKernelFixture.*'     # 5/5 PASS
./sim --gtest_filter='Phase1Ecc.*:Phase1Storage.*' # 3/3 PASS
./sim --gtest_filter='PIMBenchFixture.add'    # known 1.98597 < 2.0
```

- G6 工作区二进制：`13cf8ab4cd178b6c01b5f60a6cdee5ed04a13076a67371b768b813b8865d5d4f`（`environment/g6_binary.sha256`；副本 `runs/g6/sim-g6`）。
- 累计 G6 补丁：`environment/source.patch` SHA256 `53b1887f32e40a16549f233be65003fb3d16f5795a01ebc352267efa59743c16`。
- G5 snapshot `environment/source_g5.patch` `f3d898...`；G4/G3/G2/G1 snapshots 保留。
- 干净 worktree `/tmp/opencode/pim-g6-clean`：apply check/apply exit 0；build exit 0；clean binary `7f531f676c1aefb98d452216ccdbc404fd593fc699499dcbfeb20dfb8579e91f`。
- 干净 smoke：16/16 G6、ECC 3/3、PIMKernel 5/5 PASS；证据 `runs/g6/clean_replay/`、`logs/g6_clean_*.log`。

### 4. 失败与修正

1. G6 首次实现使用 `Phase1Address::fromConfiguration()` 直接构造 `Phase1Reports`，类型错误；改为显式 `Phase1Address reportCodec(geometry)`。
2. two-channel 初始只排入 8 个事务，暂停前已全部完成，无法显示 channel1 继续进度；增至 64 个事务后 channel1 8->64，channel0 停止。
3. G6 fixture 从 params 读取 device/system ini，避免 clean replay 路径硬编码。
4. 未删除测试、放宽阈值或把 `PIMBenchFixture.add` 写成通过。

### 5. G6 交付文件

```text
docs/g6_pause.md
simulator/src/Phase1Pause.{h,cpp}
simulator/src/tests/Phase1G6TestCases.cpp
scripts/run_g6.py
environment/source.patch（累计）
environment/source_g6.patch
environment/g6_binary.sha256
runs/g6/...
results/g6/...
logs/g6_*.log
```

### 6. 下一关唯一任务

**G7：实现可信输入恢复与状态复位。要求 P1-E 正例正确，五类负例正确拒绝。**

只有用户明确指示后才进入 G7。G0–G6 的基线、构建、测试、environment、runs、patch 和 clean replay 产物必须保留。

## G7 — 可信输入恢复与状态复位

执行日期：2026-11-15（UTC）。G0/G1 只读验证、G3 ECC、G4/G5/G6 回归在本关开始和结束时均保留；本关未进入 G8。

### 1. 实现

新增：

```text
simulator/src/Phase1Checkpoint.{h,cpp}
simulator/src/tests/Phase1G7TestCases.cpp
scripts/run_g7.py
docs/g7_recovery.md
```

修改：

```text
simulator/src/Phase1Runtime.{h,cpp}    # QUIESCED/RESTORING/REEXECUTING 转换
simulator/src/Phase1Feeder.{h,cpp}     # clearPending() 丢弃旧未提交 descriptor
simulator/src/PIMRank.{h,cpp}          # resetState() 复位 CRF/GRF/SRF/中间/控制状态与 mode
simulator/src/Bank.h                   # test/evaluator 只读 raw storage 视图
simulator/src/tests/Phase1G7TestCases.cpp
```

- `Phase1CheckpointStore` 保存不可变 A/X、version 与 FNV-1a checksum；恢复源不是 `reference.bin`。
- `Phase1Runtime::quiesceOutput` 允许 `PRODUCING`（显式 probe）或 `SUSPECT`（CE report）进入 `QUIESCED`；`beginRestore` 只接受 `QUIESCED`，`PUBLISHED` -> `ALREADY_COMMITTED`，旧 handle -> `STALE_HANDLE`；`beginReexecute` 接受 `ALLOCATED`/`RESTORING`。
- `PIMRank::resetState` 将 `pimPC_`、jump/repeat/control 标志、CRF 和所有 PIM block 的 GRF/SRF/中间值归零，并把 rank mode 置为 `SB`。
- 恢复写入使用 `MultiChannelMemorySystem::addTransaction(true, ...)` 的模拟 host 写事务；feeder 中旧 kernel 未提交工作由 `clearPending()` 丢弃，已接受事务由 G6 pause/drain 完成。
- 恢复前写入已知非默认 GRF A/B、SRF、mOut、aOut 和 CRF word；reset 后逐项检查归零。

### 2. G7 正例矩阵

- `n_tiles∈{1,8}`，`seeds=0..9`，共 20 个 report-triggered cases。
- 每个 case 在 A 的 bank0/row0/cw0 bit15 注入 F1；真实 CE report 触发 pause。
- 另有 4 个显式 `RECOVERY_PROBE`：n_tiles=1/8、seeds=0/1；probe 在旧工作提交前冻结，不注入存储故障，单独标注来源。
- 每个 case 都检查：旧输出未发布、checkpoint bytes、restore writes/bytes、raw A/X 逐 codeword、状态复位、旧 pending 丢弃、重执行 drain、无新 CE/UE、新版本发布、HOST 读取、raw B 和独立 Python 逐 bit 比较。

实际结果：

| 指标 | n_tiles=1 | n_tiles=8 |
|---|---:|---:|
| checkpoint bytes | 8192 | 65536 |
| restore host writes | 256 | 2048 |
| restore bytes | 8192 | 65536 |
| discarded pending（report） | 68 | 446 |
| discarded pending（probe） | 103 | 481 |
| CE report cycle | 1305 | 8938 |
| request -> ACK | 1313 -> 1354 | 8946 -> 8987 |
| quiesce cycles | 41 | 41 |
| restore loop cycles | 557 | 4274 |
| reexec loop cycles | 804 | 4130 |
| ACK -> publish cycles | 1361 | 8404 |
| version | 1 -> 2 | 1 -> 2 |
| 最终 bit-exact | 10/10 | 10/10 |

- Report cases 的旧输出状态为 `SUSPECT`，probe 为 `PRODUCING`；均未在恢复前发布。
- report cases 的 `mode_before_reset=HAB_PIM`，reset 后 `mode_after_reset=SB`。
- 20/20 report + 4/4 probe 的 `restored_raw_a_ok`、`restored_raw_x_ok`、`raw_b_all_ok`、`output_bit_exact` 均为 true。
- 重执行阶段 report/CE/UE 计数均为 0。

### 3. 五类负例

runner 为每个负例启动独立进程，读取实际返回值并写入 `metrics.json`：

| case | observed |
|---|---|
| CHECKPOINT_MISSING | CHECKPOINT_MISSING |
| VERSION_MISMATCH | VERSION_MISMATCH |
| CHECKPOINT_CORRUPT | CHECKPOINT_CORRUPT |
| ALREADY_COMMITTED | ALREADY_COMMITTED |
| STALE_HANDLE | STALE_HANDLE；`old_can_consume=false`、`current_version=2` |

### 4. 必要初始化负面对照

1. `NO_INPUT_RESTORE`（n_tiles=1, seed=0）：跳过恢复写。原始 F1 仍留在 raw A（`restored_raw_a_mismatches=1`），重执行产生 2 个 CE，新输出停在 `SUSPECT`、HOST 不可读、未发布。证明恢复写不是空操作。
2. `NO_STATE_RESET`（n_tiles=1, seed=0）：跳过 `resetState()`。rank 停在 `HAB_PIM`，模拟 host restore writes 在 10000-cycle watchdog 内无法完成（`restore_timeout=true`），恢复在发布前被捕获。

### 5. 执行命令与退出码

```bash
cd simulator
scons NO_STORAGE=0 NO_LIBRARY=1                         # exit 0, logs/g7_build.log
cd ..
.venv/bin/python scripts/run_g7.py                      # 31/31 cases, exit 0, logs/g7_run.log
cd simulator
./sim --gtest_filter='PIMKernelFixture.*' --gtest_output=xml:../runs/g7/upstream-tests.xml  # 5/5, exit 0
./sim --gtest_filter='Phase1Ecc.*:Phase1Storage.*'      # 3/3, exit 0
./sim --gtest_filter='Phase1Checkpoint.*'               # 1/1, exit 0
./sim --gtest_filter='PIMBenchFixture.add'              # exit 1, known 1.98597 < 2.0
cd ..
```

- G7 工作区二进制：`0bb33025dfb0b5014be91f2cfa24439071844298554acb9de251f0b07c0c5c58`（`environment/g7_binary.sha256`；副本 `runs/g7/sim-g7`）。
- 累计 G7 补丁：`environment/source.patch` SHA256 `d94c5cfbfbc76fefae825a0a33d29a4a9e843bbb25fd4edce94389737821be5e`；`environment/source_g7.patch` 保存同一快照。
- G6 snapshot `environment/source_g6.patch` `53b188...` 保留；G5/G4 等更早 snapshot 未覆盖。
- G6 lock 的 `source_patch` 引用改为不可变 `environment/source_g6.patch`；G7 前的原始 lock 保存在 `environment/g6.lock.pre_g7.json`（SHA256 `4d3a39...`）。
- 干净 worktree `/tmp/opencode/pim-g7-clean`：`git apply --check`/`apply` exit 0；`scons NO_STORAGE=0 NO_LIBRARY=1` exit 0；clean binary `b24a34f5b30883bf5f32231f3ecaa633604a688952f8029c73af585795c15b82`。
- 干净 smoke：31/31 G7、ECC 3/3、PIMKernel 5/5、checkpoint 1/1 PASS；证据 `runs/g7/clean_replay/`、`logs/g7_clean_smoke.log`。
- G0/G1 回归：`G0_ARTIFACT_VALIDATION=PASS`、`G1_VALIDATION=PASS`；补丁反向检查 exit 0；`git diff --check` exit 0（`logs/g7_final_audit.log`）。

### 6. 失败与修正

1. 初始正例把 `reexecReports` 设为 0，重执行时重放旧 CE，导致新输出被错误锁为 `SUSPECT`；改为从 `reports.reports().size()` 开始消费，并单独保存 `reexecReportBase` 计算新报告数。
2. `readData` 最初使用 `runDirectWrites`，但 `Phase1FeederMemory` 拦截读事务，结果全 0；改为 `runFeederLoop` 后逐 bit 正确。
3. 显式 probe 时旧输出仍在 `PRODUCING`，`quiesceOutput` 初始只接受 `SUSPECT/READY_UNPUBLISHED`；增加 `PRODUCING` 的 G7 probe 转换。
4. `NO_INPUT_RESTORE` 的 `output_bit_exact` 为 false 是因为 HOST 读被 gate 阻止，不能单独证明 raw 输入仍有故障；增加 raw A/X 逐 codeword 检查和重执行 CE 计数后，证据变为 `raw A mismatch + 2 CE + 未发布`。
5. `NO_STATE_RESET` 最初直接抛异常、gtest 失败且无 metrics；改为在 restore watchdog 超时路径写结构化 `caught=true` 指标，作为预期负例通过，同时保留超时证据。
6. 重执行报告计数最初按引用变量 `reexecReports` 计算，循环后它已被消费指针推进，导致新报告数恒为 0；改用独立的 `reexecReportBase`。
7. 未删除测试、放宽阈值或把 `PIMBenchFixture.add` 写成通过。

### 7. G7 交付文件

```text
docs/g7_recovery.md
simulator/src/Phase1Checkpoint.{h,cpp}
simulator/src/Phase1Runtime.{h,cpp}
simulator/src/Phase1Feeder.{h,cpp}
simulator/src/PIMRank.{h,cpp}
simulator/src/Bank.h
simulator/src/tests/Phase1G7TestCases.cpp
scripts/run_g7.py
environment/source.patch（累计 G7）
environment/source_g7.patch
environment/g7_binary.sha256
environment/g7.lock.json
runs/g7/...
results/g7/...
logs/g7_*.log
```

### 8. 下一关唯一任务

**G8：展开 paper 矩阵并完整执行。要求 manifest 预期数量=成功+预期负例+失败+超时，无漏项。**

只有用户明确指示后才进入 G8。G0–G7 的基线、构建、测试、environment、runs、patch 和 clean replay 产物必须保留。

## G8 — Paper 矩阵展开与完整执行

执行日期：2026-11-15（UTC）。本关只执行 G8，未进入 G9；G0–G7 的二进制、运行、结果、补丁快照和 clean replay 均保留。

### 1. 实现

新增：

```text
configs/suite_paper.yaml
scripts/calibrate_g8.py
scripts/expand_suite.py
scripts/run_suite.py
scripts/aggregate.py
simulator/src/tests/Phase1G8P1CTestCases.cpp
simulator/src/tests/Phase1G8CostTestCases.cpp
simulator/src/tests/Phase1G8RobustTestCases.cpp
docs/g8_paper_matrix.md
```

- `expand_suite.py` 校验 YAML、展开矩阵、用规范配置哈希生成 16 位 run_id，并写出 `configs/generated/paper-manifest.json` 与逐 case config。
- `run_suite.py` 每 case 独立进程执行，写 params/config/metadata/stdout/stderr/exit_code/validation；支持 `--jobs` 与 `--resume`（仅跳过 binary、patch、config hash 一致且 validation PASS 的运行）。
- `aggregate.py` 对账 manifest，写出 `results/run_metrics.csv`、`summary.csv`、`failure_cases.csv`、`validation_summary.json` 和 `figure_data/` 六类图源数据。
- P1-C fixture 覆盖 direct W1、HOST/PIM consumer、W2=B×2、D/H ratio 校准值、report-triggered lock/quiesce、LOCAL_HIDDEN/HOST_ONLY 控制。
- P1-F cost fixture 使用 bounded feeder，覆盖 EXT_OFF、LOCAL_VISIBLE+H0/H0.25T/H1T。
- P1-F robustness fixture 覆盖 Q/I/drop 矩阵，drop=0.1 为确定性 observation gap 注入，记录具体 detection seq。

### 2. 矩阵与执行

| group | cases | composition |
|---|---:|---|
| P1-A | 144 | 4 profiles × 4 paths × 3 faults × 3 banks |
| P1-B | 63 | 60 D/I/granularity/bank + 3 burst |
| P1-C | 1848 | 1800 main + 48 LOCAL_HIDDEN/HOST_ONLY controls |
| P1-D | 22 | 12 synthetic + 9 actual CE + 1 two-channel |
| P1-E | 27 | 20 positive + 5 status negatives + 2 state/input controls |
| P1-F Cost | 480 | 2 sizes × 2 consumers × 30 seeds × 4 policies |
| P1-F Robustness | 18 | 3 Q × 3 I × 2 drop rates |
| **total** | **2602** | |

对账结果：

```text
SUCCESS             2595
EXPECTED_NEGATIVE      7
FAIL                   0
TIMEOUT                0
MISSING                0
```

- P1-C outcome：TIMELY_CONTROL 1118、LATE_CONTROL 682、NO_REPORT 24、FORMAT_UNSUPPORTED 24。
- LOCAL_HIDDEN 24 cases 为 NO_REPORT；HOST_ONLY 24 cases 为 FORMAT_UNSUPPORTED；两者均不记为恢复成功。
- P1-D：22/22，`data_delta_after_ack=0`，quiesce 1–402 cycles。
- P1-E：20 positive bit-exact；7 expected negative 全部正确识别。
- P1-F cost：H0 0.0%、H0.25T 平均 10.21%、H1T 平均 43.96% 模拟周期 overhead。
- P1-F robustness：8 个 overflow cases，最大队列深度 30；drop=0.1 固定丢弃 detection 10/20/30。

### 3. T_ref 校准

`scripts/calibrate_g8.py` 记录：

| path | n_tiles=1 | n_tiles=8 |
|---|---:|---:|
| P1-C direct producer | 731 | 3339 |
| P1-F feeder producer | 804 | 4130 |

- P1-C 用 `D=ceil(D_ratio*T_ref)`、`H=ceil(H_ratio*T_ref)`。
- P1-F 用 feeder T_ref 生成 H0/H0.25T/H1T。
- P1-C target burst：`seed mod 3` 选择 A burst 序列前/中/后三段，PCG64(seed) 在段内选择精确 burst；C++ 不重新随机。
- 校准证据：`results/g8/calibration.json`、`runs/g8/calibration/`、`logs/g8_calibration.log`。

### 4. 执行命令与退出码

```bash
.venv/bin/python scripts/calibrate_g8.py                              # exit 0
.venv/bin/python scripts/expand_suite.py --suite configs/suite_paper.yaml  # exit 0
.venv/bin/python scripts/run_suite.py --manifest configs/generated/paper-manifest.json --jobs 8  # exit 0
.venv/bin/python scripts/aggregate.py --runs runs/paper --out results # exit 0
cd simulator
./sim --gtest_filter='PIMKernelFixture.*'                             # 5/5, exit 0
./sim --gtest_filter='Phase1Ecc.*:Phase1Storage.*'                    # 3/3, exit 0
./sim --gtest_filter='Phase1Runtime.*:Phase1Contract.*:Phase1Checkpoint.*'  # exit 0
./sim --gtest_filter='PIMBenchFixture.add'                            # exit 1, known 1.98597 < 2.0
```

- G8 工作区二进制：`ab2165c02c6cb81283b8c40e36ce92e39ec809503fc75517423b11fce1607082`（`environment/g8_binary.sha256`；副本 `runs/g8/sim-g8`）。
- G8 累计补丁：`environment/source.patch` SHA256 `93daf08c1428bfd23f9f4cfadd2d5e5cd650b625ac84a7987adbd70601a40192`；`environment/source_g8.patch` 保存同一快照。
- G7 snapshot `environment/source_g7.patch` `d94c5c...` 保留；G6/G5/G4 等更早 snapshot 未覆盖。
- G7 lock 的 `source_patch` 引用改为不可变 `environment/source_g7.patch`；G8 前的原始 lock 保存在 `environment/g7.lock.pre_g8.json`（SHA256 `1941d4...`）。
- 干净 worktree `/tmp/opencode/pim-g8-clean`：apply check/apply exit 0；build exit 0；clean binary `71b115348ac19a53d666f260efc568c8cc52d3a918e31d21a36371182150e70a`。
- 干净 smoke：P1-A/B/C/D/E positive/expected negative/P1-F cost/robust 各至少 1 个，8/8 PASS；PIMKernel 5/5、ECC 3/3；证据 `runs/g8/clean_replay/`、`logs/g8_clean_smoke.log`。
- G0/G1 回归与补丁反向检查见 `logs/g8_final_audit.log`。

### 5. 失败与修正

1. G4/G6 的旧 validator 直接做 `case["out"] / "metrics.json"`，manifest 中 `out` 是字符串；run_suite 在调用前转换为 `Path`。
2. P1-C 最初把 `reports.pendingCount()` 放进 producer 循环条件，延迟报告把 `producer_cycles` 从 731 拉长到 1684；改为 producer 完成只按 direct idle 判定，报告在后续 hold/consumer 阶段继续交付。
3. P1-C 在 hold 阶段收到报告时只把 runtime 置 SUSPECT，未记录 lock/pause/ack，导致大量 `INVALID_CASE`；补上 hold 阶段的 lock/request/ack 记录。
4. P1-C 主矩阵只有 LOCAL_VISIBLE，无法产生 NO_REPORT/FORMAT_UNSUPPORTED；增加 LOCAL_HIDDEN 与 HOST_ONLY 各 24 个 controls。
5. P1-F cost 的 `ext_off_metadata_zero` 与 `protected_metadata_present` 布尔条件写反；修正后 480/480 PASS。
6. P1-F robustness 的 manifest case 缺少 `transport_delay_cycles`，validator 报 KeyError；补入 case metadata。
7. `--resume` 最初不重新加载 metrics，聚合表会丢指标；改为读取已有 `metrics.json`。
8. 未删除失败项、未放宽阈值；`PIMBenchFixture.add` 仍保留 1.98597 < 2.0 的已知基线失败。

### 6. 边界

- P1-C 使用 direct memory path（与 G5 smoke 一致），其 T_ref 为 direct 值，不是 bounded feeder；这是与 HANDOFF “same feeder”措辞的明确模型偏离。
- P1-C PIM consumer 的 W2 为设备侧 `B×2`，消费边界为第一次 B 读取。
- P1-D synthetic 的 L 不适用；L 只由 actual CE cases 覆盖。
- P1-F cost 是模拟周期成本，不是硬件墙钟；H0 的 0.0% 是周期模型结果，空间/元数据成本另列。
- P1-F robustness 的 drop 是确定性观测缺口注入，不是硬件误码模型。
- G8 只完成矩阵执行、对账和 figure_data；六张图、统计报告和 artifact 打包属于 G9。

### 7. 下一关唯一任务

**G9：统计、画图、论文式报告、干净复现。要求 6 图及数据、结果报告、补丁重放 smoke、hash manifest。**

只有用户明确指示后才进入 G9。G0–G8 的基线、构建、测试、environment、runs、paper、结果、补丁和 clean replay 产物必须保留。

## G9 — 统计、画图、论文式报告与 Artifact

执行日期：2026-11-15（UTC）。根据用户要求，本关不重跑模拟器 case；只读取 G8 已保存的 `results/figure_data/*.csv`、runs、logs 和 environment 产物。

### 1. 实现

新增：

```text
scripts/plot_paper.py
scripts/reproduce.sh
scripts/package_artifact.py
figures/
report/phase1_results.md
report/phase1_results.tex
report/references.bib
artifact/
environment/g9.lock.json
```

- `plot_paper.py` 读取 `results/figure_data/*.csv`，写 242 条统计、六张主图、补充 robustness 图、灰度 PNG、`figures/captions.md` 依赖的 plot manifest。
- 连续量使用 95% bootstrap（10000 次，固定 seed=20261115）；比例使用 Wilson 95%。
- `aggregate.py` 为 Fig.2b 增加 `fig2_report_events.csv`（逐报告 delivery cycle/cumulative count）。
- `reproduce.sh` 支持 `--smoke` 和 `--paper`；`package_artifact.py` 生成非递归 SHA256 清单和 tar.gz。
- 报告包含方法、五组结果、失败类别、能力判定、限制和复现步骤；`.tex` 提供英文版，当前环境无 LaTeX 工具链，未本地编译。

### 2. 统计与图

- 统计：`results/statistics.csv`、`results/statistics.json`，共 242 行；每行保存 n、numerator/denominator、mean/median/min/max 和 CI 方法。
- 图：`figures/fig01_path_coverage.*`、`fig02_report_latency.*`、`fig03_control_window.*`、`fig04_quiescence.*`、`fig05_restore.*`、`fig06_cost.*`、`figS1_robustness.*`。
- 每图输出 PDF、SVG、300 dpi PNG；`figures/gray/` 保存灰度 PNG。
- `figures/plot_manifest.json` 记录输入 CSV SHA256、列、过滤、样本数、聚合、输出文件 SHA256。
- 关键数值：
  - P1-C TIMELY 1118/1800（62.1%，Wilson 59.8–64.3%），LATE 682/1800（37.9%，35.7–40.2%），output_changed 0。
  - P1-E 20/20 正例；restore/reexec/recovery = 557/804/1361（nt1）、4274/4130/8404（nt8）。
  - P1-F H0/H0.25T/H1T 平均 overhead = 0.00%/10.21%/43.96%；空间 13.6/108.1 KiB。
  - P1-F robustness 18/18；Q=1,I=64 交付 3/32、29 queue drops；drop=0.1 固定 gap seq 10/20/30。

### 3. 命令与退出码

```bash
.venv/bin/python scripts/plot_paper.py --results results --out figures   # exit 0, logs/g9_plot.log
.venv/bin/python scripts/package_artifact.py                              # exit 0, logs/g9_package.log
sha256sum -c artifact/MANIFEST.sha256                                     # exit 0
```

- 未重跑 `expand_suite.py` / `run_suite.py` / 模拟器 case。
- `artifact/MANIFEST.sha256` 对 36960 个文件逐文件校验通过。
- `artifact/phase1-artifact.tar.gz` 约 80 MB；归档哈希写在 `artifact/ARCHIVE.sha256`，最终根 README 另列。
- G9 lock 记录 G0–G8 二进制/锁、G8 patch、统计、图、报告、artifact manifest 和 archive 哈希。

### 4. 失败与修正

1. Fig.1 右面板 y 标签与左面板注释重叠；改为共享 y 标签并移除右面板 y tick label。
2. Fig.2 x 轴标签过长被裁剪；缩短为 “configured D (cycles, categorical)”。
3. Fig.3 上排 x 标签与下排标题重叠；上排只保留 y 标签，下排保留 H/T_ref。
4. Fig.4 “uns paused” 拼写错误；改为 “unpaused”。
5. Fig.5 正负例注记与图例/柱子重叠；移入标题副行。
6. Artifact 首次 manifest 校验失败：`logs/g9_package.log` 在自身被写入时被纳入哈希；排除该日志后重新打包，36960 文件全部 PASS。
7. 本环境无 pdflatex/xelatex/lualatex/tectonic，`.tex` 未编译；未把未编译内容写成已编译结果。

### 5. 边界

- 图只使用保存的 `figure_data`，不重跑 case、不读取 truth 作为 runtime 观测。
- 统计的 30 seeds 是 30 个模拟场景，不是 30 个独立芯片；100% 结果的 Wilson 下界仍约 88.6%。
- P1-C 使用 direct memory path；P1-F 使用 bounded feeder；P1-D synthetic 的 L 不适用。
- 报告明确说明模型 ECC/接口、host/互连/OS 成本未建模，不能外推为真实商品平台结论。
- artifact 包含原始 runs/logs/results/figures/report/patches/locks，不包含 `.venv`、`simulator/.git`、`simulator/bin`、`simulator/lib` 和旧归档。

### 6. 交付位置

```text
results/statistics.csv, results/statistics.json
figures/fig01..fig06.*, figures/figS1_robustness.*
figures/captions.md, figures/plot_manifest.json
report/phase1_results.md, report/phase1_results.tex, report/references.bib
artifact/MANIFEST.sha256, artifact/ARCHIVE.sha256, artifact/phase1-artifact.tar.gz
scripts/plot_paper.py, scripts/reproduce.sh, scripts/package_artifact.py
environment/g9.lock.json
logs/g9_*.log
```

### 7. Phase 1 完成

G0–G9 全部完成。后续如继续，应按新任务重新规划，不修改已完成的证据、阈值或预期结果。

## G10 — 补充实验环境、回归基线与预注册

执行日期：2026-11-16（UTC）。执行对象：`PHASE1_SUPPLEMENT_HANDOFF.md` G10。工作目录：`pim-phase1/`。

### 1. 实际改动的文件

新增：

```text
scripts/make_regression_manifest.py
scripts/compare_regression.py
docs/g10_supplement_protocol.md
configs/generated/regression_g10-manifest.json
results/regression_g10/compare.csv
results/regression_g10/compare_summary.txt
results/regression_g10/frozen_sha256.txt
results/regression_g10/run_metrics.csv
results/regression_g10/summary.csv
results/regression_g10/validation.json
runs/regression_g10/<240 run dirs>/
logs/g10_apt_install.log
logs/g10_python_env.log
logs/g10_git_restore.log
logs/g10_regression_run.log
```

修改：

```text
scripts/run_suite.py                 # 新增 --results-dir（默认 results/g8）和 --runs-dir（默认 runs/paper）
docs/STATUS.md                       # G10 状态、证据、下一关
docs/implementation_log.md           # 本节
```

构建脚本实际重写（非冻结文件）：

```text
environment/binary.json
environment/binary.sha256
environment/build_flags.txt
runs/baseline/binary.json
runs/baseline/binary.sha256
logs/build_20261116T100819Z.log
```

未修改：`Phase1Ecc.cpp`、`Bank.*`、`Phase1Reports.*`、任何 C++ 源码/测试、`configs/suite_paper.yaml`、`configs/generated/paper*`、`runs/paper/`、`results/g8/`、`figures/`、`artifact/`、`environment/source_g1..g8.patch`。

### 2. 环境恢复

G10 开始时 `.venv/bin/python` 不存在，系统也没有 `python3`、`scons`、`libgtest-dev`。第一次 `apt-get install` 因 security 源索引过期返回 100（python3.10 包 404）；执行 `apt-get update` 后重装成功。

```bash
DEBIAN_FRONTEND=noninteractive apt-get update
DEBIAN_FRONTEND=noninteractive apt-get install -y \
  python3 python3-venv python3-pip scons libgtest-dev
# update exit 0, install exit 0; logs/g10_apt_install.log

python3 -m venv .venv
.venv/bin/python -m pip install --no-cache-dir -r environment/requirements.lock.txt
# pip exit 0（第一次安装被用户中断，重跑后完成）
.venv/bin/python -c "import numpy, pandas, yaml, matplotlib, jsonschema, pytest"
# exit 0
.venv/bin/python -m pip check
# exit 0: No broken requirements found.
.venv/bin/python -m pip freeze > /tmp/g10_pip_freeze.txt
diff -u environment/requirements.lock.txt /tmp/g10_pip_freeze.txt
# exit 0: 无差异
```

实际版本：Python 3.10.12、scons 4.0.1、libgtest 1.11.0、numpy 2.2.6、pandas 2.3.3、matplotlib 3.10.9、PyYAML 6.0.3、jsonschema 4.26.0、pytest 9.2.0。与 `environment/requirements.lock.txt` 完全一致。

### 3. 固定上游 git 元数据恢复

上传步骤曾按用户确认删除 `simulator/.git`。G10 环境检查发现 `git -C simulator rev-parse HEAD` 会穿透到外层仓库（返回 `8a595b2...`），导致 `scripts/build.sh` 的固定提交检查失败。

处置：用 GitHub API 读取固定提交的 commit/tree 元数据，用 `raw.githubusercontent.com` 下载 94 个 blob；对每个 blob 写入 `git hash-object -w` 后核对哈希；用 `git mktree` 自底向上重建 17 个 subtree 与 root tree；重建 commit 对象并核对 SHA1 等于 `3703d1f19c8f027360cc33a3243eb271e3bb6898`。最终：

```text
HEAD=3703d1f19c8f027360cc33a3243eb271e3bb6898
tree=7ac34faf86579aca00f5b357c61b5550d1dff7e7
git status --porcelain = 39 行（与删除 .git 前的修改/未跟踪状态一致）
```

该过程只写入 `simulator/.git` 对象，不覆盖工作区源文件。重建设脚本保存在 `/tmp/opencode/reconstruct_pim_git.py`（不在交付目录），日志：`logs/g10_git_restore.log`。

### 4. 构建

```bash
bash scripts/build.sh
# exit 0
# binary sha256=ab2165c02c6cb81283b8c40e36ce92e39ec809503fc75517423b11fce1607082
# 与 G8 记录一致；BUILD_OK, EXIT_CODE=0
```

日志：`logs/build_20261116T100819Z.log`。

### 5. 冻结哈希

在改脚本/跑回归之前记录 `results/regression_g10/frozen_sha256.txt`：

- 覆盖 `runs/paper/`、`results/g1..g8/`、`results/figure_data/`、`figures/`、`configs/generated/paper/`、`configs/generated/paper-manifest.json`、`configs/suite_paper.yaml`、`artifact/`、`results/statistics.csv/json`、`results/run_metrics.csv`、`summary.csv`、`failure_cases.csv`、`validation_summary.json`、`environment/source_g1..g8.patch`。
- 共 33,868 个文件。
- 工作区实际不存在 `environment/source_g0.patch`（G0 没有源码改动；原冻结清单写的是 g0..g8）。文件头部如实记录该缺失，没有创建空文件或伪造哈希。
- 第一次串行哈希在 600 s 超时（共享文件系统较慢）；改用 `xargs -P 16` 后 6 s 完成。G10 结束时用 16 线程复核：33,868/33,868 一致。

### 6. 回归 manifest

```bash
.venv/bin/python scripts/make_regression_manifest.py
# 第一次 exit 1：P1-C profile 位于 params 而非 case，筛选为 0
# 修正筛选后 exit 0
```

选择与数量：

```text
P1-A                     144
P1-C seed==0 LOCAL_VISIBLE 60
P1-E positive seed==0      2
P1-F Cost seed==0         16
P1-F Robustness           18
total                    240
```

只改 `params.output_dir` 和 `case.out` 到 `runs/regression_g10/<run_id>`，其他字段原样复制。输出：`configs/generated/regression_g10-manifest.json`。

`scripts/run_suite.py` 增加：

- `--results-dir`，默认 `results/g8`；
- `--runs-dir`，默认 `runs/paper`（补充参数，避免回归运行时覆盖冻结的 `runs/paper/run_manifest.json`；默认行为不变）。

### 7. 回归执行与比较

```bash
.venv/bin/python scripts/run_suite.py \
  --manifest configs/generated/regression_g10-manifest.json \
  --results-dir results/regression_g10 \
  --runs-dir runs/regression_g10 \
  --jobs 8
# exit 0；240/240 SUCCESS；8/8 runner checks PASS
# 日志 logs/g10_regression_run.log

.venv/bin/python scripts/compare_regression.py
# exit 0；REGRESSION=PASS；compare.csv 仅 header（0 差异）
```

比较口径：`metrics.json` 忽略 `wall_seconds`、`paired_ext_off_wall_seconds`、`wall_overhead_fraction` 和任何 `_utc` 键后逐键相等；`output.bin`、`reports.csv` 两边存在性一致且 SHA256 相等。

结果：

```text
manifest_total=240 executed=240
SUCCESS=240 FAIL=0 TIMEOUT=0 EXPECTED_NEGATIVE=0
REGRESSION=PASS
frozen_verified=33868/33868
```

### 8. 预注册协议

`docs/g10_supplement_protocol.md` 按补充手册 §4、§5 整理 P1-G/P1-H 的位模式、G11/G12 要求、结果分类、矩阵、统计口径和 H-G1..H-G3、H-H1..H-H4；明确 scrub 与 codeword 粒度为 ASSUMED。文件 SHA256：

```text
8758ead33623453f9eea18cf7d8dd328a97eb19dc07470e9e8a39988ee3e8247  docs/g10_supplement_protocol.md
```

协议在 G14 后不得修改；偏离只能写入 `docs/g16_supplement_results.md`。

### 9. 通过/失败检查

通过：

- Python 导入、pip check、freeze 与 lock 逐行一致；
- build.sh 固定提交检查与构建，二进制与 G8 相同；
- `.git` 重建的 commit/tree/blob 哈希全部核对；
- 240/240 回归 SUCCESS；
- 0 FAIL、0 TIMEOUT；
- REGRESSION=PASS（0 差异）；
- 33,868/33,868 冻结文件哈希一致。

失败并已修正：

1. 初次 `apt-get install` exit 100：security 源中 python3.10 包 404；`apt-get update` 后 exit 0。
2. 初次 venv 安装被用户中断：重跑 `pip install -r` 后 exit 0。
3. `make_regression_manifest.py` 初次 exit 1：P1-C profile 字段位置判断错误；改用 `params.profile` 后 exit 0。
4. 初次串行 frozen hash 超时：改为 16 路并行后 6 s 完成。
5. GitHub HTTPS fetch 不可用（TLS/连接超时）：改用 `api.github.com` + `raw.githubusercontent.com` 逐对象重建；所有对象哈希均与 GitHub 返回值核对。

未发生：未删除测试、未修改预期、未更换位模式、未让 runtime 读取真值、未伪造数据、未把未执行命令写成已完成。

### 10. 产物位置

```text
docs/g10_supplement_protocol.md
configs/generated/regression_g10-manifest.json
runs/regression_g10/<run_id>/
results/regression_g10/run_metrics.csv
results/regression_g10/summary.csv
results/regression_g10/validation.json
results/regression_g10/compare.csv
results/regression_g10/compare_summary.txt
results/regression_g10/frozen_sha256.txt
logs/g10_*.log
```

### 11. 下一关唯一任务

**G11：ECC 漏检位模式验证（Python + C++）。** 按补充手册 §4.1/§4.2，只新增 `tests/test_ecc_reference.py::test_escape_patterns` 和 `simulator/src/tests/Phase1EccTestCases.cpp::TEST(Phase1Ecc, EscapePatterns)`，运行 Python 与 C++ ECC 测试；不得修改 `Phase1Ecc.cpp`，不得更换位模式，不实现 fixture、scrub、codeword 粒度或矩阵。

## G11 — ECC 漏检位模式验证

执行日期：2026-11-16（UTC）。执行对象：`PHASE1_SUPPLEMENT_HANDOFF.md` §4.1/§4.2。工作目录：`pim-phase1/`。

### 1. 实际改动的文件

修改/新增测试：

```text
tests/test_ecc_reference.py                       # 新增 test_escape_patterns（8 个 PATTERNS 参数化）
simulator/src/tests/Phase1EccTestCases.cpp        # 新增 TEST(Phase1Ecc, EscapePatterns)
```

未修改：

```text
simulator/src/Phase1Ecc.cpp                       # SHA256 2e5f9fa3c688a295a8091818c3b7b8cf4b7cb0a8d96512d29b8ddc8804c444a7
simulator/src/Phase1Ecc.h
任何 runtime / storage / reports 代码
```

构建脚本重写（非冻结文件）：

```text
environment/binary.json
environment/binary.sha256
environment/build_flags.txt
runs/baseline/binary.json
runs/baseline/binary.sha256
logs/build_20261116T101343Z.log
```

G11 未生成独立的 `environment/source_g11.patch`，也未更新累计 `environment/source.patch`；按补充手册 §9，G16 才生成 `environment/source_g16.patch` 并统一更新累计补丁。当前累计补丁仍为 G8 的 `93daf08c...`。

### 2. Python 验证

新增 `test_escape_patterns` 使用 `tests/test_ecc_reference.py` 已有的独立 72-bit 参考实现（`encode`/`decode`/`codeword`/`from_codeword`），不逐行翻译 C++。对 `PATTERNS` 的 8 个数据字逐一验证：

- F1：数据位 {15} → `CORRECTED`，返回原值，syndrome=21，overall=1；
- F3M：数据位 {0,1,4} → `CORRECTED`，返回原值⊕{0,1,4,10}，syndrome=15，overall=1；
- F4S：数据位 {0,1,4,10} → `CLEAN`，返回同一坏值，syndrome=0，overall=0；
- S4 第二位：{15} 后翻 {14} → `UNCORRECTABLE`，syndrome=1，overall=0；
- cw1：断言 `79//64==1`、`79%64==15`，单比特 → `CORRECTED`，返回原值。

同时断言数据位→codeword 位置映射：0→3、1→5、4→9、10→15、14→20、15→21；并显式断言 F3M 的 corrected data 与 F4S 的返回数据逐值相同。

```bash
.venv/bin/python -m pytest tests/test_ecc_reference.py -q
# exit 0
# 25 passed in 1.79s
```

日志：`logs/g11_ecc_python.log`（含原有 clean/single/double/crosscheck 测试）。

### 3. C++ 验证

新增 `TEST(Phase1Ecc, EscapePatterns)`，使用 `Phase1Ecc::encodeParity/decode`、`dataBitPositions()`、`hammingParityPositions()` 和文件已有的 `codewordFrom`/`fromCodeword` 辅助函数，对同一组 8 个 `patterns()` 逐一断言：

- F1、F3M、F4S、S4、cw1 的状态、返回数据、syndrome、overall parity；
- F3M 与 F4S 返回数据相同；
- 数据位→位置映射与 `bitInBurst=79` 的 codeword 1 / data bit 15 解释。

```bash
bash scripts/build.sh
# exit 0；新二进制 SHA256 493335a136c533cda34c7054f30ba7f1e9b2f4b1ea9126e0a40bec0f4e342f99
cd simulator
./sim --gtest_filter='Phase1Ecc.*'
# exit 0
# [==========] 2 tests from 1 test suite ran.
# [  PASSED  ] 2 tests.
```

日志：`logs/g11_build.log`、`logs/g11_ecc_cpp.log`。`Phase1Ecc.ExhaustiveSingleAndDoubleBit` 仍为 17 ms 通过，未删除或放宽既有测试。

### 4. 通过/失败检查

通过：

- Python：25 passed；新增 8 个 escape pattern 全部符合 §4.1；
- C++：`Phase1Ecc.*` 2/2 通过；`EscapePatterns` 0 ms；
- F3M 与 F4S 坏数据完全相同；
- S4 为 UE；
- cw1 映射正确；
- `Phase1Ecc.cpp` 未修改；
- 构建 exit 0。

失败：无。

未发生：未修改 ECC 实现、未更换位模式、未删除测试、未修改预期、未伪造数据、未执行 G12 或后续关卡。

### 5. 产物位置

```text
tests/test_ecc_reference.py
simulator/src/tests/Phase1EccTestCases.cpp
logs/g11_ecc_python.log
logs/g11_ecc_cpp.log
logs/g11_build.log
environment/binary.sha256
```

### 6. 下一关唯一任务

**G12：P1-G fixture + smoke。** 按补充手册 §4.3/§4.4，复制 `Phase1G8P1CTestCases.cpp` 为 `Phase1G10EscapeTestCases.cpp`，实现 F3M/F4S/F1W_F4S 注入、只读评估器、`escape_outcome` 分类和 `contract_pass`；运行 24 个 smoke cases 并全部 contract_pass。不得实现 scrub、codeword 粒度、完整 4320 矩阵或 P1-H。

## G12 — P1-G fixture 与 24-case smoke

执行日期：2026-11-16（UTC）。执行对象：`PHASE1_SUPPLEMENT_HANDOFF.md` §4.3/§4.4。工作目录：`pim-phase1/`。

进入 G12 前复核 G10/G11：G10 的 33,868 个冻结文件全部哈希一致，240-case 回归 `REGRESSION=PASS`；G11 的 Python 25 passed、C++ `Phase1Ecc.*` 2/2 passed；G11 的 `Phase1Ecc.cpp` 仍为 `2e5f9fa3...`。复核通过后才修改 G12 fixture。

### 1. 实际改动的文件

新增：

```text
simulator/src/tests/Phase1G10EscapeTestCases.cpp   # TEST(Phase1G10EscapeFixture, RunCase)
runs/supplement/g12_smoke/manifest.json
runs/supplement/g12_smoke/<24 run_id>/
results/supplement/g12_smoke_summary.json
results/supplement/g12_smoke_run_metrics.csv
logs/g12_build.log
logs/g12_smoke.log
logs/g12_validate.log
```

修改：

```text
docs/STATUS.md
docs/implementation_log.md
```

构建脚本重写（非冻结文件）：

```text
environment/binary.json
environment/binary.sha256
environment/build_flags.txt
runs/baseline/binary.json
runs/baseline/binary.sha256
logs/build_20261116T101846Z.log   # 首次失败（文件截断）
logs/build_20261116T101906Z.log
logs/build_20261116T101953Z.log
logs/build_20261116T102019Z.log   # 最终成功
```

未修改：`Phase1Ecc.cpp`（仍 `2e5f9fa3...`）、`Bank.*`、`Phase1Reports.*`、既有 P1-C fixture、paper 运行与冻结文件。

G12 未生成独立补丁快照；累计 `environment/source.patch` 仍为 G8 的 `93daf08c...`，按补充手册 §9 在 G16 统一更新。

### 2. fixture 实现

`Phase1G10EscapeTestCases.cpp` 从 P1-C fixture 复制，保留 preload → 注入 → `executeEltwise` ADD → `runProducer` → hold → consumer 主流程，新增：

- 参数：`fault_kind ∈ {F3M,F4S,F1W_F4S}`、`escape_placement ∈ {NONE,SAME_BANK,DIFF_BANK}`、`target_burst`；F3M/F4S 强制 NONE，F1W 强制 SAME/DIFF；
- F3M：target burst 翻 {0,1,4}；
- F4S：target burst 翻 {0,1,4,10}；
- F1W_F4S：target burst 翻 witness {15}；从 `target_burst+1` 起逐个递增取第一个满足 bank 条件的 `corrupt_burst`（回绕到 0），翻 {0,1,4,10}；断言 `corrupt_burst != witness_burst`；
- 每次注入写一行 INJECTION 到 `truth.csv`，含 `storage_key`、`bit_positions`、`raw_before/raw_after`、`target_access_id=A_burst_<b>`、实际 `decoder_status`；
- 测试专用 `RecordingSink` 转发检测到 `Phase1Reports`，用于验证 F1W 的 corrupt_burst 不产生检测事件；runtime 不持有 truth/golden/注入位置引用；
- 只读评估器：
  - `corruption_propagates`：A′ = A⊕{0,1,4,10}，重算 B′=A′+X、C′=2·B′，与无故障期望逐字节比较；
  - `held_output_corrupt`：仅未发布时，从 `codec.encode({0,0,0,256,0}) + corrupt_burst*32` 解码 B 位置，读取 4 个 codeword 的 raw data 拼 32 B 与 `expectedB[corrupt_burst]` 比较；任一 key 不存在为 null；
  - `report_region_ids`、`report_covers_corrupt_bank`：bank 粒度公式照抄 `Phase1Reports.cpp`（`channel*num_banks + actual_bank`）；
- `escape_outcome` 按 §4.3 顺序判定；`contract_pass` 按 §4.3 全部条件；
- metrics 保留 P1-C 字段，新增 `fault_kind, escape_placement, witness_burst, corrupt_burst, witness_bank, corrupt_bank, corruption_propagates, held_output_corrupt, escape_outcome, report_region_ids, report_covers_corrupt_bank, internal_ce, internal_ue`；其中 `held_output_corrupt` 输出 true/false/null。

### 3. smoke 生成与执行

smoke 参数：n_tiles=1、consumer=HOST、profile=LOCAL_VISIBLE、seed=0..2、四种 fault 配置（F3M/NONE、F4S/NONE、F1W/SAME_BANK、F1W/DIFF_BANK）、D/T_ref∈{0,1}、H/T_ref=0、T_ref=731、L=8、I=1、Q=64、watchdog=400000。目标 burst 用 `target_burst_for(seed,1)`：seed 0→35、1→62、2→121。

临时 smoke runner（`/tmp/opencode/run_g12_smoke.py`，非交付脚本；G14 由 `expand_supplement.py`/`run_suite.py` 正式化）生成 24 个 params.ini，逐 case 独立进程运行：

```bash
.venv/bin/python /tmp/opencode/run_g12_smoke.py
# exit 0；G12_SMOKE=PASS pass=24/24
```

结果：

```text
contract_pass=24/24
PREVENTED=8  CORRUPT_CONSUMED=14  MASKED=2
UNCLASSIFIED=0  INVALID_CASE=0  TIMEOUT=0
```

分组：

- F3M：D=0 的 3 个全部 PREVENTED（held_output_corrupt=true）；D=731 的 3 个全部 CORRUPT_CONSUMED；truth decoder_status=CORRECTED。
- F4S：6/6 CORRUPT_CONSUMED；detected_events=0、delivered_reports=0、internal_ue=0；truth decoder_status=CLEAN。
- F1W_F4S：5 PREVENTED、5 CORRUPT_CONSUMED、2 MASKED；witness 均产生 CE，corrupt_burst 无检测；SAME_BANK 的 `report_covers_corrupt_bank=true`，DIFF_BANK 全为 false。
- smoke 中 seed=1 的 SAME_BANK 选择 corrupt_burst=78，DIFF_BANK 选择 63；不同 A 值导致前者被 FP16 舍入掩蔽，后者传播。因此 H-G3 的“SAME/DIFF 结果相同”在 smoke 子集内未完全复现；这是预注册假设的观察，未调参，完整判定留给 G14/G16。

### 4. 独立复核

`logs/g12_validate.log` 从保存文件重新计算，不依赖 fixture 结论：

```text
G12_INDEPENDENT PASS bad=0
```

检查项：reports 行数 = delivered_reports；truth 注入行数（F3M/F4S=1、F1W=2）；published 时 held_output_corrupt=null；PREVENTED 条件；CORRUPT_CONSUMED 条件；MASKED 条件；F4S 无报告/无 UE；report_region_ids 与 reports.csv 一致；F3M/F4S/F1W truth decoder_status；DIFF_BANK 不覆盖 corrupt bank。

### 5. 命令与退出码

```bash
bash scripts/build.sh
# 第一次 exit 2：新文件截断在 runConsumer 内部（拼装错误）
# 修正后 exit 0；最终二进制 SHA256 1c2d3fc69e2bf89330bc71155421d182c9fa059b8b482fe9452233e4d9108aea

.venv/bin/python /tmp/opencode/run_g12_smoke.py
# 第一次 exit 1：18/24，F3M 全部 INVALID_CASE
# 修正后 exit 0：24/24

.venv/bin/python - <<'PY' ...独立复核...
# exit 0
```

### 6. 失败与修正

1. 初始文件拼装用 `head -n 190`，截断在 `runConsumer` 的 `while` 内，`TEST` 被当作局部类，build exit 2。修正为保留完整 `runConsumer` 后再接新 helper/test；build exit 0。
2. F3M 首轮全部 `INVALID_CASE`：评估器把原始注入掩码当成解码后数据掩码。F3M 的 raw 翻位是 {0,1,4}（0x13），解码器再翻 data bit 10，得到坏数据掩码 {0,1,4,10}（0x413）。修正为 `kEscapeRawMask`（raw）与 `kEscapeMask`（decoded data）分开；重跑 24/24。
3. `held_output_corrupt` 初版写 1/0，独立复核按布尔检查失败；改为 true/false/null 后重新构建与重跑，独立复核 0 不一致。
4. 未修改 ECC 实现、未更换位模式、未修改预期、未删除测试、未让 runtime 读取 truth。

### 7. 产物位置

```text
simulator/src/tests/Phase1G10EscapeTestCases.cpp
runs/supplement/g12_smoke/manifest.json
runs/supplement/g12_smoke/<run_id>/{params.ini,config.json,truth.csv,reports.csv,metrics.json,output.bin,stdout.log,stderr.log,exit_code}
results/supplement/g12_smoke_summary.json
results/supplement/g12_smoke_run_metrics.csv
logs/g12_build.log
logs/g12_smoke.log
logs/g12_validate.log
environment/binary.sha256
```

### 8. 下一关唯一任务

**G13：scrub/codeword 接口 + P1-H smoke + 回归。** 按补充手册 §5.1–§5.3，在 `Bank` 增加默认关闭的理想化 scrub 开关与计数，在 `Phase1Reports` 增加 codeword 粒度（ASSUMED，公式写入注释/协议），新建 `Phase1G11SequenceTestCases.cpp` 实现 S1–S6/P/Q/R/scrub，运行 seed=0 的 33 个 P1-H smoke，并在改代码后立即跑 G10 regression 流程，必须 `REGRESSION=PASS`。不得提前实现 G14 全矩阵、聚合、图或报告。

## G13 — scrub/codeword 接口、P1-H smoke 与回归

执行日期：2026-11-16（UTC）。执行对象：`PHASE1_SUPPLEMENT_HANDOFF.md` §5.1–§5.3。工作目录：`pim-phase1/`。

### 1. 实际改动的文件

C++：

```text
simulator/src/Bank.h
simulator/src/Bank.cpp
simulator/src/Phase1Reports.cpp
simulator/src/tests/Phase1G11SequenceTestCases.cpp   # 新增 TEST(Phase1G11SequenceFixture, RunCase)
```

脚本（新增可选参数，默认行为不变）：

```text
scripts/make_regression_manifest.py   # --out-manifest / --run-root
scripts/compare_regression.py         # --manifest / --out-dir
```

文档/协议：

```text
docs/g10_supplement_protocol.md       # 追加 codeword region_id 公式（ASSUMED）
docs/STATUS.md
docs/implementation_log.md
```

新增产物：

```text
configs/generated/regression_g13-manifest.json
runs/regression_g13/<240 run_id>/
results/regression_g13/{run_metrics.csv,summary.csv,validation.json,compare.csv,compare_summary.txt}
runs/supplement/g13_smoke/manifest.json
runs/supplement/g13_smoke/<33 run_id>/
results/supplement/g13_smoke_summary.json
results/supplement/g13_smoke_run_metrics.csv
logs/g13_build.log
logs/g13_smoke.log
logs/g13_validate.log
logs/g13_regression_run.log
logs/g13_regression_compare.log
```

构建脚本重写（非冻结文件）：

```text
environment/binary.json
environment/binary.sha256
environment/build_flags.txt
runs/baseline/binary.json
runs/baseline/binary.sha256
logs/build_20261116T102517Z.log
logs/build_20261116T102600Z.log
```

未修改：`Phase1Ecc.*`、P1-C/P1-G fixture、paper 运行/结果/图/artifact；`environment/source.patch` 仍为 `93daf08c...`，G16 统一更新。

### 2. scrub 实现（ASSUMED）

`Bank` 新增：

```text
void setPhase1Scrub(bool enabled);
uint64_t phase1ScrubWrites() const;
```

成员默认 `false`、计数 0。`Bank::read` 的受保护 ECC 路径中，仅当：

```text
result.status == CORRECTED && phase1Scrub_ == true
```

调用 `phase1Storage_.writeLegal(key, result.data)` 并递增计数。该写是模型内直接 codeword 重写，不产生总线事务、不增加周期；开关为 false 时不写、不计数，默认输出/报告/周期不变。

### 3. codeword 粒度（ASSUMED）

`Phase1Reports` 构造函数接受 `"codeword"`；bank/page 分支保持原样。codeword 分支：

```text
region_type = "codeword"
region_id = (((channel * num_banks + actual_bank) * num_rows + row)
             * burst_columns + burst_column) * 4 + codeword_index
```

公式写入 `Phase1Reports.cpp` 注释和 `docs/g10_supplement_protocol.md`。该接口只定义模型公开报告标识，不提供位位置或 syndrome，不代表真实 HBM 报告格式。

### 4. P1-H fixture

`Phase1G11SequenceTestCases.cpp` 只用主机读写路径，复用 G4 的 `memory.addTransaction`/`addBarrier` 写法：

- P=(bank_p,row 0,col 0)、Q=(bank_p,row 1,col 0)、R=(bank_r,row 0,col 0)；codec encode/decode round-trip 断言；
- 三位置先合法写入确定性 4-codeword 模式；
- 4 rounds，每轮先执行预定注入，再按 P、Q、R 固定顺序各读一次，每次读到 idle 且报告队列清空；
- S1–S6 注入表严格按 §5.2；S6 仅 scrub=1，r1 注入前断言 P 的 cw0 data bit 15 已恢复为初始值；
- 测试专用 `RecordingSink` 转发 detection 到 `Phase1Reports`；runtime 不存在，报告只来自真实检测；
- `truth.csv` 写 INJECTION 与 DETECTION 行；`reports.csv` 只含公开字段；`metrics.json` 含 scenario/granularity/scrub/seed/bank_p/bank_r/rounds/injections/detected_events/delivered_reports/internal_ce/internal_ue/scrub_writes/report_signature/contract_pass/timeout；
- `report_signature` 按 report_seq 连接 `error_class:region_id`，不含 report_cycle。

### 5. smoke 参数与结果

seed=0；`bank_p=13`、`bank_r=7`（PCG64(0)：`rng.integers(0,16)=13`，`(13+1+rng.integers(0,15))%16=7`）。33 cases = 3 粒度 ×（S1–S5 各 scrub=0/1 + S6 scrub=1）。

```bash
.venv/bin/python /tmp/opencode/run_g13_smoke.py
# 第一次 exit 1：27/33，S1_SINGLE 被错误要求 2 次注入
# 修正 S1 期望注入数为 1 后 exit 0；G13_SMOKE=PASS pass=33/33
```

结果：

```text
contract_pass=33/33
注入数：S1=1，S2–S6=2
scrub_writes：scrub=0 全 0；scrub=1 时 S1=1，S2–S6=2
```

独立复核（`logs/g13_validate.log`）：reports/truth 行数、report_signature、scrub_writes、bank/codeword region_id 公式全部一致，`G13_INDEPENDENT PASS bad=0`。

### 6. smoke 观察（不是完整统计）

- bank、scrub=1：S2/S3/S4/S6 的 signature 均为 `CE:13;CE:13`；在 smoke 子集内支持 H-H1。
- codeword、scrub=1：S4/S6 均为 `CE:27262976;CE:27262976`；S2 为 `CE:27262976;CE:27262977`，S3 为 `CE:27262976;CE:27263104`；支持 H-H2。
- scrub=0：S4 在 bank/page/codeword 三种粒度均出现 UE；支持 H-H3。
- bank、scrub=1：S2/S3/S4/S6 都有同一 region 收到 2 份 CE，CoF 式规则同样触发；S1 只有 1 份，S5 为两个不同 region；支持 H-H4。
- 以上仅 seed 0 smoke；完整 10-seed 判定留给 G14/G15。

### 7. 回归

为保留 G10 改代码前证据，G13 使用独立 manifest/目录，不覆盖 G10：

```bash
.venv/bin/python scripts/make_regression_manifest.py \
  --out-manifest configs/generated/regression_g13-manifest.json \
  --run-root runs/regression_g13
# exit 0；240 cases

.venv/bin/python scripts/run_suite.py \
  --manifest configs/generated/regression_g13-manifest.json \
  --results-dir results/regression_g13 \
  --runs-dir runs/regression_g13 --jobs 8
# exit 0；240/240 SUCCESS；8/8 checks

.venv/bin/python scripts/compare_regression.py \
  --manifest configs/generated/regression_g13-manifest.json \
  --out-dir results/regression_g13
# exit 0；REGRESSION=PASS；compare.csv 仅 header
```

默认参数未变：不带参数时 `make_regression_manifest.py`/`compare_regression.py` 仍指向 G10；G10 的 240-case 结果与冻结文件未被覆盖。G10 冻结哈希复核 33,868/33,868 一致。

### 8. 命令与退出码

```bash
bash scripts/build.sh
# 最终 exit 0；二进制 SHA256 1662ad0522736cdf0bedb5eeeb58c5029efbf128c64dd5112187b748a5c2cda1

.venv/bin/python /tmp/opencode/run_g13_smoke.py
# 第一次 exit 1（27/33）；修正后 exit 0（33/33）

.venv/bin/python -m py_compile scripts/make_regression_manifest.py scripts/compare_regression.py scripts/run_suite.py
# exit 0

.venv/bin/python scripts/make_regression_manifest.py --out-manifest ... --run-root runs/regression_g13
# exit 0
.venv/bin/python scripts/run_suite.py --manifest ... --results-dir results/regression_g13 --runs-dir runs/regression_g13 --jobs 8
# exit 0
.venv/bin/python scripts/compare_regression.py --manifest ... --out-dir results/regression_g13
# exit 0；REGRESSION=PASS
```

### 9. 失败与修正

1. 首次 G13 smoke 27/33：S1_SINGLE 只调度 1 次注入，fixture 与 smoke runner 都错误要求 2 次。修正为 S1=1、其余=2；重跑 33/33。
2. truth INJECTION 行在初版中多了一个逗号（decoder_status 列错位）；在首次构建前已修正为与 P1-C 相同的 14 列。
3. 未修改 ECC、未放宽 contract、未更换位模式、未伪造报告。

### 10. 产物位置

```text
simulator/src/Bank.h, simulator/src/Bank.cpp
simulator/src/Phase1Reports.cpp
simulator/src/tests/Phase1G11SequenceTestCases.cpp
docs/g10_supplement_protocol.md
runs/supplement/g13_smoke/manifest.json
runs/supplement/g13_smoke/<run_id>/
results/supplement/g13_smoke_summary.json
results/supplement/g13_smoke_run_metrics.csv
configs/generated/regression_g13-manifest.json
runs/regression_g13/<run_id>/
results/regression_g13/
logs/g13_*.log
logs/build_20261116T102517Z.log
logs/build_20261116T102600Z.log
```

### 11. 下一关唯一任务

**G14：展开并运行补充矩阵。** 新建 `configs/suite_supplement.yaml`、`scripts/expand_supplement.py`，在 `scripts/run_suite.py::validate_case` 增加 `validate_p1g`/`validate_p1h`，运行全量 P1-G 4320 + P1-H 330（smoke 24/33），manifest 对账必须满足总数 = SUCCESS + FAIL + TIMEOUT + MISSING；失败单列不删除。G14 不实现聚合、图或报告修改。

## G14 — 补充矩阵展开与完整执行

执行日期：2026-11-16（UTC）。执行对象：`PHASE1_SUPPLEMENT_HANDOFF.md` §4.4/§5.3/§6。工作目录：`pim-phase1/`。

### 1. 实际改动的文件

新增：

```text
configs/suite_supplement.yaml
scripts/expand_supplement.py
configs/generated/supplement-manifest.json
configs/generated/supplement-index.csv
configs/generated/supplement/<4650 run_id>/config.json
runs/supplement/<4650 run_id>/
results/supplement/run_metrics.csv
results/supplement/summary.csv
results/supplement/validation.json
results/supplement/failure_cases.csv
logs/g14_expand.log
logs/g14_run_full.log
logs/g14_validate.log
```

修改：

```text
scripts/run_suite.py                  # 新增 validate_p1g / validate_p1h 与 P1-G/P1-H 分支
docs/STATUS.md
docs/implementation_log.md
```

未修改：任何 C++ 源码/测试、paper 运行/结果/图/artifact、冻结文件。G14 的二进制仍为 G13 的 `1662ad05...`；`environment/source.patch` 仍为 `93daf08c...`，G16 统一更新。

### 2. suite 与 expander

`configs/suite_supplement.yaml`：

- P1G：profile LOCAL_VISIBLE、粒度 bank、n_tiles 1/8、consumer HOST/PIM、seed 0..29、四种 fault 配置、D/T_ref 0/0.5/1、H/T_ref 0/0.25/1、L=8、I=1、Q=64、watchdog=400000；
- P1H：seed 0..9、granularity bank/page/codeword、S1–S6、S6 仅 scrub=1、rounds=4、D=0、I=1、Q=64、watchdog=200000；
- 顶层 `smoke: false`；`--smoke` 覆盖为 P1-G 24、P1-H 33。

`scripts/expand_supplement.py` 使用 `target_burst_for`、`base_params` 和与 paper 相同的 hash-addressed `add_case`，但 `RUN_ROOT=runs/supplement`、`CONFIG_ROOT=configs/generated/supplement`、manifest 写 `configs/generated/supplement-manifest.json`。对未知/缺失 key 报错。

```bash
.venv/bin/python scripts/expand_supplement.py --suite configs/suite_supplement.yaml
# exit 0
# expanded 4650 cases (full)
# P1-G Escape 4320; P1-H Sequence 330
# G14_EXPAND=PASS

.venv/bin/python scripts/expand_supplement.py --suite configs/suite_supplement.yaml --smoke
# exit 0; 24 + 33 = 57
```

全量 manifest：`expected_total=4650`，4650 个唯一 run_id；SHA256 `781e560ef5965a7e8d1ebf377525e39aaa23b9491ab1b229b434de03abc47d63`。

### 3. run_suite 验证分支

`scripts/run_suite.py` 新增：

- `validate_p1g`：读 truth/reports/metrics，检查注入数（F3M/F4S=1、F1W=2）、report 行数、delivered/detected、internal_ue、`report_region_ids`、`report_covers_corrupt_bank`、published/consumed、output_changed、held_output、output.bin 大小；按 fault 检查 F3M truth decoder=CORRECTED、F4S CLEAN 且 0 报告/检测、F1W witness CORRECTED + corrupt CLEAN 且 burst 不同；从 published/consumed/output_changed/corruption_propagates/lock/held 复算 `escape_outcome`；
- `validate_p1h`：读 truth/reports/metrics，检查注入数（S1=1、其余=2）、detection/report 行数、delivered=detected、internal_ce+ue=detected、report_signature、region_type、bank/codeword region_id 公式、scrub_writes（scrub=0 全 0；scrub=1 时 S1=1、其余=2）、rounds=4、S6 scrub=1。

未改其他 group 的验证逻辑；默认路径仍为 G8 paper 行为。

### 4. 全量运行

```bash
.venv/bin/python scripts/run_suite.py \
  --manifest configs/generated/supplement-manifest.json \
  --results-dir results/supplement \
  --runs-dir runs/supplement \
  --jobs 8 --resume
# exit 0
# 4650/4650 SUCCESS；8/8 runner checks PASS
```

日志：`logs/g14_run_full.log`；运行区间 2026-11-16T10:34:39Z–10:37:16Z。

对账：

```text
manifest_total = 4650
SUCCESS = 4650
EXPECTED_NEGATIVE = 0
FAIL = 0
TIMEOUT = 0
MISSING = 0
P1-G Escape: 4320/4320 contract_pass
P1-H Sequence: 330/330 contract_pass
```

`results/supplement/failure_cases.csv` 只有表头，未删除任何 case。

### 5. 独立复核

`/tmp/opencode/validate_g14.py` 逐 case 重新调用 `validate_p1g`/`validate_p1h`：

```text
manifest_total=4650 groups={'P1-G Escape': 4320, 'P1-H Sequence': 330}
missing=0 failed=0
runner_validation=True counts={'SUCCESS': 4650, ...}
checks_failed={}
G14_VALIDATE=PASS
```

日志：`logs/g14_validate.log`。

### 6. 结果分布（仅对账摘要，不做 G15 统计）

P1-G 4320：

```text
PREVENTED=2166  CORRUPT_CONSUMED=1920  MASKED=234
F3M   (1080): PREVENTED=706  CORRUPT_CONSUMED=302  MASKED=72
F4S   (1080): CORRUPT_CONSUMED=1008  MASKED=72
F1W   (2160): PREVENTED=1460 CORRUPT_CONSUMED=610  MASKED=90
```

P1-H 330：S1/S2/S3/S4/S5 各 60、S6 30（覆盖 3 粒度与 scrub 组合）。

以上不是 G15 的置信区间或图数据；G15 才做分母、Wilson/bootstrap、不可区分对和 CoF 统计。

### 7. 回归与冻结检查

```bash
.venv/bin/python scripts/compare_regression.py
# exit 0；REGRESSION=PASS（G10 240 cases）
.venv/bin/python scripts/compare_regression.py \
  --manifest configs/generated/regression_g13-manifest.json \
  --out-dir results/regression_g13
# exit 0；REGRESSION=PASS（G13 240 cases）
```

G10 冻结文件复核：33,868/33,868 一致。G14 没有修改 G10/G13 的回归目录内容（只读取）。

### 8. 边界与说明

- 全量补充矩阵是 4650 个独立进程/目录；运行器 metadata 的 `gate` 字段沿用 runner 的 G8 名称，本关以 supplement manifest、`results/supplement/validation.json` 和独立复核为准。
- G14 未实现 `aggregate_supplement.py`、figure_data、统计表、图、captions 或报告修改；这些属于 G15/G16。
- G14 未生成独立补丁快照；累计 `environment/source.patch` 仍为 G8 的 `93daf08c...`，G16 统一生成 `source_g16.patch` 并更新。
- 全量运行没有失败/超时/缺失；不能把“0 失败”写成对真实 HBM 的零漏报保证。

### 9. 产物位置

```text
configs/suite_supplement.yaml
configs/generated/supplement-manifest.json
configs/generated/supplement/<run_id>/config.json
runs/supplement/<run_id>/
results/supplement/run_metrics.csv
results/supplement/summary.csv
results/supplement/validation.json
results/supplement/failure_cases.csv
logs/g14_expand.log
logs/g14_run_full.log
logs/g14_validate.log
```

### 10. 下一关唯一任务

**G15：聚合、统计、作图。** 新建 `scripts/aggregate_supplement.py` 和 `scripts/plot_supplement.py`，生成 §6/§7 要求的 figure_data CSV、`statistics_supplement.csv`、Fig. 7/8/S2 与 plot manifest；执行 manifest 对账，失败单列；检查分母 n−MASKED、N/A、图例遮挡和灰度。G15 不修改报告、captions 或最终结论。

## G15 — 补充实验聚合、统计与作图

执行日期：2026-11-16（UTC）。执行对象：`PHASE1_SUPPLEMENT_HANDOFF.md` §6/§7。工作目录：`pim-phase1/`。

### 1. 实际改动的文件

新增：

```text
scripts/aggregate_supplement.py
scripts/plot_supplement.py
results/supplement/figure_data/fig7_escape.csv
results/supplement/figure_data/fig8_signatures.csv
results/supplement/figure_data/fig8_pairs.csv
results/supplement/figure_data/fig8_cof_rule.csv
results/supplement/statistics_supplement.csv
results/supplement/reconciliation_supplement.json
figures/fig07_escape_efficacy.{pdf,svg,png}
figures/fig08_report_ambiguity.{pdf,svg,png}
figures/figS2_escape_by_config.{pdf,svg,png}
figures/gray/fig07_escape_efficacy.png
figures/gray/fig08_report_ambiguity.png
figures/gray/figS2_escape_by_config.png
figures/plot_manifest_supplement.json
logs/g15_aggregate.log
logs/g15_plot.log
logs/g15_validate.log
```

修改：

```text
docs/STATUS.md
docs/implementation_log.md
```

未修改：任何 C++ 源码/测试、`report/phase1_results.md/.tex`、paper 数据/图/artifact、冻结文件。`figures/captions.md` 已追加三张补充图的条目（用户可读性要求）。二进制仍为 `1662ad05...`。

### 2. 聚合与统计

`scripts/aggregate_supplement.py` 只读 `runs/supplement` 的 metrics/reports 与 supplement manifest：

```bash
.venv/bin/python scripts/aggregate_supplement.py
# exit 0
# P1G rows=4320 P1H rows=330 stats=432
# G15_AGGREGATE=PASS
```

输出：

- `fig7_escape.csv`：4320 行；每 case 的 fault/placement/D/H、outcome、escape_outcome、corruption/held/published/consumed 等；
- `fig8_signatures.csv`：330 行；含 `report_signature` 与 `report_signature_with_timing`（相对第一份报告的 cycle 差值）；
- `fig8_pairs.csv`：90 行 = 3 粒度 × 2 scrub × 15 场景对；不可区分 seed 数、n_seeds、timing 不可区分数；scrub=0 的 S6 对为 N/A；
- `fig8_cof_rule.csv`：36 行；每个 (granularity,scrub,scenario) 的 CoF fire seeds、UE seeds、n_seeds；
- `statistics_supplement.csv`：432 行 = 144 格 × {PREVENTED_fraction, CORRUPT_CONSUMED_fraction, MASKED_fraction}；Wilson 95% CI；PREVENTED/CORRUPT 分母 `n−MASKED`，MASKED 分母 total；另列 `masked`、`total`；
- F3M PREVENTED 行增加 `fig3_timely_fraction/n/low/high` 与 `prevented_minus_timely`；36 个 F3M 格子中 8 个与 fig3 同格 TIMELY 不一致（多为 MASKED 改变分母）；
- `reconciliation_supplement.json`：manifest 4650 = 4320 + 330，missing=0，runner counts 4650 SUCCESS。

### 3. 图

`scripts/plot_supplement.py` 只读 `results/supplement/figure_data/*.csv`：

```bash
.venv/bin/python scripts/plot_supplement.py
# exit 0
# wrote 3 supplement figures
# G15_PLOT=PASS
```

- Fig.7：1×4（F3M、F1W SAME、F1W DIFF、F4S），n_tiles=8 HOST；x=H/T_ref，y=D/T_ref（0 在上，1 在下）；颜色=CORRUPT_CONSUMED/(n−MASKED)，0–1 白→深红；面板副标题给出 `n'` 和 `M`；格内只显示 `C <count>` / `P <count>`，不再重复分母和每格 `m`；底部一行说明 C/P、n′ 及 F3M/F4S 同坏数据。
- Fig. S2：4×4（n_tiles×consumer × fault）；只在最左列/最下行显示 tick labels，D/T_ref 为共享轴标题；每面板副标题给 `n'`/`M`，格内 C/P 计数；样本 4320。
- Fig.8：2×3（scrub × granularity），只画非对角场景对；零值留空，对角省略，S6/scrub=0 斜线；颜色=不可区分 seed 比例，0 白→1 深蓝；仅 k>0 的格子标 `k/10`；S4* 加粗；底部一行说明 S4* 与 hatched 区域。
- 三图输出 PDF/SVG/300 dpi PNG；灰度副本写入 `figures/gray/`。
- `plot_manifest_supplement.json` 记录输入 figure_data 的 SHA256、列、过滤、样本数、聚合和输出 SHA256。

### 4. 视觉检查与可读性修订

用户反馈初版“可读性差、有用信息不明确、排版混乱、文字太多、图注不明确”后，重新设计并逐张检查 PNG：

1. Fig.7 去掉每格的 `m=...` 和重复分母，只在面板副标题写 `n'`/`M`；格内改为 `C <count>` / `P <count>`；加白色 cell grid，底部只保留一行 C/P 与 F3M/F4S 同坏数据的说明。
2. Fig.S2 只在最左列/最下行显示 tick labels，共享 `D/T_ref` 轴标题；每面板副标题给 `n'`/`M`，避免 16 个面板重复坐标文字。
3. Fig.8 只画非对角场景对；零值留空、对角省略、S6/scrub=0 斜线但不再重复 `N/A`；仅 k>0 的格子标 `k/10`；标题改为 Bank/Page/Codeword，底部一行说明 S4* 与 hatched 区域。
4. 在 `figures/captions.md` 追加 Supplement figures 说明和 Fig.7/Fig.8/Fig.S2 完整图注，明确 S1–S6、C/P、n′、分母、N/A、scrub/codeword 假设。
5. 灰度版本逐张检查；坐标、色条、标题和 S4* 标记仍可读。最终图不再有文字跨格或图例/注释遮挡数据。

### 5. 结果观察（供 G16 使用，不在 G15 写结论）

- P1-G：PREVENTED 2166、CORRUPT_CONSUMED 1920、MASKED 234。
- P1-H 10-seed 数据：
  - bank/scrub=1：S2/S3/S4/S6 两两 10/10 不可区分（H-H1 支持）；
  - codeword/scrub=1：S4/S6 10/10 不可区分，S2/S3 可区分（H-H2 支持）；
  - scrub=0：S4 三种粒度均 10/10 出现 UE（H-H3 支持）；
  - bank/scrub=1：S2–S6 的 CoF 规则均 10/10 触发，S1/S5 为 0（H-H4 支持）。
- F3M PREVENTED 与 fig3 TIMELY 的完整 36 格中 8 格不一致；G16 必须按实际数字写报告，不调参。

### 6. 独立复核

`logs/g15_validate.log`：

```text
fig7 4320 fig8 330 pairs 90 cof 36 stats 432
bad 0
G15_VALIDATE=PASS
```

检查：figure_data 行数、144 个格子每格 30 seeds、statistics 分子与 fig7 计数一致、P1-H signature 与 metrics 一致、plot manifest 输入/输出 SHA256 与文件一致、所有输出文件存在。

### 7. 命令与退出码

```bash
.venv/bin/python scripts/aggregate_supplement.py
# exit 0；G15_AGGREGATE=PASS
.venv/bin/python scripts/plot_supplement.py
# 第一次 SyntaxError（global 位置）；修正后 exit 0；G15_PLOT=PASS
.venv/bin/python - <<'PY' ...独立复核...
# exit 0；G15_VALIDATE=PASS
```

### 8. 产物位置

```text
scripts/aggregate_supplement.py
scripts/plot_supplement.py
results/supplement/figure_data/fig7_escape.csv
results/supplement/figure_data/fig8_signatures.csv
results/supplement/figure_data/fig8_pairs.csv
results/supplement/figure_data/fig8_cof_rule.csv
results/supplement/statistics_supplement.csv
results/supplement/reconciliation_supplement.json
figures/fig07_escape_efficacy.*
figures/fig08_report_ambiguity.*
figures/figS2_escape_by_config.*
figures/gray/fig07_escape_efficacy.png
figures/gray/fig08_report_ambiguity.png
figures/gray/figS2_escape_by_config.png
figures/plot_manifest_supplement.json
figures/captions.md
logs/g15_aggregate.log
logs/g15_plot.log
logs/g15_validate.log
```

### 9. 下一关唯一任务

**G16：报告/图注/tex、最终回归、补丁与收尾。** 按补充手册 §8/§9 修改 `report/phase1_results.md` 和 `.tex`，审阅/补全已追加的 `figures/captions.md`，新建 `docs/g16_supplement_results.md`；再跑 G10/G13 回归（必须 0 差异），生成包含全部新增文件的 `environment/source_g16.patch` 并更新累计补丁；在固定提交干净副本应用补丁、构建并跑 P1-G/P1-H smoke；更新 README/STATUS。不得改动已有 paper 数字或冻结文件。

## G16 — 报告、图注、最终回归与补丁收尾

执行日期：2026-11-16（UTC）。执行对象：`PHASE1_SUPPLEMENT_HANDOFF.md` §8/§9。用户指示：本轮只修改报告，**无需重跑代码**。

### 1. 实际改动的文件

报告：

```text
report/phase1_results.md
report/phase1_results.tex
figures/captions.md
docs/g16_supplement_results.md
```

补丁/文档：

```text
environment/source_g16.patch
environment/source.patch
README.md
docs/STATUS.md
docs/implementation_log.md
logs/g16_patch.log
```

未修改：任何 C++ 源码/测试、`runs/paper/`、`results/g8/`、`figures/fig01..06`、现有 artifact、冻结数据。

### 2. Markdown 报告增补

`report/phase1_results.md`：

- §1 增加 supplement P1-G/P1-H 两条 scope，并说明独立 4650-case manifest 不计入 2602；
- §3.1 增加 supplement manifest 表：P1-G 4320、P1-H 330、total 4650；
- §4.7 新增 P1-G：F3M/F4S/F1W 设计、只读 held-B 评估、n_tiles=8 HOST 的 P/C/M 表、全量 4320 总数、F3M 与 Fig.3 的 28/36 精确一致与 8/36 差异（最大 5.7pp）、F4S 无报告结论、F1W SAME/DIFF 的 990/1080 一致与 90 个 MASKED 差异、DIFF 报告不覆盖 corrupt bank；
- §4.8 新增 P1-H：S1–S6、三种粒度、scrub 0/1、非单例等价组、CoF 规则、scrub=0 的 S4 UE 与不可纠正性；
- §5 增加 supplement 4650 = 4650 SUCCESS + 0 FAIL/TIMEOUT/MISSING；
- §6 表增加 Fig.7/Fig.8/Fig.S2 数据源与样本数；
- §7 增加“及时控制可扣留构造 escape 的错误输出（有报告时）”和“粗粒度报告不能识别 S4 是否与多个易翻转位共享 codeword”；
- §8 增加 scrub 理想化、codeword ASSUMED、F3M/F4S 构造模式、一次性注入、whole-kernel 锁定、10 seeds 等限制；
- §9 增加 supplement 的 expand → run → aggregate → plot 命令。

原有 2602-case 数字、P1-A..P1-F 结论与表格未改。

### 3. LaTeX 报告与图注

`report/phase1_results.tex`：

- 日期改为 2026-11-16；摘要增加 4650 supplement 全部成功；
- Scope 增加 P1-G/P1-H 说明；
- 新增 P1-G subsection、`tab:p1g`、`fig:escape`；
- 新增 P1-H subsection、`tab:p1h`、`tab:cof`、`fig:ambiguity`；
- 新增 Fig.S2 `fig:escape_config`；
- Expected negatives、Capability judgment、Limitations 增加 supplement 内容；
- 新增 Reproducibility section 与 supplement 命令；未本地编译（LaTeX 工具链不可用）。

`figures/captions.md` 已包含 Supplement figures 总说明和 Fig.7/Fig.8/Fig.S2 的
Source/Matrix/Cell text/Sample/Denominator/Failure handling/Model assumption。
`docs/g16_supplement_results.md` 逐条判定 H-G1..H-G3、H-H1..H-H4，并记录偏离、
失败、回归和 clean-replay 未执行。

### 4. 最终回归与冻结检查

```bash
.venv/bin/python scripts/compare_regression.py
# exit 0；REGRESSION=PASS（G10 240 cases）
.venv/bin/python scripts/compare_regression.py \
  --manifest configs/generated/regression_g13-manifest.json \
  --out-dir results/regression_g13
# exit 0；REGRESSION=PASS（G13 240 cases）
```

G10 冻结文件复核：33,867/33,868 哈希不变；唯一变化是允许追加的
`figures/captions.md`（G15 用户可读性修订）。没有把该例外写成“全部冻结不变”。

### 5. 补丁快照

```bash
cd simulator
git add -N $(git ls-files --others --exclude-standard)
git diff --binary > ../environment/source_g16.patch
git diff --check
git reset -q
cp ../environment/source_g16.patch ../environment/source.patch
git apply --check --reverse ../environment/source_g16.patch
```

结果：

```text
41 files changed, 7953 insertions(+), 39 deletions(-)
source_g16.patch / source.patch SHA256:
c8cf0ce175141ec6f4880d405efbf19a5b6b725431f18d7c878e974d74fd44ec
diff_exit=0, diff_check_exit=0, reverse_check_exit=0
```

补丁包含 Bank/Phase1Reports 修改、全部新增 Phase1 fixture/test（包括 G12/G13）。
日志：`logs/g16_patch.log`。

### 6. 未执行项

按用户“无需重跑代码”的指示，G16 未执行 clean worktree build/replay，也没有
重新运行 supplement smoke 或矩阵。`docs/STATUS.md` 和
`docs/g16_supplement_results.md` 明确标为未执行；没有把旧结果写成新一轮 clean
replay。最终回归比较是只读比较，不重跑 case。

### 7. 通过/失败检查

通过：

- 报告 MD/LaTeX 与 captions 增补完成；
- 原有 paper 数字/表格/结论未改；
- G10/G13 最终回归 `REGRESSION=PASS`；
- 补丁生成、`git diff --check`、反向 apply 检查；
- G10 冻结文件 33,867/33,868 不变 + 1 个允许追加的 captions 例外；
- README/STATUS 更新。

失败：无。

未执行：clean worktree build/replay（用户指示）。

### 8. 产物位置

```text
report/phase1_results.md
report/phase1_results.tex
figures/captions.md
docs/g16_supplement_results.md
environment/source_g16.patch
environment/source.patch
logs/g16_patch.log
README.md
docs/STATUS.md
docs/implementation_log.md
```

### 9. 下一关唯一任务

**无。** G0–G16 的报告、图、数据和补丁已收尾；clean replay 未按用户指示执行，
已在 STATUS 与 G16 结果文档中如实标注。

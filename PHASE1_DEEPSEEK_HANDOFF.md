# HBM-PIM Phase 1 实验框架与执行交接手册

版本：1.0；设计日期：2026-11-15。执行对象：DeepSeek Flash 或其他代码模型。文档语言为中文，代码、数据字段和论文图中文字使用英文。

**当前交付是实验设计与实现任务书，不包含已完成的仿真结果。禁止将本文中的示例参数、预期行为或验收条件描述为测量结果。**

## 0. 任务、依据与边界

依据项目文件 `HBM_jintao_main.pdf` 的 §1.1、§1.2 Preliminary Research Plan 和 §2 Threat Model。原稿 Phase 1 要求：确认 PIM 操作数读取与结果写入的 ECC 覆盖，确认可信 runtime 能得到的错误及其位置/时间字段，测量检测到通知的间隔，确定 host/device 输出可见性，并验证输出暂存、冲突操作暂停、可信输入恢复。原稿允许在实际接口不支持时使用明确的接口模型，并把结论限制在该模型内。

本任务的研究问题：**在一个明确的 HBM-PIM 执行模型中，哪些错误可被观测，报告是否及时，runtime 是否具有后续恢复所需的控制能力？**

本阶段交付接口能力、条件边界和可重复的证据。Phase 2 的依赖闭包、选择性重算算法和 Phase 3 的策略收益比较不在本阶段实现。允许为了验证恢复接口执行一次整 kernel 重启；这不是选择性恢复策略评估。

不得把以下命题混同：

1. CE 表示一次纠错事件，不等于计算已经污染。
2. 多次 CE 不等于当前同时存在多个错误位，也不唯一确定 codeword 集中度。
3. 报告及时与输出正确是不同指标。CE 已纠正时，即使报告迟到，输出也可能正确。
4. 接口测试按预期拒绝恢复，可以是测试通过；不代表该场景可恢复。
5. 可配置的仿真通知延迟不是实测硬件通知延迟。
6. 完整 SECDED 路径上的单 bit 错误应被纠正。禁止为展示传播而在该路径上悄悄绕过 ECC。
7. 无告警的 SDC 不得由注入器自动附送一条 CE 报告。

### 0.1 证据标签

所有配置说明、图注和结论使用以下标签之一：

| 标签 | 含义 | 可支持的结论 |
|---|---|---|
| DOCUMENTED | 原厂/论文公开资料中的事实 | 仅适用于相应平台、版本和访问路径 |
| UPSTREAM | 固定版本仿真器源码已有行为 | 该仿真器实现了这种行为 |
| ASSUMED | 本研究增加的接口或参数 | 假设该接口存在时的条件性结论 |
| SIMULATED | 本框架实际运行得到的数据 | 指定模型、配置和样本范围内的结果 |
| UNSUPPORTED | 未实现或证据不足 | 明确缺失，不填成 0 或通过 |

顶会论文标准在此指可审查的假设、合理对照、完整失败样本、独立正确性检查、可复现数据和规范图表；不保证创新性或录用。不得仅凭本阶段仿真声称真实 Aquabolt-XL 支持新增 ECC 接口，或已经阻断所有 Rowhammer 攻击。

## 1. 固定技术路线

### 1.1 基础仓库与版本

- 主仓库：<https://github.com/SAITPublic/PIMSimulator>
- 固定提交：`3703d1f19c8f027360cc33a3243eb271e3bb6898`
- 该提交在本手册编写时通过远端 `refs/heads/dev` 核实。执行时必须校验，不允许静默切到最新版本。
- 原始参考配置：`ini/HBM2_samsung_2M_16B_x64.ini` 与 `system_hbm_1ch.ini`。
- CPU 环境：Ubuntu 22.04 x86-64，Windows 使用 WSL2 中的 Ubuntu。工程放在 Linux 文件系统，如 `~/pim-phase1`；不要把 Windows PowerShell 当作后面的 Bash 执行环境。
- 编译器：发行版 GCC/G++，保留上游 C++14；Python 3.10 虚拟环境用于批量运行、统计和画图。
- C++ 使用上游 GoogleTest，不新增大型仿真框架。Python 使用 numpy、pandas、matplotlib、PyYAML、jsonschema、pytest。
- **必须启用数据存储，`NO_STORAGE=0`。** 每次构建记录实际编译选项。
- 本阶段不接入 gem5、Ramulator、真实 FPGA，不复现攻击利用链。

本地若存在其他 CoF 或论文实验工程，保持独立。所有新实现放在 `pim-phase1/`；不要修改 `cof-headroom/`、原论文文件或其他实验目录。

仓库包含自定义研究用途许可。保留原始许可证；交付优先采用固定上游版本下载脚本和自身补丁，不能把上游代码整体重标为 MIT。

### 1.2 已核实的入口与必须防止的问题

以下是源码审计起点，不是让执行模型盲目按行号修改。以固定提交的符号和调用路径为准。

| 文件/符号 | 用途与实现要求 |
|---|---|
| `src/Bank.cpp`, `Bank::read/write` | 存储读写。注入应修改实际存储，不能只修改 host 返回副本。所有实验读取必须先初始化；不要依赖未初始化读取行为。 |
| `src/Rank.cpp`, `readSb/writeSb/sendToBank` | 区分普通存储、保留地址、PIM 触发访问。寄存器控制地址不能按普通数据 codeword 处理。 |
| `src/PIMRank.cpp`, `readOpd/writeOpd/readHab/writeHab` | PIM 内部数据访问。一次包会访问多个真实 bank；须记录实际 bank 对象身份，不能仅使用 `packet->bank` 或 `physicalAddress` 作为故障位置。 |
| `src/tests/PIMKernel.cpp` | 命令生成与 barrier 参考。复用模式切换、CRF 编程和逐元素运算流程。 |
| `src/tests/KernelTestCases.h` | 原功能 fixture 使用 64 个逻辑 channel。新增 Phase1 fixture 明确选择 1/2 个，不能只修改一个 ini 就声称已经缩为 1 个。 |
| `src/MultiChannelMemorySystem.cpp` | 使用 `actual_update()` 的 DRAM 周期作为唯一时钟；`setCPUClockSpeed(0)` 明确 1:1 时钟关系。 |
| `hasPendingTransactions()` | 原实现统计在途事务。新增运行结束/暂停判断还必须检查 feeder、命令状态、报告队列、控制定时器及输出状态。 |
| `src/Burst.h` | burst 为 32 B，FP16 为每 burst 16 个元素。显式按字节构造/比较，不依赖带容差的运算符判定 bit-level 正确性。 |
| `Sconstruct` | 上游自动收集 `src/*.cpp` 和 `src/tests/*.cpp`。新模块平铺在这两处；不要创建未被构建的深层目录。 |

源码路径依据见 §16。编写手册时已核对上述入口；未在本环境完成 Linux 编译。执行者必须完成 G0 构建验收，不能把“源码可访问”写成“平台已验证”。

## 2. 交付目录与职责

下面文件除本手册外均由执行者创建。`runs/`、`results/`、`figures/` 必须由程序生成，不手工填写结果。

```text
pim-phase1/
  README.md                         # 最短复现步骤、完成状态、主要限制
  HANDOFF.md                        # 本手册的副本
  environment/
    upstream.lock.json              # URL、commit、原始配置哈希
    environment.txt                 # OS、编译器、CPU/RAM、Python、构建标志
    requirements.lock.txt           # 实际成功环境的 pip freeze
    source.patch                    # 包含新增源码的可应用补丁
  simulator/                        # 固定提交的 PIMSimulator 工作副本
    src/Phase1Ecc.{h,cpp}
    src/Phase1Address.{h,cpp}
    src/Phase1Storage.{h,cpp}
    src/Phase1Reports.{h,cpp}
    src/Phase1Runtime.{h,cpp}
    src/Phase1Driver.{h,cpp}
    src/Phase1Trace.{h,cpp}
    src/tests/Phase1TestCases.cpp
  configs/
    base.yaml                       # 本文默认配置，版本化
    suite_smoke.yaml
    suite_paper.yaml
    suite_robustness.yaml
    schema.json
    generated/                      # 展开后每个 case 的完整配置
  scripts/
    bootstrap.sh                    # 固定版本获取与环境检查
    build.sh
    run_baseline.sh
    expand_suite.py
    run_suite.py
    validate_run.py
    aggregate.py
    plot_paper.py
    reproduce.sh
    package_artifact.py
  tests/
    test_ecc_reference.py            # 独立 Python 参考编码/解码
    test_schema.py
    test_metrics.py
    test_plot_inputs.py
  docs/
    platform_contract.md            # 能力、假设、限制及证据位置
    source_audit.md                  # 实际调用链、bank 身份、时钟和完成语义
    experiment_protocol.md          # 实际执行协议与偏离本文之处
    claim_evidence.md               # 每项结论对应运行、统计表和图
    limitations.md
    implementation_log.md           # 每关变更、命令、失败原因与解决办法
    STATUS.md                       # 各关状态与下一项唯一任务
  runs/
    baseline/                       # 未修改数值/时序基线
    smoke/<run_id>/
    paper/<run_id>/
    robustness/<run_id>/
  results/
    run_metrics.csv
    summary.csv
    failure_cases.csv
    figure_data/                    # 每张图实际使用的 CSV
    validation_summary.json
  figures/
    fig01_path_coverage.{pdf,svg,png}
    fig02_report_latency.{pdf,svg,png}
    fig03_control_window.{pdf,svg,png}
    fig04_quiescence.{pdf,svg,png}
    fig05_restore.{pdf,svg,png}
    fig06_cost.{pdf,svg,png}
    captions.md
  report/
    phase1_results.md                # 论文式方法、结果、边界与限制
    phase1_results.tex               # 可纳入论文的英文实验段落与 figure 引用
    references.bib
  artifact/
    MANIFEST.sha256
    reproduction_log.txt
    phase1-artifact.tar.gz
```

每个运行目录保存：`config.json`、`params.ini`、`metadata.json`、`stdout.log`、`stderr.log`、`events.csv`、`reports.csv`、`truth.csv`、`output.bin`、`reference.bin`、`metrics.json`、`validation.json`。没有输出的预期中止实验可不生成两个 bin，但必须在 metadata 指明原因。报告集为空时生成只有表头的 CSV，不能伪造一条零事件。

`source.patch` 包含新文件；可用 `git diff --no-index` 或本地提交后相对上游生成。仅运行 `git diff` 但遗漏 untracked 新文件不算交付。打包在新目录重放补丁、运行 smoke 验证后才算可复现。

## 3. 模型配置：先固定，再做受控扫描

### 3.1 硬件与工作负载

主实验：1 个模拟逻辑 channel（按上游 pseudo-channel 模型解释）、1 rank，bank 数、PIM block 数、时序参数全部沿用设备 ini，不人为缩为 1 bank。FP16、Scheme8。扩展隔离测试使用 2 个逻辑 channel；不要把它们称为两个真实 HBM stack。

所有运行保存解析后的完整内存几何、容量、tCK、队列深度和地址映射；构造函数容量必须与解析几何一致，且满足上游要求。若解析结果与拟用配置矛盾，G1 失败并记录，禁止修改 geometry 来获得想要的结果。

工作负载固定如下：

| ID | 运算与用途 |
|---|---|
| W0 | host 写一个 burst、host 读同一 burst，校准存储/ECC/报告语义 |
| W1 | PIM 逐元素 ADD：`B=A+X`；主要生产者 kernel |
| W2 | W1 完成后在 PIM 执行 `C=B*K`，K 的每个元素为 2；测试设备消费者 |
| W3 | 在另一个逻辑 channel 执行独立 W1；测试暂停作用域 |

W1 使用 `n_tiles ∈ {1,8}`；初始输入生成规则 `A[i]=((i+seed) mod 17 - 8)/8`、`X[i]=((3*i+seed) mod 13 - 6)/8`，均为有限、可精确表达的二进制分数。参考结果按 FP16 运算顺序生成；W2 在 ADD 结果舍入后再乘 2。不要引入 GEMV 的不同归约顺序作为第一版正确性难题。

依据上游逐元素生成器，`dim` 是 burst 数，不是 FP16 元素数；本方案设置 `dim_bursts=n_tiles*NUM_BANKS*NUM_PIM_CHANS*NUM_PIM_RANKS*8`，元素数为其 16 倍。确认生成器不发生整数截断。A、X、B、K、C 分别使用普通数据 row 0、128、256、384、512，列从 0 开始；检查列进位与保留行，保存完整实际映射。多 channel 的逻辑元素与物理位置由现有地址生成器分配，不能靠连续地址猜测落在哪个 bank。

### 3.2 保护配置

| profile | 具体定义 | 解释 |
|---|---|---|
| NATIVE | 不增加 ECC、报告或控制；保持原始数据执行 | 上游能力对照，不等于真实芯片关闭了所有 ECC |
| LOCAL_VISIBLE | 普通存储与 PIM 存储读写均经过新增 ECC；CE/UE 可报告 | 主实验，ASSUMED |
| LOCAL_HIDDEN | 数据保护与 LOCAL_VISIBLE 相同，但 CE 不交付 runtime；UE 按下面规则处理 | 验证纠正与可观测性的区别 |
| HOST_ONLY | host 读写保护；PIM 内部读取不检查，PIM 写回不产生 ECC | 路径缺口对照，ASSUMED，不是已验证的 Aquabolt 实现 |

HOST_ONLY 下 PIM 写回必须标记 `ecc_valid=false`，后续受保护读取返回 `UNPROTECTED_FORMAT` 并使结果无效；不得沿用陈旧 parity 再把格式不一致算成 Rowhammer UE。仅测试 PIM 读取路径缺口时，对错误传播使用独立真值输出对比，不强行安排一个不存在的报告。

UE 的最小语义：decoder 返回无效状态，整个当前 kernel 结果标记 `POISONED`，不允许提交。允许在途命令排空，但计算出的 payload 不作为有效数据。该有效性控制是新增接口；UE 不混入 CE 延迟窗口主图。`UNPROTECTED_FORMAT` 也不冒充 CE/UE。多 bit 的 SDC 不在本阶段强制构造。

### 3.3 ECC 规范

实现显式 extended Hamming SECDED(72,64)，每 8 B 数据对应 8 个校验位；每个 32 B burst 有四个独立 codeword。它是研究模型，不能当作真实 HBM 的 codeword 布局。

固定编号：位置 1..71 中的 1、2、4、8、16、32、64 是 Hamming parity，其他位置按升序放数据 bit 0..63；位置 72 是覆盖前 71 位的整体偶校验。数据按字节小端顺序读入，bit j 对应 byte `j/8` 的 `j%8` 位。

读取时从当前原始数据和 parity 计算 syndrome s 与整体校验 p：

- s=0,p=0：CLEAN。
- s!=0,p=1 且 s<=71：纠正位置 s，返回 CE。
- s=0,p=1：整体 parity 位错误，返回 CE，数据不变。
- s!=0,p=0：UE。
- s>71,p=1：按无效/不可纠正处理；不做越界修复。

单/双 bit 范围外不承诺 decoder 行为；不能用“翻转数大于 1 就 UE”替代真实 syndrome 计算。

parity 放在独立元数据中，与 `(channel,rank,actual_bank,row,burst_column,codeword_index)` 绑定；该存储开销为每 32 B 数据 4 B，不包含其他元数据。主配置读取纠正后**不自动 scrub 回原存储**，故同一持续错误可能产生多次 CE；按真实读取次数记录，不按注入次数统计报告。普通合法写入重新编码；故障注入不得调用会重新编码的写接口。

ECC 功能层的额外流水延迟默认 0：表示本阶段只检查接口语义、报告与控制时序。必须明确这是理想化假设；本阶段不能据此声称完整 ECC 系统没有性能成本。后续若加入 ECC 时序，必须在实际数据依赖调度中生效，不允许只在日志中加数字。

### 3.4 报告与控制默认值

- 主报告粒度 bank；page 对照为 4 KiB 的模拟 host 物理地址页。地址由实际访问 tuple 反向构造并 round-trip 验证。一个 page 可能跨多个 bank，不能把 page 与 bank 当作严格包含层级。
- runtime 可见字段仅：`report_seq, report_cycle, error_class, region_type, region_id`。不暴露注入时间、检测时间、bit、syndrome、codeword、golden hash。
- 每次 codeword 解码事件最多生成一条 CE/UE 报告，不聚合；正常模式队列容量 Q=64，服务间隔 I=1 DRAM cycle，运输延迟 D 按实验扫描。
- 报告最早在 `detect_cycle+max(1,D)` 可交付。D=0 表示无额外运输延迟但仍有一个循环交付边界。FIFO 按检测顺序入队，同周期按 channel、bank、codeword 和事件序号稳定排序。
- 当 runtime 收到首个相关报告，立即将当前 producer kernel 输出标记 SUSPECT 并禁止发布；处理延迟 L=8 cycles 后请求暂停。L 是假设的控制开销，不是主机 CPU 实测。
- 暂停作用域默认整个目标逻辑 channel；输出提交/恢复粒度默认整个 producer kernel。
- 主实验 runtime 仅管理一个 producer kernel，因此首个相关报告保守关联整个 kernel；不需要也不得使用真值定位某个 tile。多 channel 只按公开报告映射与预登记的工作范围确定冲突集合。
- 主实验队列不应丢报告；发生 overflow 属于协议失败。鲁棒性测试中显式允许丢弃并计数，标为观测缺口。不得自动向 runtime 额外发送 overflow 告警，除非独立标明新增接口。

## 4. 功能模块与接入要求

### 4.0 固定模块接口与依赖方向

实现时保持下面职责，不把全部逻辑塞进 fixture。名称可按 C++ 风格调整，但输入/输出边界不能改变。

| 模块 | 必需操作 | 不得依赖 |
|---|---|---|
| Phase1Ecc | `encode(data64)`、`decode(data64, parity8)` 返回 corrected_data/status | 故障 bit mask、golden、runtime |
| Phase1Address | `makeStorageKey(actual_bank, row, col, cw)`、`toHostAddress(key)`、`coarsen(key, granularity)` | 分类标签或预期实验结果 |
| Phase1Storage | raw read/write、合法写编码、路径化 read/decode、测试专用 raw bit mutation | 恢复策略决策 |
| Phase1Reports | `onDetected(internal_event)`、`tick(cycle)`、只向 runtime 交付 PublicReport | 输出参考值 |
| Phase1Runtime | `onReport(public_report)`、`canAdmit(descriptor)`、`tick(cycle)`、输出版本/暂停/恢复状态机 | fault schedule、检测真值、golden、隐藏队列内容 |
| Phase1Driver | 事务 recipe、feeder、回调计数、时钟挂接、控制请求执行 | 用 golden 代替实际 kernel 计算 |
| Phase1Trace | 事件 CSV、报告 CSV、真值 CSV 的独立输出入口 | 改变模型状态 |
| Python validator | 从保存的 trace/config/reference 重新计算断言与指标 | 控制正在执行的 runtime |

将配置解析成 `ModelConfig`、`RuntimeConfig`、`FaultSchedule` 三个不同对象；RuntimeConfig 只包含公开报告协议、延迟上界、受管 buffer 元数据和控制策略参数。不要把完整 params.ini 的 map 引用传入 runtime 后仅靠“约定不读”隔离真值。故障解析后的精确地址只属于 FaultSchedule/评估器。

外部评估器可以在未发布状态对存储做只读检查，用于观察真值；该读取必须标记 EVALUATOR、绕过消费者权限计数，不触发 ECC 报告，不进入模拟事务或被冒充为实际消费。正常 host/PIM 读必须经模拟路径和门禁。从 reportless case 观察到错误，仅证明评估器知道，不意味着 runtime 知道。

### 4.1 Phase1Address / Phase1Storage

构造唯一的 StorageKey，使用实际 bank 对象身份。修改 Bank 或其调用封装，让 host、PIM operand、PIM result、HAB 数据传输都能显式传递访问类型。所有可达路径均有测试，不允许只给 host 数据通路加 ECC。

提供原始只读查询和测试专用 bit mutation 接口，仅注入器/评估器可调用。runtime 类不得持有注入器、真值数据或 golden output 引用。存储读取中先执行预设故障事件，再按 profile 解码，最后把解码后的数据交给消费者。

页报告必须从实际 bank 的地址映射产生。扩展测试至少挑 bank 0、bank 3 和最后一个 bank；确认报告分别指向真实目标，不把广播命令的 bank 字段误用为所有 PIM block 的地址。

### 4.2 Phase1Driver：有界提交与命令所有权

沿用上游命令生成逻辑；为 Phase1 单独增加事务描述符与提交适配器。描述符包含地址、读写方向、tag、kernel/stage ID、消费者类型、受访问 buffer 版本和稳定的 payload 生命周期。

第一版每 channel 最多允许 8 个已提交未完成的事务，feeder 每 cycle 最多尝试提交 1 个。提交被拒绝时保留 descriptor 下周期重试，不能丢命令。保持原有 barrier 和模式切换顺序。标准 baseline 与所有新增模型对照使用同一个 feeder；同时保留真正未修改上游的结果，以区分 feeder 引入的变化。

捕获写事务时保存当时 payload 的副本，不能保留指向反复复用的临时 `BurstType` 的悬空指针。读事务保持独立接收缓冲区并在完成时交给指定接收方。先通过关闭扩展的数值对照，再增加故障。

不要一次将整个 kernel 的命令不可逆塞进控制器，随后仅停止生成下一个 kernel 并称为“支持暂停当前 kernel”。外部待提交队列和内部在途队列必须分开计数。

### 4.3 Phase1Runtime：输出状态

每个输出 buffer 版本的状态机为：

```text
ALLOCATED -> PRODUCING -> READY_UNPUBLISHED -> PUBLISHED
                   \              \
                    -> SUSPECT <---     （收到相关报告）
SUSPECT -> QUIESCED -> RESTORING -> REEXECUTING -> READY_UNPUBLISHED
任意未发布状态 -> POISONED / ABORTED
```

状态必须与版本号绑定。旧输出恢复后被新版本替代；消费者不能拿旧版本句柄读取新版本或已经失效的数据。`SUSPECT`、`POISONED` 不得仅靠超时自动转为 PUBLISHED。

结果写在原有 HBM 输出区域，增加版本与发布权限，不默认额外复制一整份输出。占用的暂存容量指尚未发布/可复用的输出字节数。host 读取和下游 PIM kernel 提交都检查该权限。上游 producer 的内部计算访问不受自己的输出发布门禁阻塞。

采用可信 runtime 管理的 producer/consumer kernel 调度。只有已知读集的受管理 kernel 可提交；未知读集按整个受管输出域保守处理。门禁在命令进入控制器前生效；对直接到达存储接口的非法消费者访问设置 fail-fast 断言。禁止在 `Bank::read()` 内无限等待，导致控制器死锁。

hold 策略：`H=0` 允许 producer 完成后立即发布；`H>0` 则在 producer 完成后至少等待 H cycles，期间没有 SUSPECT/POISONED 才能发布。这是有限等待策略，**不是无告警即安全的认证**。额外的 `H=indefinite` 只用于恢复接口测试，必须有人工定义的恢复/终止路径，不能参与正常延迟均值。

### 4.4 暂停与恢复

暂停过程：发出 `QUIESCE_REQUEST` -> 禁止目标 channel 新提交 -> 已提交的有限事务排空 -> 验证没有可继续访问数据的命令/数据返回 -> `QUIESCE_ACK`。外部未提交的事务保持冻结，不纳入“必须排空”的集合。报告队列继续推进。

刷新不因暂停停止；禁止用户/PIM 数据访问，不禁止维持 DRAM 状态所需的 REF。暂停中自动控制命令也必须说明用途与允许范围。

Phase1 的恢复实验在 ACK 后丢弃目标旧 kernel 的未提交命令，按明确流程退出/重新建立 PIM 模式，重新初始化 CRF、GRF、SRF、累加中间状态和控制状态，再从可信输入重启整个 kernel。不得把暂停模型改为“new 一个全新模拟器并从头运行”，这样会丢失在途工作、时间和内存状态。

第一版可信副本放在模型中的受保护 host 区域，保存不可变的初始输入与版本号。恢复采用经模拟存储通路的 host 写事务；除该可模拟搬运以外的 host 取副本、互连开销若没建模，标为 excluded。原输入备份建立和占用也计入单独指标。checksum 仅验证副本完整性，不作为恶意破坏的加密保证。

旧报告处理固定为：在主配置无丢弃、Q 有界、D 有界的条件下，ACK 后保持暂停至少 `max(1,D)+Q*I` cycles，使最后一次可能的解码产生的报告有机会交付；期间只累计报告，不重复启动恢复。该等待上界属于接口假设，runtime 不读取隐藏报告队列来判断已清空。若实际交付超出约定上界，判 REPORT_BOUND_VIOLATION。无延迟上界、发生丢弃或报告源仍活跃的配置不能沿用该证明。

随后显式进入 `RESTORE_MAINTENANCE`，只允许带恢复身份的 host 写和状态重建操作；普通消费者仍被阻止。完成重建再进入 REEXECUTING。因此“ACK 后无数据访问”的检查区间是暂停保持区间，不包括经过授权的恢复维护阶段。

本阶段默认一次性存储故障，恢复写入后不继续注入。重新使用原存储只验证恢复接口；不能据此声称已经隔离持续脆弱的存储或防止再次 Rowhammer。持续攻击下的迁移/下线策略留给后续阶段。

## 5. 时间、事件顺序与结束条件

唯一时间单位是 DRAM cycle；ns 仅通过已记录的 tCK 换算。模拟器墙钟运行秒数用于实验成本，不作为设备延迟。

每个 cycle 的 Phase1 执行顺序固定为：

1. 应用预定的 cycle 型存储故障；仅测试用的第 k 次读取注入在实际读取前执行。
2. 交付本 cycle 已到期的报告，最多一个（I=1 时）。
3. runtime 处理收到的报告与控制定时器；立刻锁定可疑输出。
4. feeder 提交被允许的事务，遵守暂停和输出权限。
5. 推进上游内存/PIM 一个 DRAM cycle；可能产生检测、结果写回、事务完成事件。
6. 汇总 kernel 完成与输出发布。相同 cycle 的旧报告交付优先于发布；步骤 5 新产生的报告最早下 cycle 交付。

实现可在 `actual_update()` 前后挂接；只调用一次内存推进，不能 driver 调一次、hook 又调一次。使用单调 `event_seq` 和 `phase` 明确同 cycle 顺序。故障发生、检测、报告、暂停确认、结果可读取、不可逆提交分别记录，缺失事件使用空值，不能填 0。

关键时间：`t_fault,t_detect,t_report,t_lock,t_quiesce_request,t_quiesce_ack,t_producer_done,t_publish,t_consume,t_restore_begin,t_restore_end,t_reexec_end`。

物理写回不等于对消费者发布；PIM/host 消费也不一定不可逆。本阶段把 host 的最终交付和下游 PIM consumer 的首次实际操作数读取设为该实验的不可撤销边界。这是保守定义，后续有完整回滚机制时可放宽。

正常结束需要：producer/consumer/控制流程终态、feeder 无待执行任务、模拟器无在途数据事务、报告已交付或按配置丢弃、所有需要结束的定时器终态。常规刷新存在不阻止仿真终止。默认 watchdog 为 `max(100000,100*T_ref)` cycles，另设每进程 120 s 墙钟超时；触发后保存诊断并标 TIMEOUT。若 baseline 本身超过该墙钟限制，依据 baseline 时间统一调大并记录，不只放宽失败 case。

在 P1-C 的 CONTROLLED_NOT_RECOVERED 或其他预期中止终态，允许在 ACK 后将冻结的未提交 descriptor 标为 CANCELLED、记录数量并结束；不要求它们继续执行到 producer 完成。报告交付/观察窗口仍需走完。没有 T_ref 的早期单元实验使用固定 100000 cycles watchdog。

事件顺序示例（仅说明协议，不是实验数据）：检测在 cycle100，D=16，则空闲报告服务器在116交付。假设 producer 在110完成：H=4 时计划114发布，报告来晚；H=16 时计划126发布，116先锁定，124发起暂停，126不得发布。若该事件是成功纠正的 CE，两种情况下数值均可能正确；变化的是控制机会。

## 6. 故障、参考执行与数据隔离

### 6.1 故障类别

- F0：无故障。
- F1：一个数据 bit 翻转；主要用目标 FP16 元素符号位（burst 数据 bit `16*lane+15`），lane∈[0,15]。
- F2：同一 64-bit 数据 codeword 的两个不同数据 bit；默认该 codeword 的数据 bit 0 和 1。
- FP：一个校验位翻转，仅用于 ECC 验证。
- FN：未覆盖 PIM 读路径上的 F1，检验无报告/传播边界。

以 raw stored bit 为注入位置，禁止在 ALU 输出上直接篡改来代替存储错误。每 case 默认一次故障；注入后不会自动再次翻转。持续存在的错误可以多次被读取。没有初始化或目标读取从未发生必须报 INVALID_CASE。

### 6.2 配对参考

每个配置、输入 seed 和 consumer 类型，运行无故障参考并保存输出 hash、实际二进制输出和时间线摘要。单独维护原始上游基线、采用 feeder 的无扩展基线、启用接口但无故障的基线；三者不能混用。

正确性检查采用同一算子顺序的原始 FP16 bytes 精确比较；辅助输出绝对/相对数值误差和 NaN/Inf 数量。不可使用宽容差掩盖 bit flip。独立 Python 参考与上游无故障执行也应一致；若有差异先修输入/精度/顺序，不调整误差阈值。

golden 执行和 fault schedule 仅为评估工具可见。控制器不得通过 `truth.csv`、reference.bin、精确注入地址或故障发生时间做决策。用静态依赖检查及运行中访问断言验证这种隔离。

### 6.3 统计单位

默认 paper seeds 为 0..29，每个 seed 决定输入与合法故障位置；30 次是 30 个场景，不是独立芯片。选择目标时使用 Python `numpy.random.Generator(PCG64(seed))` 生成并写入展开后的配置；C++ 不重新随机采样。位置按 `seed mod 3` 分层到合法 producer 读取事件序列的前/中/后第三段，再在该段确定目标。

微实验使用合成小程序，不能宣称代表真实生产 workload 频率。随机样本结果提供场景分布，不等于普遍安全保证。

## 7. 五组实验与固定运行矩阵

执行顺序 A -> B -> C -> D -> E，之后才能执行开销与鲁棒性。每组首先 smoke，再 paper。避免把所有参数做无目的笛卡尔积。

### P1-A：保护路径与数据正确性

路径：HREAD（host 写后 host 读）、PREAD（host 写后 PIM 读）、PWRITE_HREAD（PIM 写后 host 读）、PWRITE_PREAD（PIM 写后下一 PIM 读）。PIM 写回测试在写回完成后、后续读取前注入；否则没有测试到结果存储保护。

矩阵：4 profiles × 4 paths × {F0,F1,F2} × {bank 0,bank 3,last bank}；同一 bank 重复时去重。使用 n_tiles=1、seed=0，F2 为 same-codeword。另加每个 profile 对普通数据地址与保留控制地址的区分测试。

预期：LOCAL_VISIBLE/LOCAL_HIDDEN 的受保护单 bit 读取输出正确；同码字双 bit 读取无效。LOCAL_HIDDEN 的 CE 不对 runtime 可见。HOST_ONLY 的内部读取不声称被纠正，PIM 写后读取按 UNPROTECTED_FORMAT 处理。NATIVE 不应自动产生 ECC 报告。

输出：按 profile/path 记录访问总数、解码数、纠正数、CE/UE/格式事件、报告数、正确/错误/无效输出数。F1 的 PIM 传播可能被运算掩蔽，不能强迫每个 FN case 都输出错误；另设非零符号翻转的确定性 witness 确保该路径确实能传播。

验收：profile 与实际钩子匹配，保证范围内的 ECC 结果全对；每次检测都有唯一事件，所有报告均有对应真实检测；reportless case 不出现伪造报告。

### P1-B：位置与通知延迟

使用 W0 单次受保护读取，确保一次 F1 只产生一次 CE。LOCAL_VISIBLE，粒度 {bank,page}，D∈{0,1,16,256,4096} cycles，I∈{1,8} cycles，目标 {bank0,bank3,last bank}。输入 seed=0。

无队列竞争时预期通知延迟为 max(1,D)，若服务调度存在前序占用则另计服务等待。服务计时起点定义为首次 ready event：空闲服务器有到期事件即可立即交付，之后 I cycles 内不得再次交付。

再增加受控 burst：32 次实际解码事件，保持 Q=64，以同一 target 被重复读取或多个已初始化目标产生；记录队列占用、到期时间与服务时间。不得直接伪造 32 条报告替代解码事件。

验收：报告区域包含真实访问；单事件无重复/无丢失；实测交付满足 FIFO、ready 和服务间隔约束。参数 D 与实测总延迟分别列出。

### P1-C：输出可见性与控制窗口

主矩阵：LOCAL_VISIBLE、bank 报告、W1 的 n_tiles∈{1,8}、consumer∈{HOST,PIM(W2)}、D/T_ref∈{0,0.1,0.5,1,2}、H/T_ref∈{0,0.25,1}、seeds=0..29。共 1800 个 fault case；对应参考可按完整无故障配置哈希复用，不重复算作独立样本。

T_ref 为相同 n_tiles、相同 feeder、无 hold、无故障 producer 在数据初始化完成之后，从模式准备开始到全部 producer 事务完成的 DRAM 周期数；不含输入 preload，包含模式切换与结果写回。先完成校准，再以 `ceil(ratio*T_ref)` 固定 D/H，保存到每个 config。不同 consumer 在该定义下共享 producer 校准，但仍保存完整参考键。

consumer 在 producer 完成后立即提出读取请求。HOST 读取全部 B 后交给外部 sink；PIM 在 W2 的第一次 B 读取时记录消费边界，完成后验证 C。H 延迟从 producer 完成起算。

主要 fault 为 F1。收到报告立刻锁住未发布输出，L cycles 后请求暂停；本实验只测能否控制，不自动重算。已锁住的 case 在 quiesce 完成并完成日志后以 `CONTROLLED_NOT_RECOVERED` 结束。对未收到报告且 H 到期的 case 允许按配置发布，并据实际数据/时间分类，不能称为认证安全。

负面对照：LOCAL_HIDDEN 和 HOST_ONLY，各取 n_tiles=1、两个 consumer、H∈{0,T_ref}、seeds=0..5。HOST_ONLY 使用 PIM 读缺口；若因格式无效中止，单列 FORMAT_UNSUPPORTED，不计为恢复成功。NATIVE 的确定性传播 witness 也必须保留。

同周期边界单测：人为校准使 report 到达分别位于予定 publish 的前一 cycle、同 cycle、后一 cycle。前两者应优先锁定，后一种必须记录迟到；测试注入器不得把预计 publish 时间传给运行时。

结果分类至少分为：TIMELY_CONTROL、LATE_CONTROL、NO_REPORT、POISONED、FORMAT_UNSUPPORTED、INVALID_CASE、TIMEOUT。另有独立数值字段 `output_changed`。不存在消费事件时不得凭空计算 margin。

### P1-D：暂停作用域与在途工作

主测试 n_tiles=8、1 channel，分别在首次任务提交前、已有 feeder 队列但尚未发出目标命令、目标数据命令已经在途、接近 kernel 完成时请求暂停。前三组可用显式 control probe 校验原语，必须标为 SYNTHETIC_CONTROL；另以实际 CE 报告触发运行一次完整通路。

提交窗口深度 K∈{1,8,32}；L∈{0,8,64} cycles；每组合固定 seed=0，并保存真实触发位置。暂停后继续推进至少 `max(32,2*tRFC_cycles)` cycles 的观察窗口，允许刷新，断言目标用户/PIM 数据访问计数不再增加。

2-channel 扩展：W1 和独立 W3 同时运行；暂停 channel0，channel1 不设置额外门禁。验证 channel1 是否继续进展，并报告完成时间变化。共享总线/调度若导致影响，照实报告，不能把“没暂停”改写成“零性能影响”。

验收：ACK 时真正无在途数据访问；冻结的外部队列不丢失；正常刷新继续；没有新冲突请求被接受；不冻结整个模拟时钟。记录 `quiesce_ack-request`、停止前/后访问数和独立工作进度。

### P1-E：可信输入与重启接口

n_tiles∈{1,8}，seeds=0..9；输出始终暂存，恢复前不发布。用一次报告触发或显式 RECOVERY_PROBE 启动暂停，分别标注来源。固定恢复整个 kernel。

正例：初始 A/X 副本有效 -> 暂停 -> 废弃旧结果版本 -> 通过模拟 host writes 恢复输入 -> 重建 PIM 状态 -> 执行 W1 -> 发布新版本 -> HOST/W2 验证结果。参数中保存初始输入版本，不允许读取 reference.bin 作为恢复源。

负例各至少 1 次：副本缺失、副本版本错误、副本校验失败、输出已提交、旧 consumer 句柄读失效版本。分别返回 CHECKPOINT_MISSING、VERSION_MISMATCH、CHECKPOINT_CORRUPT、ALREADY_COMMITTED、STALE_HANDLE。已提交案例可演示后续修复，但不能记为原输出完整性恢复。

验证寄存器状态必要性：恢复前给 GRF/SRF/中间累加状态设置已知非默认测试值，然后按重启流程重建。此为状态复位测试，不计入存储 Rowhammer 故障样本。至少有一个移除必要初始化后能被测试捕获的负面对照。

记录恢复输入字节、实际写事务数、模式切换/状态初始化、重启周期、被保留输入容量、结果版本变化、最终逐 bit 正确性。不可只验证“restore 函数返回 true”。

### P1-F：成本与鲁棒性补充

无故障开销：n_tiles∈{1,8}，HOST/PIM consumer，seeds=0..29，比较同 feeder 下 EXT_OFF、LOCAL_VISIBLE+H0、LOCAL_VISIBLE+H0.25T、LOCAL_VISIBLE+H1T。统计模拟端到端时间、版本元数据、parity、可信输入、暂存输出峰值占用和额外数据流量。另单列插桩造成的模拟器墙钟开销。

鲁棒性：Q∈{1,8,64}，I∈{1,8,64}，在报告 burst 下测试溢出；report drop∈{0,0.1} 仅作为人为注入的观测缺口，记录具体丢弃序号。再测试保持输入错误但推迟首次读取，说明 fault-to-detect 与 detect-to-report 不同。只在独立补充图中呈现，不污染主实验假设。

## 8. 数据协议与自动检查

### 8.1 配置示例

下例是需实现的 schema，不是上游现有命令行功能。`expand_suite.py` 将 YAML 校验、展开并写为规范 JSON；C++ 只读取自动生成的扁平 `params.ini`，避免新增 YAML C++ 依赖。未知 key、遗漏必需值、负延迟一律报错。

```yaml
schema_version: 1
experiment: P1C
seed: 0
profile: LOCAL_VISIBLE
channels: 1
workload: ADD
n_tiles: 1
consumer: HOST
report_granularity: bank
transport_delay_cycles: 16
report_service_interval_cycles: 1
report_queue_capacity: 64
handler_delay_cycles: 8
hold_cycles: 128
feeder_outstanding_limit: 8
feeder_issue_per_cycle: 1
fault_kind: F1
fault_trigger: before_target_read
target_access_ordinal: 4
target_lane: 0
data_bit_in_lane: 15
checkpoint: immutable_initial_inputs
```

实际生成配置还必须包含解析后 StorageKey、T_ref、配置文件哈希、故障是否已解析、超时和输出目录。目标读取序号须在预运行生成的合法事件清单内，不使用示例数字直接批量注入。

### 8.2 文件字段

`metadata.json`：schema_version、run_id、experiment、seed、upstream_commit、patch_sha256、binary_sha256、config_sha256、reference_id、environment_hash、start_time_utc、wall_seconds、exit_code、run_status。

`events.csv`：event_seq、cycle、phase、event_type、kernel_id、version_id、access_id、channel、rank、actual_bank、row、column、access_path、bytes、status。内部诊断可有精确位置，但 runtime 不得读取此文件。

`reports.csv` 严格只有：report_seq、report_cycle、error_class、region_type、region_id。这个文件就是 runtime 实际看到的报告副本。

`truth.csv`：fault_id、fault_cycle、storage_key、codeword_index、bit_positions、raw_before、raw_after、target_access_id、detect_cycle、decoder_status、eligible_report_id、dropped。单独由评估器写出；runtime 不得读取。

truth 增加 `record_type` 与 `detection_id`：INJECTION 行描述一次注入；DETECTION 行描述一次解码检测，允许同一 fault_id 对应多次 detection。报告可能为 0 条、多条或丢失，不能强制一条故障只对应一条报告。无故障格式事件的 fault_id 可为空。

`metrics.json` 至少包含：contract_pass、outcome_class、output_changed、changed_elements、invalid_outputs、detected_events、delivered_reports、dropped_reports、report_latency_cycles（列表）、quiesce_cycles、restore_cycles、reexec_cycles、producer_cycles、end_to_end_cycles、host_transfer_bytes、restore_write_bytes、peak_held_output_bytes、checkpoint_bytes、parity_bytes、runtime_metadata_bytes、timeout。无定义字段用 null。

`validation.json` 列出每条 assertion 的名称、pass/fail、预期值、实际值、相关 event_seq。`contract_pass` 与 `outcome_class` 分开，避免把期望失败案例当作错误实现。

### 8.3 不变量

- 事件序号严格递增，cycle 不倒退，同 cycle phase 次序一致。
- 实际注入次数符合配置；未命中目标不能自动换目标并继续。
- 受保护 F1 的返回数据等于注入前编码数据；decoder 不访问 golden。
- F2 在指定同一 codeword 内，不能跨码字后仍预期 UE。
- 每条 runtime 报告对应某次真实检测，交付不早于 ready；无重复序号。
- 从 READY_UNPUBLISHED/SUSPECT 直接发生 consumer 数据读取是违规。
- QUIESCE_ACK 到 RESTORE_MAINTENANCE/显式恢复运行之间，目标数据访问不增加；REF 排除在该断言外。
- 老版本不被重启后的消费者使用。
- 扩展关闭时输出逐 bit 不变；同 feeder 下无行为插桩不改变模拟周期。
- `NATIVE` 不因插桩自动增加 ECC 行为。
- 期望失败、程序崩溃、超时、配置错误有不同退出/状态；不能都记为 recovery failure。

## 9. 指标、统计与图表

### 9.1 计算口径

- 通知延迟：`t_report-t_detect`，仅对真实交付事件计算；丢失另列分母与数量。
- 控制时间：`t_lock`（门禁生效）与 `t_quiesce_ack`（静止）分别报告，不互相替代。
- 成功及时控制率：按 case 计算，在首次不可撤销消费前门禁生效且没有违规消费的比例。没有报告的案例单列，并纳入“全部案例”汇总分母；另可报告有报告条件下的比例，必须标清条件。
- 基线窗口余量：`paired_no_hold_t_consume-t_lock`，只用于比较反事实机会，明确 baseline-derived；不得用永不消费的 H=indefinite case 计算实际 margin。
- 暂停延迟：`t_quiesce_ack-t_quiesce_request`；已静止时可为 0，但须从事件证明。
- 恢复延迟拆为输入搬运、控制/状态初始化、重执行；阶段重叠时不能把相加结果冒充端到端时间。
- 正常运行额外时间：`T_variant/T_same_feeder_EXT_OFF-1`。每个 seed 配对，不把不同输入的均值直接相除。
- 数据完整性：逐 bit 比较的 changed_elements、无效输出数；CE 与逃逸/未保护案例分开。
- 空间开销按 checkpoint、未发布输出、ECC parity、运行时元数据分别统计，不能重复计入同一物理 buffer。

连续指标报告每配置样本数、均值、中位数、min/max；主图平均值附按 seed 成组重采样的 95% bootstrap CI（10000 次、固定统计 seed=20261115）。同一运行的多条报告不是独立 seed。二项比例使用 Wilson 95% CI。零失效不意味着概率为零；有限样本的零漏报也不能推出绝对安全。30 个场景不报告 p99。确定性单事件协议检查无需虚构误差条。

统计实现约定：先对每个 seed 计算目标量，再在同一完整配置组内以 seed 为单位有放回抽样，取 bootstrap 均值分布的 2.5%/97.5% 分位数；配对差值先在同 seed 内求差再 bootstrap。比例 Wilson 使用 z=1.96，中心 `(p+z²/(2n))/(1+z²/n)`，半宽 `z*sqrt(p*(1-p)/n+z²/(4n²))/(1+z²/n)`。n=0 时结果为缺失，不画 0% 柱。summary 同时保存 numerator、denominator、n_seeds 和 CI 方法。

主图展示所有预先指定配置；失败运行以明确符号标识。缺失/超时不得直接删除后对剩余成功结果求均值；成功条件均值需附成功数/总数，并单独显示失败类别。

### 9.2 必交图表

| 图 | 数据与轴 | 要表达的结论 | 禁止的解读 |
|---|---|---|---|
| Fig.1 Path coverage | 4 profiles × 4 paths 分类矩阵；分 F1/F2 面板；格内标 corrected/reported/invalid/uncovered | 覆盖、报告与格式支持是不同能力 | 不把 model profile 当作商用芯片测量 |
| Fig.2 Report latency | (a) x=Configured transport delay (cycles), y=Observed notification latency (cycles)；(b) burst 下累计报告/队列等待 | 显式延迟与排队共同决定通知时点 | 曲线符合配置只验证实现，不能证明硬件延迟 |
| Fig.3 Control window | x=D/T_ref, y=H/T_ref，颜色=Timely-control fraction (%)；按 consumer 和规模分面 | 能否赶在消费前控制输出 | CE 迟到不等于已经输出错误；另附 output_changed |
| Fig.4 Quiescence | x=Outstanding limit，y=Quiescence latency (cycles)；分触发点；另面板独立 channel 进度 | 排空与暂停粒度的真实约束 | 不把停止时钟称为低成本暂停 |
| Fig.5 Restore | x=tile count，y=Recovery latency (cycles)；阶段分解与端到端标记；旁注正负例结果 | 可信输入/版本/状态复位是恢复前提 | 缺副本的拒绝恢复不是恢复成功 |
| Fig.6 Cost | (a) x=policy，y=Fault-free overhead (%)；(b) 分项空间 (KiB) | 常驻控制和暂存成本 | 不声称含未建模 ECC、host runtime 或互连成本 |

主文建议选择 Fig.1、Fig.3、Fig.4/5 中最能支持论点的图，其他放附录；交付必须生成全部六张。若某图所需接口无法实现，提交明确 blocked 原因和已有数据，不生成示意曲线代替实测曲线。

### 9.3 作图规范

- 仅使用 `results/figure_data/*.csv` 作图，不在绘图脚本硬编码结果。`captions.md` 给出每张图的数据来源、profile、时间单位、样本数、统计方法、失败处理和模型假设。
- Matplotlib 非交互后端 Agg；输出矢量 PDF、SVG 和 300 dpi PNG。设置 PDF 字体类型 42，白底，无渐变、立体柱状图、双 y 轴或装饰背景。
- 单栏宽 3.35 inch，双栏宽 7 inch；按最终版面检查 8–10 pt 字体可读。轴必须有单位，明确比例是 fraction 还是 %。
- 配色控制在 3–4 种，色盲友好；用线型/marker/纹理提供灰度区分。失败与缺失使用叉号/斜线，不涂成 0%。所有面板同含义用同颜色与图例。
- 延迟跨度过大可用对数坐标，但 D=0 单独标识，不把 0 改为 1 后隐瞒。热图按离散类别等宽显示 D/T_ref，不能暗示等距连续插值。
- 同条件对比统一 y 轴；柱状图默认从 0 起；若归一化，图注说明分母。
- 绘图脚本输出 plot manifest：输入 CSV SHA256、列名、过滤条件、样本数、聚合函数、输出文件哈希。
- 用 PNG 查看全部图的裁剪、文字、图例、色条、注释和误差条；能渲染不等于可读。至少检查单栏实际大小和灰度版本。发现缺标签或遮挡先修图再交付。

绘图脚本采用以下固定基础样式，按单栏/双栏调整图幅，不随意改变字体比例：

```python
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
plt.rcParams.update({
    "font.family": "DejaVu Sans", "font.size": 9,
    "axes.labelsize": 9, "axes.titlesize": 9,
    "xtick.labelsize": 8, "ytick.labelsize": 8, "legend.fontsize": 8,
    "pdf.fonttype": 42, "ps.fonttype": 42,
    "axes.spines.top": False, "axes.spines.right": False,
    "savefig.dpi": 300, "figure.dpi": 120,
})
COLORS = ["#0072B2", "#D55E00", "#009E73", "#666666"]
# 每张图的数据读取、校验、聚合和绘制均需实际实现；不能生成占位曲线。
```

## 10. 实现关卡：严格按顺序完成

每次只执行一个关卡。成功后写 STATUS.md，附命令、产物和检查结果；失败修复当前关，不改预期结果绕过失败。

| 关卡 | 工作内容 | 完成证据 |
|---|---|---|
| G0 | 固定源码、构建、原功能测试、环境锁定 | upstream.lock、构建日志、原测试 XML、二进制哈希 |
| G1 | 审计路径/时钟/布局，新 fixture 与 feeder，无故障运行 | source_audit、映射 round-trip、独立 FP16 参考一致 |
| G2 | 事件记录与数据协议；不改变行为 | EXT_OFF 同 feeder 周期/输出一致，schema/序号检查 |
| G3 | SECDED 独立模块与存储注入 | ECC 单元测试、C++/Python 交叉检查、一次 F1 witness |
| G4 | 全路径 ECC 与粗粒度报告 | P1-A/P1-B smoke 通过，位置无广播混淆 |
| G5 | 输出状态、host/device 门禁 | P1-C smoke、同周期边界与旁路访问负测 |
| G6 | 暂停、排空与两 channel 验证 | P1-D smoke，ACK 后数据访问为 0，REF 正常 |
| G7 | 可信输入恢复与状态复位 | P1-E 正例正确、五类负例正确拒绝 |
| G8 | 展开 paper 矩阵并完整执行 | manifest 预期数量=成功+预期负例+失败+超时，无漏项 |
| G9 | 统计、画图、论文式报告、干净复现 | 6 图及数据、结果报告、补丁重放 smoke、hash manifest |

G3 的 ECC 最小测试：数据取全 0、全 1、交替 01、交替 10 和 4 个固定随机 64-bit 值；每个值穷举 72 个单 bit 和 C(72,2)=2556 个双 bit 翻转。单 bit 必须返回原数据与 CE，双 bit 必须 UE；另做无故障编码/解码以及合法写后重新编码。Python 参考不要逐行翻译 C++，使用不同表示方式，例如 72 位列表与校验位置集合，独立检验。

G5 最小门禁测试：未发布 host 读被阻止、未发布 PIM consumer 未提交、producer 内部读写继续、报告后禁止 publish、旧句柄失效、直接旁路消费者触发断言。不要仅检查一个布尔变量。

G6 最小状态检查：分别统计 feeder pending、控制器队列、已发出数据返回、PIM 状态；如果只能证明整个 channel 暂停，就按 channel 能力交付，不自行宣称 bank 级暂停。

## 11. 启动、构建与运行命令

### 11.1 G0：可直接使用的上游获取步骤

以下在 Ubuntu Bash 执行。若目录已存在先验证其内容和 commit，不覆盖已有工作。

```bash
mkdir -p ~/pim-phase1/environment ~/pim-phase1/runs/baseline
cd ~/pim-phase1
sudo apt-get update
sudo apt-get install -y git build-essential scons libgtest-dev python3-venv python3-pip
git clone https://github.com/SAITPublic/PIMSimulator.git simulator
git -C simulator checkout --detach 3703d1f19c8f027360cc33a3243eb271e3bb6898
git -C simulator rev-parse HEAD
python3 -m venv .venv
.venv/bin/python -m pip install numpy pandas matplotlib pyyaml jsonschema pytest
.venv/bin/python -m pip freeze > environment/requirements.lock.txt
cd simulator
scons NO_STORAGE=0 NO_LIBRARY=1
./sim --gtest_list_tests
./sim --gtest_filter='PIMKernelFixture.*' --gtest_output=xml:../runs/baseline/upstream-tests.xml
```

bootstrap.sh 需把退出码、stdout/stderr 和环境写入文件，使用 `set -euo pipefail`，不能因为管道接 `tee` 吞掉测试失败。`git rev-parse HEAD` 必须等于固定 hash。网络失败保留错误并重试相同版本，不替换为不明 fork。

上游 Sconstruct 内部默认并行 32 jobs；内存不足时将该值改为从环境变量 `PHASE1_BUILD_JOBS` 读取、默认 4，记录为 build-only compatibility patch。未修改基线与后续版本使用同一构建修正。编译器兼容修复只允许最小改动；不删除失败功能测试。

若原测试依赖数据缺失，查固定提交的 `data/` 与生成脚本并按文档补齐；记录生成命令和数据哈希。原测试成功后执行新 fixture，不能用新测试成功替代上游回归。

### 11.2 必须实现的项目 CLI

以下是本项目要求实现的命令接口，不是声称当前仓库已有这些脚本：

```bash
# 均从 ~/pim-phase1 执行
bash scripts/build.sh
.venv/bin/python scripts/expand_suite.py --suite configs/suite_smoke.yaml
.venv/bin/python scripts/run_suite.py --manifest configs/generated/smoke-manifest.json --jobs 1
.venv/bin/python scripts/aggregate.py --runs runs/smoke --out results/smoke

# G0-G7 通过后
.venv/bin/python scripts/expand_suite.py --suite configs/suite_paper.yaml
.venv/bin/python scripts/run_suite.py --manifest configs/generated/paper-manifest.json --jobs 4 --resume
.venv/bin/python scripts/aggregate.py --runs runs/paper --out results
.venv/bin/python scripts/plot_paper.py --results results --out figures
.venv/bin/python scripts/package_artifact.py
```

单 case 执行约定：

```bash
PHASE1_CONFIG=/absolute/path/to/run/params.ini \
PHASE1_OUT=/absolute/path/to/run \
./sim --gtest_filter=Phase1Fixture.RunCase
```

环境变量只指向配置与输出路径；所有实验参数均出现在 config/params 中。C++ 新 fixture 不修改原 MainTest 的默认行为。ECC 单元测试用 `Phase1Ecc.*`，接口专项测试用 `Phase1Contract.*`。

每 case 一个独立进程和独立 working directory；上游全局 ConfigurationDB 不适合在一个进程反复切换 1/2/64 channel 配置。运行目录中提供需要的 ini/data 路径，禁用或重定向上游共享日志，避免并发运行互相覆盖。

run_id 为规范科学配置、source patch hash、binary hash、输入哈希的 SHA256 前 16 个十六进制字符，完整哈希写入 metadata；发现截断冲突则拒绝运行。规范科学配置包含实际 D/H/T_ref 与故障位置，但排除 run_id、输出目录、运行时间等派生字段，避免自引用哈希。`--resume` 仅跳过哈希一致、退出成功且 validate_run 通过的运行，不能仅凭目录存在跳过。

reproduce.sh 支持 `--smoke` 和 `--paper`；负责构建、验证、运行、聚合、绘图。正常结果的检查失败返回非零；预期负例由 assertion 通过，不依赖无差别忽略非零退出码。

artifact 打包排除虚拟环境、临时构建文件、上游完整 .git 和归档文件自身；包含补丁、锁文件、脚本、配置、原始实验日志/输出、图表源数据和报告。SHA256 清单不递归包含自身；归档的哈希单独写入最终 README。上游代码由 bootstrap 按固定提交获取，干净复现不能依赖作者机器上未打包的绝对路径。

## 12. 常见错误与固定处理办法

| 现象 | 首先检查 | 固定处理 |
|---|---|---|
| 所有 fault case 输出都正确 | 是否启用 NO_STORAGE；注入是否触及实际被读数据；是否每次注入都重新生成 parity | 对 raw mutation 前后字节与目标 access_id 做断言，先修路径 |
| LOCAL_VISIBLE 单 bit 输出改变 | PIM 路径是否绕过 decoder；codeword 布局、bit 编号是否一致 | 重跑 ECC 穷举，再用 W0 与 PREAD 定点追踪 |
| 每个 bank 都报告 bank0 | 使用了广播 packet 地址 | 用实际 Bank identity 建 key，验证三个不同 bank |
| 报告延迟都是 0 | 记录的是配置/函数调用时刻，未推进报告队列 | 从事件重新计算 detect/report；检查下一 cycle 交付边界 |
| 无在途事务时报告消失 | 原 runPIM 提前结束 | 在统一完成条件中加入报告/控制定时器 |
| hold 不增加 host 延迟却 PIM 仍读取 | 只隐藏回调，没阻止数据访问 | 补齐 host/PIM admission gate 和低层访问断言 |
| 暂停卡死 | 冻结时钟，或等冻结 feeder 清空，或 barrier 次序断裂 | 时钟与 REF 继续；只等待已接受的事务；保留顺序 |
| 恢复后偶发错误 | 输入版本、GRF/SRF/累加状态、payload 生命周期 | 核对恢复版本；重新初始化；为描述符分配稳定缓冲 |
| 命令完成但数值未写回 | 把提交回调当成物理写完成 | 追踪实际 storage write 与模式状态，用更强 idle 检查 |
| HOST_ONLY 写回导致大量假 CE | 沿用旧 parity | 标记 UNPROTECTED_FORMAT，不统计为扰动检测 |
| 图中只剩有利 case | 聚合脚本丢弃失败/NaN | manifest 对账，失败数和原因单列 |
| 图十分平滑但 run 数不符 | 对离散数据插值或使用占位结果 | 从 figure_data 重画，无插值、无补造值 |

不允许的“修复”：删除负例、放宽数值比较、隐藏失败样本、让 runtime 读取 truth、改成 host 计算替代 PIM、用新模拟器重建代替在线恢复、手工写预期 CSV、把参数 D 复制进 observed latency 字段。

## 13. 论文报告内容与结论边界

`phase1_results.md` 和 `.tex` 必须包含：

1. 研究问题与 Phase 1 在整体论文中的作用。
2. 固定平台、源代码版本、工作负载、故障、接口与时序假设。
3. 全路径验证方法，参考执行和 runtime/真值隔离方法。
4. 五组实验结果与成本，包括负例、无报告、迟到、失效副本。
5. 图表对应的具体发现，每个数字可追溯至 CSV 和 run_id。
6. 平台能力判定：可在模型内支持 Phase 2、需新增接口、或当前配置不可恢复。
7. 未建模因素：真实 ECC 布局/流水/能耗、操作系统通知路径、host CPU 处理、部分互连成本、实际 Rowhammer 产生概率、真实芯片差异等，按实际实现更新。

推荐结论形式：

> 在 LOCAL_VISIBLE 模型、给定报告粒度与延迟、受管输出和可信初始输入条件下，观察到……；在 LOCAL_HIDDEN/迟到报告/无可信副本条件下，……。结果验证了这些接口条件下的可恢复性前提，没有验证真实商品平台的接口可用性或普遍攻击防御保证。

所有“观察到”的位置必须替换成真实实验结论。禁止提前承诺收益为正。若报告及时但算法仍不能确定故障 codeword，不把该问题视为 Phase 1 失败；Phase 1 不承担唯一定位任务。

## 14. 最终验收清单

- [ ] 固定上游 commit 正确，原始与新增配置/源码/二进制可追溯。
- [ ] 原功能测试通过，存储模式启用，新增 feeder 无故障数值正确。
- [ ] ECC 单/双 bit 穷举通过，独立参考一致。
- [ ] host/PIM 读写覆盖与 profile 行为一致。
- [ ] 实际 bank/page 报告位置经过映射检查，未泄露真值。
- [ ] 报告交付来自事件队列，循环时钟与同周期顺序明确。
- [ ] host 和 PIM consumer 均无法读取未发布版本。
- [ ] 暂停确认后用户数据访问停止，刷新和独立 channel 行为有证据。
- [ ] 恢复使用独立可信输入，控制/寄存器状态恢复，数值逐 bit 正确。
- [ ] 所有预期负例被正确识别；contract pass 与 recoverability 分开。
- [ ] paper manifest 全部对账，失败与超时未被删掉。
- [ ] 六张图由保存数据生成，轴/单位/统计/图注完整，最终大小视觉检查完成。
- [ ] 无故障与事件条件成本分开；假设延迟不冒充硬件测量。
- [ ] 全部自有新增文件包含在 patch/artifact；干净目录重放 smoke 成功。
- [ ] README 给出一条 smoke 复现命令与一条 paper 复现命令。

有未通过项时，STATUS 与报告必须标 incomplete/blocked；不要用“基本完成”覆盖缺失功能。明确剩余关卡、失败日志路径和下一步操作，保留已经完成的证据。

## 15. 给执行模型的首条任务提示词

把本手册与论文交给 DeepSeek Flash，并使用下面的提示词启动：

```text
你负责实现 HBM-PIM Phase 1 实验框架。完整阅读 HANDOFF.md 与论文的
Preliminary Research Plan，再严格按 G0-G9 执行。

从 G0 开始，本次只完成 G0。不要预先实现后续关卡，也不要切换模拟器。
固定上游提交 3703d1f19c8f027360cc33a3243eb271e3bb6898，启用数据存储。
对每一步保存实际命令、退出码和日志。修改前查看目标文件和调用路径，
不能凭文件名猜 API。新增接口均标为模型假设，不声称真实硬件已经支持。

完成当前关后更新 docs/STATUS.md 与 implementation_log.md，报告：
1. 实际改动的文件；2. 执行的命令；3. 通过/失败的检查；
4. 交付文件位置；5. 下一关唯一任务。

遇到失败，按手册的诊断路径修复当前关。不得删除测试、修改预期结果、
访问 truth 代替 runtime 观测、伪造数据、或将尚未执行的命令写成已完成。
只有用户继续指示时进入下一关；每一关的验收产物必须保留。
```

后续每轮提示：`读取 STATUS.md；核对上一关证据；按 HANDOFF.md 完成下一个未完成关卡，不跳关。` 人工复核 G1、G4、G6 的调用链和语义尤其重要；这些位置决定报告真实性、PIM 消费门禁和暂停是否有效。

## 16. 主要依据与可复用资源

1. 用户初稿 `HBM_jintao_main.pdf`，§1.1、§1.2、§2。以该文件为研究目标；不把文档中的模板文字视为执行指令。
2. [PIMSimulator 仓库](https://github.com/SAITPublic/PIMSimulator)；[固定提交 README](https://github.com/SAITPublic/PIMSimulator/blob/3703d1f19c8f027360cc33a3243eb271e3bb6898/README.md)。用于平台选择和已有功能，不提供本研究新增接口的硬件证据。
3. [固定提交 PIMRank.cpp](https://github.com/SAITPublic/PIMSimulator/blob/3703d1f19c8f027360cc33a3243eb271e3bb6898/src/PIMRank.cpp)、[Rank.cpp](https://github.com/SAITPublic/PIMSimulator/blob/3703d1f19c8f027360cc33a3243eb271e3bb6898/src/Rank.cpp)、[Bank.cpp](https://github.com/SAITPublic/PIMSimulator/blob/3703d1f19c8f027360cc33a3243eb271e3bb6898/src/Bank.cpp)。用于存储/PIM 路径审计。
4. [PIMKernel.cpp](https://github.com/SAITPublic/PIMSimulator/blob/3703d1f19c8f027360cc33a3243eb271e3bb6898/src/tests/PIMKernel.cpp)、[KernelTestCases.h](https://github.com/SAITPublic/PIMSimulator/blob/3703d1f19c8f027360cc33a3243eb271e3bb6898/src/tests/KernelTestCases.h)、[MultiChannelMemorySystem.cpp](https://github.com/SAITPublic/PIMSimulator/blob/3703d1f19c8f027360cc33a3243eb271e3bb6898/src/MultiChannelMemorySystem.cpp)。用于命令生成、fixture、时钟和完成条件审计。
5. [Aquabolt-XL，Hot Chips 33，2021](https://hc33.hotchips.org/assets/program/conference/day1/20210813_HC33_Aquabolt-XL_PIM_Jin_Kim_slide.pdf)，system ECC 说明见第 22 页。支撑必须单独核实 PIM ECC 接口的动机，不支撑新增接口已商品化。
6. [Copy-on-Flip，NDSS 2023](https://www.ndss-symposium.org/wp-content/uploads/2023/02/ndss2023_f337_paper.pdf)。用于粗粒度报告与存储响应背景，本阶段不移植 CoF Linux 内核。
7. [Read Disturbance in HBM2，DSN 2024](https://arxiv.org/html/2310.14665v3)、[代码](https://github.com/CMU-SAFARI/HBM-Read-Disturbance)、[原始数据](https://zenodo.org/records/10257930)。后续可用于校准扰动故障分布；Phase 1 不需要下载完整数据集。

以上在线资源核验日期为 2026-11-15。手册内的 SECDED 编码、报告队列、控制时序和实验矩阵是本项目的设计规定，不能作为从上述论文直接复现的已发表结果。

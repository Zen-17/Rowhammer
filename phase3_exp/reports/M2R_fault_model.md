# M2-R 故障模型一致性

git commit `bd6b99c0ac428927b01a931e61f900fe9cffaa4b`。原始 M2 结果未覆盖。

## 三条刷新路径

1. 蒙特卡洛把一行的刷新周期记成 614400 次攻击激活。到点后只把受害行的 D(v) 清零。这是主安全统计使用的刷新。
2. 仿真器的观察器在每条 DRAM REF 上把侧指针前进两行，并对 16 个 bank 调用 `noteRefresh`。只有扰动模型挂着的时候才会清零。
3. M1 和原始 M2 的锤击阶段把扰动模型摘掉，再按 PIM 行号显式 `noteActivate`。这条路径在攻击期间不会被 REF 清零。原始 E3 的零翻转来自这条路径。

## 主实验约定

主结果 `PHASE3_REFRESH=on`：观察器保持挂接，不额外调用 `noteActivate`。`PHASE3_REFRESH=off` 只写入 `results/m2r/E3_ref_off_diagnostic.csv`，不进入主表。

探针（W2、NONE、A1、1/16 阈值）里，刷新开启时注入次数是 0；刷新关闭时同一模型注入 2 次，其中 1 次是首次跨阈。刷新开启时，计划锤击的两个相邻行在 bank 0 上的实际激活远少于攻击写次数。原因是主机写经过地址映射之后，并不对应 PIM 行号的相邻 DRAM 行。这次修订没有补回显式 `noteActivate`，因此没有把这个缺口修成翻转。

单元测试 `Phase3M2R.NeighborActAndVictimRefresh` 和 `Phase3M2R.InjectionRequiresWrittenData` 覆盖相邻行激活、受害行刷新清零、跨窗口后的首次翻转，以及没有写入数据的行不会记入注错。

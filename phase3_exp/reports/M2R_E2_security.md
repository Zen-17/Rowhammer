# M2-R E2 安全统计

git commit `bd6b99c0ac428927b01a931e61f900fe9cffaa4b`。种子 `MC_BASE_SEED + trial`，起点 20260000。每格 200 次。状态 SUCCESS。原始 `results/E2_mc_security.csv` 未改。

## 和原始 M2 的差别

原始 864 格把观察截止 614400 写进 `mttf_flip_mean`，所以热图是满格。截尾的直接原因是扫描框里的换位周期满足 `n*r + 2*(n-1) < 2*HC_min`，或者升级到 r1 之后一个换位纪元到不了抽样阈值。提前返回和快路径只在这个上界成立时使用。忠实复跑关掉这两条捷径，设计点 FIX 仍然在 614400 处右截尾，观察到的 D(v) 峰值低于阈值。NONE 对照在同一循环里发生翻转，所以模型能够产生失效。

受保护格子 864 个，其中零失效 864 个。NONE 格子 16 个，其中出现翻转的有 12 个。没有翻转的 NONE 格子是：n=16 A3 ratio 0.0001 REFRESH_HORIZON; n=64 A2 ratio 0.6591 REFRESH_HORIZON; n=16384 A2 ratio 0.0001 REFRESH_HORIZON; n=16384 A4 ratio 0.0000 REFRESH_HORIZON。零失效格子的有限期翻转概率是 0，95% Wilson 区间来自 `harness/stats.py`，零失效上界是 3/200 = 0.015。这些格子的 MTTF 留空，不把 614400 写成失效时间。

全部受保护设计都是零失效时，表里用扰动比上界和换位次数比较，不给 MTTF 差值。

忠实复跑（每格 2 次，捷径关闭）写在 `results/m2r/E2_faithful.json`：

```
[
  {
    "scheme": "NONE",
    "g": 1.5,
    "attack": "A1",
    "trials": 2,
    "flips": 2,
    "max_ratio": 1.0000050998066978
  },
  {
    "scheme": "NONE",
    "g": 1.5,
    "attack": "A3",
    "trials": 2,
    "flips": 1,
    "max_ratio": 1.0000050998066978
  },
  {
    "scheme": "FIX",
    "g": 1.5,
    "attack": "A1",
    "trials": 2,
    "flips": 0,
    "max_ratio": 0.07564559024156985
  },
  {
    "scheme": "FIX",
    "g": 1.5,
    "attack": "A3",
    "trials": 2,
    "flips": 0,
    "max_ratio": 0.008975434312592593
  },
  {
    "scheme": "ADA",
    "g": 1.5,
    "attack": "A1",
    "trials": 2,
    "flips": 0,
    "max_ratio": 0.07564559024156985
  },
  {
    "scheme": "ADA",
    "g": 1.5,
    "attack": "A3",
    "trials": 2,
    "flips": 0,
    "max_ratio": 0.11318495059458869
  },
  {
    "scheme": "OURS",
    "g": 1.1,
    "attack": "A1",
    "trials": 2,
    "flips": 0,
    "max_ratio": 0.103863096010282
  },
  {
    "scheme": "OURS",
    "g": 1.1,
    "attack": "A3",
    "trials": 2,
    "flips": 0,
    "max_ratio": 0.1654629188714438
  }
]
```

## 设计点

n=64、c=2、g=1.5 与 g=1.1 没有改。在这个扫描框里它们落在安全区内，所以分不出 MTTF。把 c 降到会翻转的区域需要人工批准，这次没有做。

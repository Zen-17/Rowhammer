# pim-phase2

Phase 2 of the HBM-PIM study: compute-side dependency tracking and selective
recovery. This directory is a **thin sibling** of `../pim-phase1`:

- Phase 1 remains the single source of truth for the base simulator
  (`../pim-phase1/simulator`), its device/system INI files, and the Phase 1
  calibration inputs (`../pim-phase1/results/g7`, `../pim-phase1/results/figure_data`).
- `pim-phase2` owns the Phase 2 engine, tests, scripts, configs, runs, results,
  figures and report.
- `overlay/src/` contains the Phase 2 C++ modules and the four modified Phase 1
  hook files. `scripts/phase2/build_phase2.sh` copies the Phase 1 simulator
  source into the generated `build/simulator`, applies the overlay, and builds
  `build/simulator/sim`.
- `simulator` is a symlink to `build/simulator`, so all Phase 2 scripts keep
  using the `ROOT/simulator/...` convention.
- `.venv`, `environment/source.patch`, `environment/environment.json`, and the
  Phase 1 INI files are symlinks into `../pim-phase1`.

## Build

```bash
cd /opt/data/data/workspace-dynamic/Rowhammer/pim-phase2
bash scripts/phase2/build_phase2.sh
```

## Reproduce

v2:

```bash
cd /opt/data/data/workspace-dynamic/Rowhammer/pim-phase2
JOBS=$(nproc) bash scripts/phase2/reproduce_phase2.sh
```

v2.1:

```bash
cd /opt/data/data/workspace-dynamic/Rowhammer/pim-phase2
JOBS=$(nproc) bash scripts/phase2/reproduce_phase2.sh --version v21
```

## Layout

| Path | Contents |
|---|---|
| `phase2/` | Python reference engine and L2 evaluator |
| `tests/phase2/` | Python L1 and differential tests |
| `overlay/src/` | C++ Phase 2 modules and modified Phase 1 hooks |
| `scripts/phase2/` | build, run, aggregate, plot, validation scripts |
| `scripts/run_*.py` | Phase 1 validators reused for the 240-case regression |
| `configs/phase2/` | L2/L3 suite configs, calibration and L3 refit |
| `configs/phase2/v21/` | v2.1 L2/L3 suites and per-kernel calibration |
| `runs/phase2/` | v2 Stage 0 snapshots and L3 run directories |
| `runs/phase2/v21/` | v2.1 L3 run directories and regression |
| `results/phase2/` | v2 L2/L3 case data, figure_data, statistics, validation |
| `results/phase2/v21/` | v2.1 L2/L3 data, figure_data, statistics, validation |
| `figures/phase2/` | v2 P2-F1..F6, P2-T1, captions and plot manifest |
| `figures/phase2/v21/` | v2.1 P2-F1..F7, P2-T1/T2, captions and manifest |
| `docs/` | status, decisions, mount-point audit, v2.1 preflight |
| `report/phase2_results.md` | v2 report |
| `report/phase2_results_v21.md` | v2.1 report |


"""Deterministic workload traces for the L2 evaluator.

W1 is the Phase 1 add kernel, W2 is ADD -> MUL -> RELU with three sequential
kernels, and W3 is a synthetic fan-in DAG whose per-tile cost reuses W1's
calibrated cost (ASSUMED).  Times follow the plan's split-overhead model.
"""
from __future__ import annotations

import math
from dataclasses import dataclass
from typing import Dict, Iterable, List, Optional, Sequence, Tuple

from .layout import (
    Layout,
    OPERAND_A,
    OPERAND_B,
    OPERAND_O,
    OPERAND_T,
    OPERAND_U,
    OPERAND_W3_INPUT_BASE,
    OPERAND_W3_OUTPUT,
    OPERAND_X,
    OPERAND_Y,
    WorkloadLayouts,
)


def iround(value: float) -> int:
    return int(math.floor(value + 0.5))


@dataclass(frozen=True)
class OpSpec:
    job_id: int
    kernel_idx: int
    tile: int
    inputs: Tuple[Tuple[int, int], ...]
    outputs: Tuple[Tuple[int, int], ...]
    t_issue: int
    t_complete: int

    @property
    def item(self) -> Tuple[Tuple[int, int], ...]:
        return self.inputs


@dataclass(frozen=True)
class Schedule:
    name: str
    layout: Layout
    ops: Tuple[OpSpec, ...]
    kernel_count: int
    n_tiles: int
    fan_in: int
    job_end: int

    def op_id(self, index: int) -> int:
        return index + 1

    def index_of(self, kernel_idx: int, tile: int) -> int:
        for index, op in enumerate(self.ops):
            if op.kernel_idx == kernel_idx and op.tile == tile:
                return index
        raise KeyError((kernel_idx, tile))

    def op(self, index: int) -> OpSpec:
        return self.ops[index]

    def producers(self) -> Dict[Tuple[int, int], int]:
        result: Dict[Tuple[int, int], int] = {}
        for index, op in enumerate(self.ops):
            for item in op.outputs:
                result[item] = index
        return result

    def readers(self, item: Tuple[int, int]) -> List[int]:
        return [index for index, op in enumerate(self.ops) if item in op.inputs]

    def final_items(self) -> List[Tuple[int, int]]:
        return [(self.layout.final_operand, tile) for tile in range(self.n_tiles)]

    def initial_inputs(self) -> List[Tuple[int, int]]:
        produced = set(self.producers())
        items = set()
        for op in self.ops:
            for item in op.inputs:
                if item not in produced:
                    items.add(item)
        return sorted(items)

    def producer_index(self, item: Tuple[int, int]) -> Optional[int]:
        for index, op in enumerate(self.ops):
            if item in op.outputs:
                return index
        return None


def _kernel_times(kernel_start: int, n_tiles: int, c_fix: float,
                  c_tile: float) -> List[Tuple[int, int]]:
    times = []
    for tile in range(n_tiles):
        issue = iround(kernel_start + c_fix / 2.0 + tile * c_tile)
        complete = iround(kernel_start + c_fix / 2.0 + (tile + 1) * c_tile)
        times.append((issue, complete))
    return times


def _kernel_end(kernel_start: int, n_tiles: int, c_fix: float, c_tile: float) -> int:
    return iround(kernel_start + c_fix + n_tiles * c_tile)


def build_schedule(workload: str, n_tiles: int, c_fix: float, c_tile: float,
                   job_id: int = 1,
                   kernel_costs: Optional[Sequence[Tuple[float, float]]] = None) -> Schedule:
    n_tiles = int(n_tiles)
    if n_tiles <= 0:
        raise ValueError("n_tiles must be positive")

    def costs_for(kernel_idx: int) -> Tuple[float, float]:
        if kernel_costs is not None and kernel_idx < len(kernel_costs):
            return float(kernel_costs[kernel_idx][0]), float(kernel_costs[kernel_idx][1])
        return float(c_fix), float(c_tile)

    if workload == "W1":
        layout = WorkloadLayouts.default().w1
        fix0, tile0 = costs_for(0)
        times = _kernel_times(0, n_tiles, fix0, tile0)
        ops = tuple(
            OpSpec(job_id, 0, tile,
                   ((OPERAND_A, tile), (OPERAND_X, tile)),
                   ((OPERAND_B, tile),), issue, complete)
            for tile, (issue, complete) in enumerate(times))
        return Schedule("W1", layout, ops, 1, n_tiles, 0,
                        _kernel_end(0, n_tiles, fix0, tile0))
    if workload == "W2":
        layout = WorkloadLayouts.default().w2
        fix0, tile0 = costs_for(0)
        fix1, tile1 = costs_for(1)
        fix2, tile2 = costs_for(2)
        add_times = _kernel_times(0, n_tiles, fix0, tile0)
        mul_start = _kernel_end(0, n_tiles, fix0, tile0)
        mul_times = _kernel_times(mul_start, n_tiles, fix1, tile1)
        relu_start = _kernel_end(mul_start, n_tiles, fix1, tile1)
        relu_times = _kernel_times(relu_start, n_tiles, fix2, tile2)
        ops = []
        for tile, (issue, complete) in enumerate(add_times):
            ops.append(OpSpec(job_id, 0, tile,
                              ((OPERAND_A, tile), (OPERAND_X, tile)),
                              ((OPERAND_T, tile),), issue, complete))
        for tile, (issue, complete) in enumerate(mul_times):
            ops.append(OpSpec(job_id, 1, tile,
                              ((OPERAND_T, tile), (OPERAND_Y, tile)),
                              ((OPERAND_U, tile),), issue, complete))
        for tile, (issue, complete) in enumerate(relu_times):
            ops.append(OpSpec(job_id, 2, tile,
                              ((OPERAND_U, tile),),
                              ((OPERAND_O, tile),), issue, complete))
        return Schedule("W2", layout, tuple(ops), 3, n_tiles, 0,
                        _kernel_end(relu_start, n_tiles, fix2, tile2))
    if workload.startswith("W3"):
        fan_in = int(workload.split("_K", 1)[1])
        layout = WorkloadLayouts.w3(fan_in)
        fix0, tile0 = costs_for(0)
        times = _kernel_times(0, n_tiles, fix0, tile0)
        ops = []
        for tile, (issue, complete) in enumerate(times):
            inputs = tuple((OPERAND_W3_INPUT_BASE + m, (tile + m) % n_tiles)
                           for m in range(fan_in))
            ops.append(OpSpec(job_id, 0, tile, inputs,
                              ((OPERAND_W3_OUTPUT, tile),), issue, complete))
        return Schedule(workload, layout, tuple(ops), 1, n_tiles, fan_in,
                        _kernel_end(0, n_tiles, fix0, tile0))
    if workload.startswith("W4_F"):
        fan_in = int(workload.split("_F", 1)[1])
        layout = WorkloadLayouts.w4(fan_in)
        fix0, tile0 = costs_for(0)
        fix1, tile1 = costs_for(1)
        fix2, tile2 = costs_for(2)
        add_times = _kernel_times(0, n_tiles, fix0, tile0)
        mul_start = _kernel_end(0, n_tiles, fix0, tile0)
        mul_times = _kernel_times(mul_start, n_tiles, fix1, tile1)
        relu_start = _kernel_end(mul_start, n_tiles, fix1, tile1)
        relu_times = _kernel_times(relu_start, n_tiles, fix2, tile2)
        ops = []
        for tile, (issue, complete) in enumerate(add_times):
            ops.append(OpSpec(job_id, 0, tile,
                              ((OPERAND_A, tile), (OPERAND_X, tile)),
                              ((OPERAND_T, tile),), issue, complete))
        for tile, (issue, complete) in enumerate(mul_times):
            inputs = tuple((OPERAND_T, (tile + m) % n_tiles) for m in range(fan_in))
            ops.append(OpSpec(job_id, 1, tile, inputs,
                              ((OPERAND_U, tile),), issue, complete))
        for tile, (issue, complete) in enumerate(relu_times):
            ops.append(OpSpec(job_id, 2, tile,
                              ((OPERAND_U, tile),),
                              ((OPERAND_O, tile),), issue, complete))
        return Schedule(workload, layout, tuple(ops), 3, n_tiles, fan_in,
                        _kernel_end(relu_start, n_tiles, fix2, tile2))
    raise ValueError(f"unknown workload: {workload}")


def fault_operands(workload: str) -> List[int]:
    if workload == "W1":
        return [OPERAND_A, OPERAND_X]
    if workload == "W2":
        return [OPERAND_A, OPERAND_Y, OPERAND_T]
    if workload.startswith("W3"):
        return [OPERAND_W3_INPUT_BASE]
    if workload.startswith("W4_F"):
        return [OPERAND_A, OPERAND_T]
    raise ValueError(workload)


def kernel_count(workload: str) -> int:
    if workload == "W1" or workload.startswith("W3"):
        return 1
    if workload == "W2" or workload.startswith("W4_F"):
        return 3
    raise ValueError(workload)


def run_cost(runs: Iterable[dict], c_fix: float, c_tile: float,
             c_fix_by_kernel: Sequence[float] = (),
             c_tile_by_kernel: Sequence[float] = ()) -> float:
    total = 0.0
    for run in runs:
        kernel = int(run["kernel_idx"])
        fix = c_fix_by_kernel[kernel] if kernel < len(c_fix_by_kernel) else c_fix
        tile = c_tile_by_kernel[kernel] if kernel < len(c_tile_by_kernel) else c_tile
        total += fix + run["tile_count"] * tile
    return total


def full_job_cost(workload: str, n_tiles: int, c_fix: float, c_tile: float) -> float:
    return kernel_count(workload) * (c_fix + n_tiles * c_tile)

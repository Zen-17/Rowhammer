"""Calibrated timing and cost model for the L2 evaluator."""
from __future__ import annotations

import json
import math
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Sequence, Tuple

from .engine import (
    CheckpointView,
    DataRef,
    Decision,
    ExecutionPlan,
    Gate,
    Ledger,
    ancestor_complete_time,
    plan_execution,
)
from .layout import Layout
from .workloads import OpSpec, Schedule, iround, run_cost


@dataclass(frozen=True)
class CostModel:
    c_tile: float
    c_fix: float
    c_restore_per_byte: float
    c_restore_fix: float
    c_quiesce: float
    c_host_fix: float
    c_host_per_tile: float
    kernel_costs_by_workload: Optional[Dict[str, Tuple[Tuple[float, float], ...]]] = None

    @staticmethod
    def from_calibration(path: Path) -> "CostModel":
        payload = json.loads(Path(path).read_text())
        params = payload["parameters"]
        consumer = payload.get("consumer_model") or {}
        host = consumer.get("parameters", {})
        raw_kernel_costs = payload.get("kernel_costs") or {}
        kernel_costs: Dict[str, Tuple[Tuple[float, float], ...]] = {}
        for workload, entries in raw_kernel_costs.items():
            kernel_costs[str(workload)] = tuple(
                (float(entry[0]), float(entry[1])) for entry in entries)
        return CostModel(
            c_tile=float(params["c_tile"]),
            c_fix=float(params["c_fix"]),
            c_restore_per_byte=float(params["c_restore_per_byte"]),
            c_restore_fix=float(params["c_restore_fix"]),
            c_quiesce=float(params["c_quiesce"]),
            c_host_fix=float(host.get("c_host_fix", 20.142857142856883)),
            c_host_per_tile=float(host.get("c_host_per_tile", 692.8571428571429)),
            kernel_costs_by_workload=kernel_costs,
        )

    def kernel_costs_for(self, workload: str) -> Tuple[Tuple[float, float], ...]:
        if not self.kernel_costs_by_workload:
            return ()
        return self.kernel_costs_by_workload.get(workload, ())


def detect_and_ack(schedule: Schedule, item: Tuple[int, int],
                   transport_delay: int, costs: CostModel) -> dict:
    producer = schedule.producer_index(item)
    injection = 0 if producer is None else schedule.op(producer).t_complete
    version = 0 if producer is None else schedule.op_id(producer)
    readers = [index for index, op in enumerate(schedule.ops)
               if item in op.inputs and op.t_issue >= injection]
    if not readers:
        raise ValueError(f"item {item} has no reader after injection")
    first = min(readers, key=lambda index: schedule.op(index).t_issue)
    detect = iround(schedule.op(first).t_issue + costs.c_tile / 2.0)
    return {
        "producer_index": producer,
        "version": version,
        "injection_cycle": injection,
        "first_reader_index": first,
        "detect_cycle": detect,
        "report_cycle": int(detect + transport_delay),
        "ack_cycle": int(detect + transport_delay + costs.c_quiesce),
    }


def build_ledger(schedule: Schedule, t_ack: int,
                 max_log_entries: int = 100000) -> Ledger:
    ledger = Ledger(schedule.layout, max_log_entries=max_log_entries)
    for spec in schedule.ops:
        op_id = ledger.plan_op(spec.job_id, spec.kernel_idx, spec.tile,
                               list(spec.inputs), list(spec.outputs))
        if spec.t_issue <= t_ack:
            ledger.record_issue(op_id, spec.t_issue)
            if spec.t_complete <= t_ack:
                ledger.record_complete(op_id, spec.t_complete)
    return ledger


def runs_for_op_ids(schedule: Schedule, op_ids: Iterable[int]) -> List[dict]:
    selected = [schedule.op(op_id - 1) for op_id in sorted(set(op_ids))
                if 1 <= op_id <= len(schedule.ops)]
    runs: List[dict] = []
    for spec in sorted(selected, key=lambda op: (op.kernel_idx, op.tile)):
        if (runs and runs[-1]["kernel_idx"] == spec.kernel_idx
                and runs[-1]["tile_begin"] + runs[-1]["tile_count"] == spec.tile):
            runs[-1]["tile_count"] += 1
        else:
            runs.append({"job_id": spec.job_id, "kernel_idx": spec.kernel_idx,
                         "tile_begin": spec.tile, "tile_count": 1})
    return runs


def _effective_runs(plan: ExecutionPlan, decision: Decision, schedule: Schedule,
                    fallback_full: bool) -> List[dict]:
    if fallback_full:
        return runs_for_op_ids(schedule, decision.affected)
    return plan.replay_runs


def recovery_cost(decision: Decision, plan: ExecutionPlan, schedule: Schedule,
                  costs: CostModel, ack_cycle: int, release: str = "GLOBAL_HOLD",
                  delta_cycles: int = 0, hold_cycles: int = 0,
                  fallback_full: bool = False) -> dict:
    quiesce = costs.c_quiesce
    restore = 0.0
    if plan.restore_items:
        restore = costs.c_restore_fix + plan.restore_bytes * costs.c_restore_per_byte
    replay_runs = _effective_runs(plan, decision, schedule, fallback_full)
    kernel_costs = costs.kernel_costs_for(schedule.name)
    kernel_fix = tuple(cost[0] for cost in kernel_costs)
    kernel_tile = tuple(cost[1] for cost in kernel_costs)
    replay = run_cost(replay_runs, costs.c_fix, costs.c_tile, kernel_fix, kernel_tile)
    resume = run_cost(plan.remaining_runs, costs.c_fix, costs.c_tile,
                      kernel_fix, kernel_tile)
    end_of_replay = ack_cycle + restore + replay + resume
    publish = 0.0
    has_affected_output = bool(decision.affected or decision.suspect_items
                               or decision.radius_suspect_items)
    if decision.status != "OUTSIDE_WINDOW_NO_TRUSTED_INPUT" and has_affected_output:
        if release == "GLOBAL_HOLD":
            global_end = max(schedule.job_end, end_of_replay)
            publish = max(0.0, global_end + hold_cycles - end_of_replay)
        elif release == "DEP_RELEASE":
            publish = float(max(0, delta_cycles))
        else:
            raise ValueError(release)
    recovery = quiesce + restore + replay + resume + publish
    recovery_excl_resume = quiesce + restore + replay + publish
    return {
        "quiesce_cycles": quiesce,
        "restore_cycles": restore,
        "replay_cycles": replay,
        "resume_cycles": resume,
        "publish_cycles": publish,
        "recovery_cycles": recovery,
        "recovery_cycles_excl_resume": recovery_excl_resume,
        "end_to_end_cycles": ack_cycle + restore + replay + resume + publish,
        "replay_tiles": sum(run["tile_count"] for run in replay_runs),
        "replay_runs": len(replay_runs),
        "coalesced_extra_tiles": plan.coalesced_extra_tiles,
        "restore_items": plan.restore_items,
        "restore_bytes": plan.restore_bytes,
        "replayed_ops": (sorted(decision.affected) if fallback_full
                         else plan.replayed_ops),
    }


def b0_cost(schedule: Schedule, ack_cycle: int, costs: CostModel,
            initial_items: int) -> dict:
    restore_bytes = 4096 * initial_items
    restore = (costs.c_restore_fix + restore_bytes * costs.c_restore_per_byte
               if restore_bytes else 0.0)
    replay = 0.0
    kernel_costs = costs.kernel_costs_for(schedule.name)
    for kernel_idx in range(schedule.kernel_count):
        if kernel_idx < len(kernel_costs):
            fix, tile = kernel_costs[kernel_idx]
        else:
            fix, tile = costs.c_fix, costs.c_tile
        replay += fix + schedule.n_tiles * tile
    recovery = costs.c_quiesce + restore + replay
    return {
        "quiesce_cycles": costs.c_quiesce,
        "restore_cycles": restore,
        "replay_cycles": replay,
        "resume_cycles": 0.0,
        "publish_cycles": 0.0,
        "recovery_cycles": recovery,
        "recovery_cycles_excl_resume": recovery,
        "end_to_end_cycles": ack_cycle + restore + replay,
        "replay_tiles": schedule.kernel_count * schedule.n_tiles,
        "replay_runs": schedule.kernel_count,
        "coalesced_extra_tiles": 0,
        "restore_items": initial_items,
        "restore_bytes": restore_bytes,
        "replayed_ops": [index + 1 for index in range(len(schedule.ops))],
    }


def consumer_makespan(publish: Dict[Tuple[int, int], int], order: Sequence[Tuple[int, int]],
                      costs: CostModel) -> float:
    end = 0.0
    for item in order:
        start = max(float(publish[item]), end)
        end = start + costs.c_host_per_tile
    return costs.c_host_fix + end


def release_experiment(schedule: Schedule, release: str, delta_cycles: int,
                       costs: CostModel) -> dict:
    """Steady-state publication cost with PIM/host mode mutual exclusion (Fix-5).

    Host consumes only final outputs.  A final output is readable at
    ``max(t_release, t_exit)``; the PIM rank cannot serve host reads while a
    kernel is running.  The release condition is strict (``>``), matching Gate.
    """
    order = schedule.final_items()
    t_exit = schedule.job_end
    baseline_publish = {item: t_exit for item in order}
    baseline = consumer_makespan(baseline_publish, order, costs)
    publish: Dict[Tuple[int, int], int] = {}
    if release == "GLOBAL_HOLD":
        for item in order:
            publish[item] = t_exit + delta_cycles + 1
    elif release == "DEP_RELEASE":
        ledger = build_ledger(schedule, t_exit + 1, max_log_entries=len(schedule.ops))
        memo: Dict[int, int] = {}
        producers = schedule.producers()
        for item in order:
            producer = producers[item]
            release_cycle = ancestor_complete_time(ledger, producer + 1, memo) \
                + delta_cycles
            publish[item] = max(release_cycle, t_exit) + 1
    else:
        raise ValueError(release)
    makespan = consumer_makespan(publish, order, costs)
    return {
        "baseline_end_to_end_cycles": baseline,
        "end_to_end_cycles": makespan,
        "overhead_fraction": max(0.0, (makespan - baseline) / baseline),
        "job_end_cycles": schedule.job_end,
        "delta_cycles": delta_cycles,
        "delta_over_job": delta_cycles / schedule.job_end,
    }


def ledger_summary(schedule: Schedule, release: str, delta_cycles: int,
                   costs: CostModel) -> dict:
    ledger = build_ledger(schedule, schedule.job_end + 1, max_log_entries=len(schedule.ops))
    produced: List[Tuple[int, int, int, int]] = []
    for op in schedule.ops:
        for item in op.outputs:
            produced.append((op.t_complete, op.t_issue, item[0], item[1]))
    if release == "GLOBAL_HOLD":
        release_times = [schedule.job_end + delta_cycles + 1 for _ in produced]
    elif release == "DEP_RELEASE":
        memo: Dict[int, int] = {}
        release_times = []
        for index, op in enumerate(schedule.ops):
            for _item in op.outputs:
                release_times.append(
                    int(ancestor_complete_time(ledger, index + 1, memo))
                    + delta_cycles + 1)
    else:
        raise ValueError(release)
    events: List[Tuple[int, int]] = []
    for (produce, _issue, _op, _tile), release_time in zip(produced, release_times):
        events.append((produce, 1))
        events.append((release_time, -1))
    events.sort(key=lambda item: (item[0], -item[1]))
    held = 0
    peak = 0
    for _cycle, change in events:
        held += change
        peak = max(peak, held)
    return {
        "entries": len(ledger.ops),
        "bytes_est": ledger.bytes_estimate(),
        "produced_items": len(produced),
        "peak_held_items": peak,
    }

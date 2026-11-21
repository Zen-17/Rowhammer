"""L2 case evaluator: synthetic trace -> engine decision -> cost and correctness.

This module is the evaluator; it is the only place allowed to know the fault
coordinates.  The recovery engine modules never import it.
"""
from __future__ import annotations

import hashlib
from dataclasses import dataclass, field
from typing import Dict, Iterable, List, Optional, Sequence, Set, Tuple

from .engine import (
    CheckpointView,
    DataRef,
    Decision,
    EngineConfig,
    Gate,
    Ledger,
    POLICIES,
    POLICY_B1,
    POLICY_B2,
    POLICY_B2R,
    STATUS_BASELINE,
    STATUS_FALLBACK,
    STATUS_NO_TRUSTED,
    STATUS_SOURCE_ONLY,
    WINDOW_AUTO,
    analyze,
    plan_execution,
    plan_whole_job,
)
from .layout import (
    Geometry,
    Layout,
    OPERAND_A,
    OPERAND_T,
    PublicReport,
    Scheme8Codec,
    WorkloadLayouts,
    codeword_region_id,
    page_region_id,
)
from .timing import (
    CostModel,
    b0_cost,
    build_ledger,
    detect_and_ack,
    ledger_summary,
    recovery_cost,
    release_experiment,
    runs_for_op_ids,
)
from .workloads import Schedule, build_schedule, fault_operands, iround, kernel_count


@dataclass
class CaseSpec:
    kind: str
    workload: str
    n_tiles: int
    seed: int
    granularity: str = "page"
    error_class: str = "UE"
    fault_operand: int = 0
    fault_tile: int = 0
    policy: str = "B1"
    transport_delay: int = 256
    engine_delta: Optional[int] = None
    max_log_entries: int = 1000000
    checkpoint_missing: Optional[Tuple[int, int]] = None
    report_overflow: bool = False
    early_consume: bool = False
    status_scenario: str = ""
    silent_distance: str = ""
    silent_tile: int = -1
    delta_ratio: float = 0.0
    release: str = ""
    note: str = ""
    window_mode: str = WINDOW_AUTO
    same_cycle_release: bool = False
    baseline_end_to_end: Optional[float] = None


def deterministic_tile(workload: str, operand: int, seed: int, n_tiles: int) -> int:
    digest = hashlib.sha256(f"{workload}|{operand}|{seed}".encode()).digest()
    return int.from_bytes(digest[:8], "big") % int(n_tiles)


def tile_row(tile: int) -> int:
    return (8 * tile) // 32


def tile_distance_class(primary_tile: int, silent_tile: int) -> str:
    delta = tile_row(silent_tile) - tile_row(primary_tile)
    if delta == 0:
        return "same_row" if silent_tile != primary_tile else "self"
    if delta == 1:
        return "plus1"
    if delta == -1:
        return "minus1"
    if delta == 2:
        return "plus2"
    if delta == -2:
        return "minus2"
    return "far"


def choose_silent_tile(primary_tile: int, distance: str, n_tiles: int) -> int:
    row = tile_row(primary_tile)
    max_row = (n_tiles - 1) // 4
    if distance == "same_row":
        candidates = [t for t in range(max(0, row * 4), min(n_tiles, row * 4 + 4))
                      if t != primary_tile]
        if not candidates:
            raise ValueError("no same-row candidate")
        return candidates[0]
    if distance in ("plus1", "minus1", "plus2", "minus2"):
        sign = 1 if distance.startswith("plus") else -1
        step = 1 if distance.endswith("1") else 2
        target_row = row + sign * step
        if 0 <= target_row <= max_row:
            return min(n_tiles - 1, target_row * 4)
        target_row = row - sign * step
        if 0 <= target_row <= max_row:
            return min(n_tiles - 1, target_row * 4)
        raise ValueError(f"no tile at distance {distance}")
    if distance == "far":
        target_row = 0 if row >= 3 else max_row
        if abs(target_row - row) <= 2:
            raise ValueError("far row is not far enough")
        return min(n_tiles - 1, target_row * 4)
    raise ValueError(distance)


def build_report(layout: Layout, item: Tuple[int, int], granularity: str,
                 cycle: int, error_class: str, codec: Scheme8Codec,
                 seq: int = 1) -> PublicReport:
    operand, tile = item
    if granularity == "bank":
        region_id = 0
    elif granularity == "page":
        if operand not in layout.operands():
            raise ValueError("operand outside layout")
        region_id = page_region_id(layout, operand, tile)
    elif granularity == "codeword":
        row_offset, first_col = layout.tile_position(tile)
        region_id = codeword_region_id(codec.geometry, 0,
                                       layout.base_row(operand) + row_offset,
                                       first_col, 0)
    else:
        raise ValueError(granularity)
    return PublicReport(seq, int(cycle), error_class, granularity, int(region_id))


def engine_policy(policy: str) -> Tuple[str, int]:
    if policy == "B1":
        return POLICY_B1, 0
    if policy == "B2":
        return POLICY_B2, 0
    if policy.startswith("B2R-"):
        return POLICY_B2R, int(policy.split("-", 1)[1])
    raise ValueError(policy)


def oracle_decision(schedule: Schedule, t_ack: int, bad_items: Sequence[Tuple[int, int]],
                    error_class: str,
                    checkpoint: CheckpointView) -> Decision:
    produced = schedule.producers()
    affected: Set[int] = set()
    restore: Dict[Tuple[int, int], DataRef] = {}
    regenerate: Set[int] = set()
    missing = False
    for item in bad_items:
        producer = produced.get(item)
        if producer is None:
            if checkpoint.has(item[0], item[1]):
                restore[item] = DataRef(item[0], item[1], 0)
            else:
                missing = True
        else:
            regenerate.add(producer + 1)
    if error_class == "UE":
        for item in bad_items:
            for reader in schedule.readers(item):
                if schedule.op(reader).t_complete <= t_ack:
                    affected.add(reader + 1)
    affected |= regenerate
    queue = sorted(affected)
    while queue:
        op_id = queue.pop(0)
        spec = schedule.op(op_id - 1)
        for item in spec.outputs:
            for reader in schedule.readers(item):
                if schedule.op(reader).t_complete <= t_ack and reader + 1 not in affected:
                    affected.add(reader + 1)
                    queue.append(reader + 1)
    missing = missing or any(
        not checkpoint.has(ref.operand, ref.tile) for ref in restore.values())
    remaining = [index + 1 for index, op in enumerate(schedule.ops)
                 if op.t_complete > t_ack]
    status = STATUS_NO_TRUSTED if missing else (
        "SELECTIVE_RECOVERED" if affected else STATUS_SOURCE_ONLY)
    return Decision(
        status=status,
        suspect_items=[DataRef(item[0], item[1],
                               0 if item not in produced else produced[item] + 1)
                       for item in bad_items],
        seeds=[],
        regenerate=sorted(regenerate),
        affected=sorted(affected),
        restore_list=sorted(restore.values()),
        escaped_outputs=[],
        remaining=sorted(remaining),
        fallback_reasons=[],
    )


def _taint_analysis(schedule: Schedule, bad_items: Sequence[Tuple[int, int]],
                    error_class: str, t_ack: int, decision: Decision,
                    plan, fallback_full: bool, checkpoint: CheckpointView,
                    b0: bool) -> dict:
    if b0:
        replayed = {index + 1 for index in range(len(schedule.ops))}
        remaining: Set[int] = set()
    else:
        replayed = set(plan.replayed_ops)
        if fallback_full:
            replayed = {index + 1 for index, op in enumerate(schedule.ops)
                        if op.t_complete <= t_ack}
        remaining = set(decision.remaining)

    restored_items = {(ref.operand, ref.tile) for ref in decision.restore_list
                      if ref.version == 0}
    repaired_items: Set[Tuple[int, int]] = set()
    for item in bad_items:
        producer = schedule.producer_index(item)
        if producer is None:
            if item in restored_items or (b0 and checkpoint.has(item[0], item[1])):
                repaired_items.add(item)
        else:
            op_id = producer + 1
            if op_id in replayed or op_id in remaining or b0:
                repaired_items.add(item)

    tainted: Set[Tuple[int, int]] = set()
    if error_class == "UE":
        tainted.update(bad_items)
    for index, op in enumerate(schedule.ops):
        op_id = index + 1
        if op_id in replayed or op_id in remaining:
            continue
        if op.t_complete > t_ack:
            continue
        if any(item in tainted for item in op.inputs):
            tainted.update(op.outputs)

    final_items = set(schedule.final_items())
    final_wrong = sorted(tainted & final_items)

    tainted_before: Set[Tuple[int, int]] = set()
    if error_class == "UE":
        tainted_before.update(bad_items)
    for index, op in enumerate(schedule.ops):
        if op.t_complete > t_ack:
            continue
        if any(item in tainted_before for item in op.inputs):
            tainted_before.update(op.outputs)

    source_repaired = len(repaired_items) == len(set(bad_items))
    return {
        "repaired_items": repaired_items,
        "source_repaired": source_repaired,
        "final_wrong": final_wrong,
        "tainted_before": tainted_before,
    }


def _row_base(case: CaseSpec) -> dict:
    return {
        "case_id": "",
        "kind": case.kind,
        "source": "L2",
        "workload": case.workload,
        "n_tiles": case.n_tiles,
        "fan_in": 0,
        "granularity": case.granularity,
        "error_class": case.error_class,
        "fault_operand": case.fault_operand,
        "fault_tile": case.fault_tile,
        "seed": case.seed,
        "policy": case.policy,
        "transport_delay": case.transport_delay,
        "engine_delta": case.engine_delta if case.engine_delta is not None else "",
        "window_mode": case.window_mode,
        "status": "",
        "affected_ops": "",
        "job_tiles": "",
        "replay_tiles": "",
        "replay_runs": "",
        "coalesced_extra_tiles": "",
        "restore_items": "",
        "restore_bytes": "",
        "quiesce_cycles": "",
        "restore_cycles": "",
        "replay_cycles": "",
        "resume_cycles": "",
        "publish_cycles": "",
        "recovery_cycles": "",
        "recovery_cycles_excl_resume": "",
        "fault_overhead_cycles": "",
        "end_to_end_cycles": "",
        "detect_cycle": "",
        "report_cycle": "",
        "ack_cycle": "",
        "correct": "",
        "source_repaired": "",
        "escaped_bad": "",
        "poisoned": "",
        "early_consume": int(case.early_consume),
        "same_cycle_release": int(case.same_cycle_release),
        "status_scenario": case.status_scenario,
        "distance_class": case.silent_distance,
        "silent_tile": case.silent_tile,
        "silent_repaired": "",
        "release": case.release,
        "delta_over_job": "",
        "baseline_end_to_end_cycles": case.baseline_end_to_end if case.baseline_end_to_end is not None else "",
        "overhead_fraction": "",
        "ledger_entries": "",
        "ledger_bytes_est": "",
        "peak_held_items": "",
        "r_read_count": "",
        "reader_partial_at_ack": "",
        "note": case.note,
    }


def _round(value, digits: int = 6):
    if value is None or value == "":
        return value
    return round(float(value), digits)


def _reader_partial_at_ack(schedule: Schedule, bad_items: Sequence[Tuple[int, int]],
                           t_ack: int) -> int:
    for item in bad_items:
        for reader in schedule.readers(item):
            op = schedule.op(reader)
            if op.t_issue <= t_ack < op.t_complete:
                return 1
    return 0


def _simulate_early_release(schedule: Schedule, ledger: Ledger, gate: Gate,
                            report_cycle: int, t_ack: int, delta: int,
                            same_cycle: bool) -> None:
    """Cycle-accurate report -> freeze -> release -> consume order (Fix-1).

    The host consumes only final outputs.  ``release_due`` uses strict ``>``;
    the report is processed before any release in the same cycle.
    """
    from .engine import build_release_schedule

    schedule_map = build_release_schedule(ledger, "DEP_RELEASE", delta)
    producers = schedule.producers()
    final_refs = [DataRef(item[0], item[1], producers[item] + 1)
                  for item in schedule.final_items()]
    for ref in final_refs:
        gate.hold(ref)
    if same_cycle:
        for ref in final_refs:
            schedule_map[(ref.operand, ref.tile, ref.version)] = report_cycle
    gate.unfreeze()
    for cycle in range(0, t_ack + 1):
        if cycle == report_cycle:
            gate.freeze()
        if gate.frozen:
            break
        released = gate.release_due(cycle, schedule_map)
        for ref in released:
            if ledger.is_final_item(1, ref.operand, ref.tile):
                gate.consume(ref, cycle)


def _run_fault_case(case: CaseSpec, costs: CostModel,
                    bad_items: Sequence[Tuple[int, int]],
                    report_item: Tuple[int, int],
                    silent_item: Optional[Tuple[int, int]] = None) -> dict:
    schedule = build_schedule(case.workload, case.n_tiles, costs.c_fix, costs.c_tile,
                              kernel_costs=costs.kernel_costs_for(case.workload))
    timing = detect_and_ack(schedule, report_item, case.transport_delay, costs)
    t_ack = timing["ack_cycle"]
    ledger = build_ledger(schedule, t_ack, max_log_entries=case.max_log_entries)
    checkpoint = CheckpointView(
        absent={case.checkpoint_missing} if case.checkpoint_missing else ())
    gate = Gate()

    codec = Scheme8Codec(Geometry())
    report = build_report(schedule.layout, report_item, case.granularity,
                          timing["report_cycle"], case.error_class, codec)
    engine_delta = case.engine_delta
    if engine_delta is None:
        engine_delta = case.transport_delay + 64 + 1

    if case.early_consume or case.same_cycle_release:
        _simulate_early_release(schedule, ledger, gate, report.report_cycle,
                                t_ack, int(engine_delta), case.same_cycle_release)

    kernel_costs_fix = tuple(costs.c_fix for _ in range(kernel_count(case.workload)))
    kernel_costs_tile = tuple(costs.c_tile for _ in range(kernel_count(case.workload)))
    b0 = case.policy == "B0"
    if b0:
        decision = plan_whole_job(ledger)
        fallback_full = False
    elif case.policy == "Oracle":
        decision = oracle_decision(schedule, t_ack, bad_items,
                                   case.error_class, checkpoint)
        fallback_full = False
    else:
        policy_name, radius = engine_policy(case.policy)
        config = EngineConfig(policy=policy_name, radius_rows=radius,
                              delta_cycles=int(engine_delta),
                              max_log_entries=case.max_log_entries,
                              window_mode=case.window_mode,
                              c_fix_by_kernel=kernel_costs_fix,
                              c_tile_by_kernel=kernel_costs_tile)
        decision = analyze(ledger, report, t_ack, config, gate=gate,
                           checkpoint=checkpoint,
                           report_overflow=case.report_overflow)
        fallback_full = (decision.status == STATUS_FALLBACK
                         and "LEDGER_TRUNCATED" in decision.fallback_reasons)

    if fallback_full:
        completed = [index + 1 for index, op in enumerate(schedule.ops)
                     if op.t_complete <= t_ack]
        initial = schedule.initial_inputs()
        decision.affected = completed
        decision.restore_list = [DataRef(item[0], item[1], 0) for item in initial
                                 if checkpoint.has(item[0], item[1])]

    plan = plan_execution(decision, ledger,
                          None if b0 else EngineConfig(
                              policy=engine_policy(case.policy)[0]
                              if case.policy != "Oracle" else POLICY_B1,
                              delta_cycles=int(engine_delta),
                              window_mode=case.window_mode,
                              c_fix_by_kernel=kernel_costs_fix,
                              c_tile_by_kernel=kernel_costs_tile),
                          gate)
    if b0:
        cost = b0_cost(schedule, t_ack, costs, len(schedule.initial_inputs()))
    else:
        cost = recovery_cost(decision, plan, schedule, costs, t_ack,
                             fallback_full=fallback_full)

    taint = _taint_analysis(schedule, bad_items, case.error_class, t_ack,
                            decision, plan, fallback_full, checkpoint, b0)
    consumed_items = {(ref.operand, ref.tile) for ref in decision.escaped_outputs}
    consumed_items |= {(key[0], key[1]) for key in gate.consumed_refs()}
    escaped_bad = any(item in taint["tainted_before"] for item in consumed_items)

    silent_repaired = ""
    if silent_item is not None:
        silent_repaired = int(silent_item in taint["repaired_items"])

    baseline_end_to_end = (case.baseline_end_to_end
                           if case.baseline_end_to_end is not None
                           else float(schedule.job_end))
    fault_overhead = cost["end_to_end_cycles"] - baseline_end_to_end
    row = _row_base(case)
    row.update({
        "case_id": (f"{case.kind}_{case.workload}_n{case.n_tiles}_{case.granularity}"
                    f"_{case.error_class}_{case.fault_operand}_{case.fault_tile}"
                    f"_{case.policy}_D{case.transport_delay}_s{case.seed}"
                    f"_wm{case.window_mode}"
                    + (f"_{case.silent_distance}" if case.silent_distance else "")
                    + (f"_{case.status_scenario}" if case.status_scenario else "")),
        "engine_delta": int(engine_delta),
        "status": STATUS_BASELINE if b0 else decision.status,
        "affected_ops": len(decision.affected),
        "fan_in": schedule.fan_in,
        "job_tiles": schedule.kernel_count * schedule.n_tiles,
        "replay_tiles": int(cost["replay_tiles"]),
        "replay_runs": int(cost["replay_runs"]),
        "coalesced_extra_tiles": int(cost.get("coalesced_extra_tiles", 0)),
        "restore_items": int(cost["restore_items"]),
        "restore_bytes": int(cost["restore_bytes"]),
        "quiesce_cycles": _round(cost["quiesce_cycles"]),
        "restore_cycles": _round(cost["restore_cycles"]),
        "replay_cycles": _round(cost["replay_cycles"]),
        "resume_cycles": _round(cost["resume_cycles"]),
        "publish_cycles": _round(cost["publish_cycles"]),
        "recovery_cycles": _round(cost["recovery_cycles"]),
        "recovery_cycles_excl_resume": _round(cost["recovery_cycles_excl_resume"]),
        "fault_overhead_cycles": _round(fault_overhead),
        "end_to_end_cycles": _round(cost["end_to_end_cycles"]),
        "baseline_end_to_end_cycles": _round(baseline_end_to_end),
        "correct": int(bool(taint["source_repaired"]
                            and not taint["final_wrong"]
                            and decision.status != STATUS_NO_TRUSTED)),
        "source_repaired": int(bool(taint["source_repaired"])),
        "escaped_bad": int(bool(escaped_bad)),
        "poisoned": int(decision.status == STATUS_NO_TRUSTED),
        "silent_repaired": silent_repaired,
        "ledger_entries": len(ledger.ops),
        "ledger_bytes_est": ledger.bytes_estimate(),
        "r_read_count": len(decision.r_read),
        "reader_partial_at_ack": _reader_partial_at_ack(schedule, bad_items, t_ack),
        "detect_cycle": timing["detect_cycle"],
        "report_cycle": timing["report_cycle"],
        "ack_cycle": t_ack,
    })
    return row


def evaluate_case(case: CaseSpec, costs: CostModel) -> dict:
    if case.kind in ("main", "status", "late", "window"):
        item = (case.fault_operand, case.fault_tile)
        return _run_fault_case(case, costs, [item], item)
    if case.kind == "radius":
        primary = (case.fault_operand, case.fault_tile)
        silent = (case.fault_operand, case.silent_tile)
        return _run_fault_case(case, costs, [primary, silent], primary,
                               silent_item=silent)
    if case.kind == "release":
        schedule = build_schedule(case.workload, case.n_tiles,
                                  costs.c_fix, costs.c_tile,
                                  kernel_costs=costs.kernel_costs_for(case.workload))
        delta = iround(case.delta_ratio * schedule.job_end)
        measured = release_experiment(schedule, case.release, delta, costs)
        row = _row_base(case)
        row.update({
            "case_id": (f"release_{case.workload}_n{case.n_tiles}_{case.release}"
                        f"_r{case.delta_ratio}_s{case.seed}"),
            "workload": case.workload,
            "release": case.release,
            "job_tiles": schedule.kernel_count * schedule.n_tiles,
            "delta_over_job": _round(measured["delta_over_job"]),
            "baseline_end_to_end_cycles": _round(measured["baseline_end_to_end_cycles"]),
            "end_to_end_cycles": _round(measured["end_to_end_cycles"]),
            "overhead_fraction": _round(measured["overhead_fraction"]),
            "note": f"delta_cycles={measured['delta_cycles']};job_end={measured['job_end_cycles']}",
        })
        return row
    if case.kind == "ledger":
        schedule = build_schedule(case.workload, case.n_tiles,
                                  costs.c_fix, costs.c_tile,
                                  kernel_costs=costs.kernel_costs_for(case.workload))
        summary = ledger_summary(schedule, case.release,
                                 iround(case.delta_ratio * schedule.job_end), costs)
        row = _row_base(case)
        row.update({
            "case_id": (f"ledger_{case.workload}_n{case.n_tiles}_{case.release}"
                        f"_r{case.delta_ratio}"),
            "release": case.release,
            "job_tiles": schedule.kernel_count * schedule.n_tiles,
            "delta_over_job": _round(case.delta_ratio),
            "ledger_entries": summary["entries"],
            "ledger_bytes_est": summary["bytes_est"],
            "peak_held_items": summary["peak_held_items"],
        })
        return row
    raise ValueError(case.kind)


def expand_main_cases(seeds: Sequence[int] = tuple(range(30)),
                      workloads: Sequence[str] = ("W1", "W2", "W3_K4", "W3_K16"),
                      n_tile_values: Sequence[int] = (8, 32, 128),
                      granularities: Sequence[str] = ("bank", "page", "codeword"),
                      error_classes: Sequence[str] = ("CE", "UE"),
                      policies: Sequence[str] = ("B0", "B1", "B2", "Oracle"),
                      delays: Sequence[int] = (16, 256, 4096),
                      window_mode: str = WINDOW_AUTO) -> List[CaseSpec]:
    cases: List[CaseSpec] = []
    for workload in workloads:
        for n_tiles in n_tile_values:
            for granularity in granularities:
                for error_class in error_classes:
                    for operand in fault_operands(workload):
                        for delay in delays:
                            for policy in policies:
                                for seed in seeds:
                                    tile = deterministic_tile(workload, operand, seed,
                                                              n_tiles)
                                    cases.append(CaseSpec(
                                        kind="main", workload=workload,
                                        n_tiles=n_tiles, seed=seed,
                                        granularity=granularity,
                                        error_class=error_class,
                                        fault_operand=operand, fault_tile=tile,
                                        policy=policy, transport_delay=delay,
                                        window_mode=window_mode))
    return cases


def expand_window_control_cases(seeds: Sequence[int] = tuple(range(30)),
                                workloads: Sequence[str] = ("W1", "W2", "W3_K4",
                                                            "W3_K16"),
                                n_tiles: int = 32,
                                granularities: Sequence[str] = ("bank", "page"),
                                policies: Sequence[str] = ("B1", "B2", "Oracle"),
                                delays: Sequence[int] = (16, 256, 4096)
                                ) -> List[CaseSpec]:
    """DELTA-window control for bank/page (Fix-2)."""
    cases: List[CaseSpec] = []
    for workload in workloads:
        for granularity in granularities:
            for operand in fault_operands(workload):
                for delay in delays:
                    for policy in policies:
                        for seed in seeds:
                            tile = deterministic_tile(workload, operand, seed, n_tiles)
                            cases.append(CaseSpec(
                                kind="window", workload=workload, n_tiles=n_tiles,
                                seed=seed, granularity=granularity,
                                error_class="UE", fault_operand=operand,
                                fault_tile=tile, policy=policy,
                                transport_delay=delay, window_mode="DELTA"))
    return cases


def expand_late_cases(seeds: Sequence[int] = tuple(range(30)),
                      workloads: Sequence[str] = ("W2", "W4_F1", "W4_F4", "W4_F16"),
                      n_tiles: int = 32,
                      ratios: Sequence[float] = (0.1, 0.5, 1.0, 2.0, 3.0),
                      policies: Sequence[str] = ("B0", "B1", "Oracle"),
                      window_modes: Sequence[str] = ("AUTO", "DELTA")) -> List[CaseSpec]:
    """Late-report sweep (Fix-7(b))."""
    cases: List[CaseSpec] = []
    for workload in workloads:
        # v2.1 W2/W4 ADD fit: c_fix=885, c_tile=408 (Fix-8b).
        t_k1 = 885.0 + n_tiles * 408.0
        for operand in (OPERAND_A, OPERAND_T):
            for ratio in ratios:
                delay = iround(ratio * t_k1)
                for policy in policies:
                    for mode in window_modes:
                        for seed in seeds:
                            tile = deterministic_tile(workload, operand, seed, n_tiles)
                            cases.append(CaseSpec(
                                kind="late", workload=workload, n_tiles=n_tiles,
                                seed=seed, granularity="page", error_class="UE",
                                fault_operand=operand, fault_tile=tile,
                                policy=policy, transport_delay=delay,
                                engine_delta=delay, window_mode=mode,
                                status_scenario=f"LATE_{ratio}"))
    return cases


def expand_radius_cases(seeds: Sequence[int] = tuple(range(30)),
                        distances: Sequence[str] = ("same_row", "plus1", "minus1",
                                                     "plus2", "minus2", "far"),
                        policies: Sequence[str] = ("B2", "B2R-0", "B2R-1", "B2R-2", "B0"),
                        n_tiles: int = 32,
                        delay: int = 256) -> List[CaseSpec]:
    cases: List[CaseSpec] = []
    for distance in distances:
        for policy in policies:
            for seed in seeds:
                primary = deterministic_tile("W1", 0, seed, n_tiles)
                silent = choose_silent_tile(primary, distance, n_tiles)
                cases.append(CaseSpec(
                    kind="radius", workload="W1", n_tiles=n_tiles, seed=seed,
                    granularity="page", error_class="CE",
                    fault_operand=0, fault_tile=primary, policy=policy,
                    transport_delay=delay, silent_distance=distance,
                    silent_tile=silent))
    return cases


def expand_release_cases(seeds: Sequence[int] = tuple(range(30)),
                         workloads: Sequence[str] = ("W1", "W2"),
                         n_tile_values: Sequence[int] = (8, 32),
                         ratios: Sequence[float] = (0.0, 0.05, 0.1, 0.25, 0.5, 1.0),
                         releases: Sequence[str] = ("GLOBAL_HOLD", "DEP_RELEASE")
                         ) -> List[CaseSpec]:
    cases: List[CaseSpec] = []
    for workload in workloads:
        for n_tiles in n_tile_values:
            for ratio in ratios:
                for release in releases:
                    for seed in seeds:
                        cases.append(CaseSpec(
                            kind="release", workload=workload, n_tiles=n_tiles,
                            seed=seed, policy="NONE", release=release,
                            delta_ratio=ratio))
    return cases


def expand_status_cases(seeds: Sequence[int] = tuple(range(30)),
                        scenarios: Sequence[str] = ("NORMAL_UE", "CE",
                                                    "DELTA_SHORT_PAGE_0.5",
                                                    "DELTA_SHORT_BANK_0.5",
                                                    "DELTA_FULL_1.0",
                                                    "SAME_CYCLE_RELEASE",
                                                    "CHECKPOINT_MISSING",
                                                    "LEDGER_TRUNCATED",
                                                    "REPORT_OVERFLOW")
                        ) -> List[CaseSpec]:
    cases: List[CaseSpec] = []
    for seed in seeds:
        tile = deterministic_tile("W1", 0, seed, 32)
        common = dict(kind="status", workload="W1", n_tiles=32, seed=seed,
                      granularity="page", fault_operand=0, fault_tile=tile,
                      transport_delay=4096)
        for scenario in scenarios:
            if scenario == "NORMAL_UE":
                cases.append(CaseSpec(error_class="UE", policy="B1",
                                      status_scenario="NORMAL_UE", **common))
            elif scenario == "CE":
                cases.append(CaseSpec(error_class="CE", policy="B2",
                                      status_scenario="CE", **common))
            elif scenario in ("DELTA_SHORT_0.5", "DELTA_SHORT_PAGE_0.5"):
                cases.append(CaseSpec(error_class="UE", policy="B1",
                                      status_scenario="DELTA_SHORT_PAGE_0.5",
                                      engine_delta=iround(0.5 * 4096), **common))
            elif scenario == "DELTA_SHORT_BANK_0.5":
                bank = dict(common)
                bank["granularity"] = "bank"
                cases.append(CaseSpec(error_class="UE", policy="B1",
                                      status_scenario="DELTA_SHORT_BANK_0.5",
                                      engine_delta=iround(0.5 * 4096), **bank))
            elif scenario == "DELTA_FULL_1.0":
                cases.append(CaseSpec(error_class="UE", policy="B1",
                                      status_scenario="DELTA_FULL_1.0",
                                      engine_delta=4096, early_consume=True,
                                      **common))
            elif scenario == "SAME_CYCLE_RELEASE":
                cases.append(CaseSpec(error_class="UE", policy="B1",
                                      status_scenario="SAME_CYCLE_RELEASE",
                                      engine_delta=4096, early_consume=True,
                                      same_cycle_release=True, **common))
            elif scenario == "CHECKPOINT_MISSING":
                cases.append(CaseSpec(error_class="UE", policy="B1",
                                      status_scenario="CHECKPOINT_MISSING",
                                      checkpoint_missing=(0, tile), **common))
            elif scenario == "LEDGER_TRUNCATED":
                cases.append(CaseSpec(error_class="UE", policy="B1",
                                      status_scenario="LEDGER_TRUNCATED",
                                      max_log_entries=8, **common))
            elif scenario == "REPORT_OVERFLOW":
                cases.append(CaseSpec(error_class="UE", policy="B1",
                                      status_scenario="REPORT_OVERFLOW",
                                      report_overflow=True, **common))
            else:
                raise ValueError(scenario)
    return cases


def expand_ledger_cases(workloads: Sequence[str] = ("W1", "W2", "W3_K4", "W3_K16"),
                        n_tile_values: Sequence[int] = (8, 32, 128)
                        ) -> List[CaseSpec]:
    cases: List[CaseSpec] = []
    for workload in workloads:
        for n_tiles in n_tile_values:
            for release, ratio in (("GLOBAL_HOLD", 0.0),
                                   ("DEP_RELEASE", 0.0),
                                   ("DEP_RELEASE", 0.25)):
                cases.append(CaseSpec(kind="ledger", workload=workload,
                                      n_tiles=n_tiles, seed=0,
                                      policy="NONE", release=release,
                                      delta_ratio=ratio))
    return cases

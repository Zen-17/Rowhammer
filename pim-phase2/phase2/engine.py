"""Phase 2 dependency-tracking and selective-recovery engine (model side).

The engine consumes only a public report, a ledger, an engine configuration,
checkpoint availability and output-gate state.  It never reads fault schedules,
device-internal state or output arrays.  The C++ counterpart in
``simulator/src/Phase2Engine.cpp`` implements the same deterministic algorithm.

v2.1 adds per-item time-window lower bounds (DELTA/BIRTH/AUTO), read-event
filtering (``R_read``), replay-interval coalescing, and explicit decision fields
for the differential test.
"""
from __future__ import annotations

import json
from collections import deque
from dataclasses import dataclass, field
from typing import Dict, Iterable, List, Optional, Sequence, Set, Tuple

from .layout import (
    BurstKey,
    Geometry,
    Layout,
    PublicReport,
    ReportFootprint,
    Scheme8Codec,
    footprint_bursts,
    item_overlaps,
    item_touches_radius,
    overlaps,
    report_footprint,
)

UNSTARTED = (1 << 64) - 1

STATUS_NO_TRUSTED = "OUTSIDE_WINDOW_NO_TRUSTED_INPUT"
STATUS_FALLBACK = "WHOLE_JOB_FALLBACK"
STATUS_ESCAPE = "PARTIAL_WITH_ESCAPE"
STATUS_SELECTIVE = "SELECTIVE_RECOVERED"
STATUS_SOURCE_ONLY = "SOURCE_REPAIRED_ONLY"
STATUS_BASELINE = "BASELINE_B0"

POLICY_B1 = "B1_SELECTIVE"
POLICY_B2 = "B2_CLASS_AWARE"
POLICY_B2R = "B2R_RADIUS"
POLICIES = (POLICY_B1, POLICY_B2, POLICY_B2R)

WINDOW_DELTA = "DELTA"
WINDOW_BIRTH = "BIRTH"
WINDOW_AUTO = "AUTO"
WINDOW_MODES = (WINDOW_DELTA, WINDOW_BIRTH, WINDOW_AUTO)


@dataclass(frozen=True, order=True)
class DataRef:
    operand: int
    tile: int
    version: int

    def item(self) -> Tuple[int, int]:
        return (self.operand, self.tile)

    def as_dict(self) -> dict:
        return {"operand": self.operand, "tile": self.tile, "version": self.version}

    def csv_text(self) -> str:
        return f"{self.operand}:{self.tile}:{self.version}"


@dataclass(frozen=True)
class HostRead:
    """A host read recorded in the ledger (Fix-3)."""

    operand: int
    tile: int
    version: int
    t_issue: int
    t_complete: int

    def key(self) -> Tuple[int, int, int]:
        return (self.operand, self.tile, self.version)


@dataclass
class OpEntry:
    op_id: int
    job_id: int
    kernel_idx: int
    tile: int
    inputs: List[DataRef]
    outputs: List[DataRef]
    t_issue: int = UNSTARTED
    t_complete: int = UNSTARTED
    completed: bool = False
    dep_complete: bool = True
    is_replay: bool = False

    def is_active_in(self, window_start: int, window_end: int) -> bool:
        if not self.completed:
            return False
        return self.t_issue <= window_end and self.t_complete >= window_start


@dataclass(frozen=True)
class EngineConfig:
    policy: str = POLICY_B1
    radius_rows: int = 0
    delta_cycles: int = 0
    max_log_entries: int = 100000
    window_mode: str = WINDOW_AUTO
    c_fix_by_kernel: Tuple[float, ...] = ()
    c_tile_by_kernel: Tuple[float, ...] = ()

    def validate(self) -> None:
        if self.policy not in POLICIES:
            raise ValueError(f"unknown policy: {self.policy}")
        if self.policy == POLICY_B2R and self.radius_rows < 0:
            raise ValueError("radius_rows must be non-negative")
        if self.window_mode not in WINDOW_MODES:
            raise ValueError(f"unknown window mode: {self.window_mode}")

    def kernel_costs(self, kernel_idx: int) -> Optional[Tuple[float, float]]:
        if kernel_idx >= len(self.c_fix_by_kernel) or kernel_idx >= len(self.c_tile_by_kernel):
            return None
        return (float(self.c_fix_by_kernel[kernel_idx]),
                float(self.c_tile_by_kernel[kernel_idx]))


@dataclass
class Decision:
    status: str
    window_start: int = 0
    window_end: int = 0
    suspect_items: List[DataRef] = field(default_factory=list)
    radius_suspect_items: List[DataRef] = field(default_factory=list)
    r_read: List[DataRef] = field(default_factory=list)
    seeds: List[int] = field(default_factory=list)
    regenerate: List[int] = field(default_factory=list)
    affected: List[int] = field(default_factory=list)
    restore_list: List[DataRef] = field(default_factory=list)
    escaped_outputs: List[DataRef] = field(default_factory=list)
    remaining: List[int] = field(default_factory=list)
    fallback_reasons: List[str] = field(default_factory=list)
    item_windows: List[Tuple[DataRef, int]] = field(default_factory=list)
    replay_runs: List[dict] = field(default_factory=list)
    coalesced_extra_tiles: int = 0

    def as_dict(self) -> dict:
        return {
            "affected": [int(v) for v in self.affected],
            "coalesced_extra_tiles": int(self.coalesced_extra_tiles),
            "escaped_outputs": [d.as_dict() for d in self.escaped_outputs],
            "fallback_reasons": list(self.fallback_reasons),
            "item_windows": [
                {"operand": ref.operand, "tile": ref.tile, "version": ref.version,
                 "window_start": int(window)}
                for ref, window in self.item_windows
            ],
            "r_read": [d.as_dict() for d in self.r_read],
            "radius_suspect_items": [d.as_dict() for d in self.radius_suspect_items],
            "regenerate": [int(v) for v in self.regenerate],
            "remaining": [int(v) for v in self.remaining],
            "replay_runs": [dict(run) for run in self.replay_runs],
            "restore_list": [d.as_dict() for d in self.restore_list],
            "seeds": [int(v) for v in self.seeds],
            "status": self.status,
            "suspect_items": [d.as_dict() for d in self.suspect_items],
            "window_end": int(self.window_end),
            "window_start": int(self.window_start),
        }

    def to_json(self) -> str:
        return json.dumps(self.as_dict(), sort_keys=True, separators=(",", ":"))


class CheckpointView:
    """Trusted-copy availability.  ``available=None`` means every tile exists."""

    def __init__(self, available: Optional[Iterable[Tuple[int, int]]] = None,
                 absent: Iterable[Tuple[int, int]] = ()):
        self.available = None if available is None else set(available)
        self.absent = set(absent)

    def has(self, operand: int, tile: int) -> bool:
        if (operand, tile) in self.absent:
            return False
        if self.available is None:
            return True
        return (operand, tile) in self.available

    def setAbsent(self, operand: int, tile: int) -> None:
        self.absent.add((operand, tile))


class Ledger:
    """Dependency ledger with one entry per issued tile operation."""

    def __init__(self, layout: Layout, max_log_entries: int = 100000):
        self.layout = layout
        self.max_log_entries = max(1, int(max_log_entries))
        self.ops: List[OpEntry] = []
        self.by_id: Dict[int, OpEntry] = {}
        self.current: Dict[Tuple[int, int], int] = {}
        self.host_reads: List[HostRead] = []
        self.truncated = False
        self.total_logged_ops = 0
        self._next_id = 1
        self._readers: Dict[Tuple[int, int, int], List[int]] = {}
        self._producers: Dict[Tuple[int, int, int], int] = {}
        self._final_cache: Dict[int, Set[Tuple[int, int]]] = {}

    # ---- construction -------------------------------------------------
    @classmethod
    def from_entries(cls, layout: Layout, entries: Sequence["OpEntry"],
                     max_log_entries: Optional[int] = None,
                     truncated: bool = False,
                     host_reads: Sequence[HostRead] = ()) -> "Ledger":
        """Rebuild a ledger with exact stored versions (used by differential replay)."""
        ledger = cls(layout, max_log_entries or (len(entries) + 1))
        for source in entries:
            entry = OpEntry(op_id=source.op_id, job_id=source.job_id,
                            kernel_idx=source.kernel_idx, tile=source.tile,
                            inputs=list(source.inputs), outputs=list(source.outputs),
                            t_issue=source.t_issue, t_complete=source.t_complete,
                            completed=source.completed, dep_complete=source.dep_complete,
                            is_replay=source.is_replay)
            ledger.ops.append(entry)
            ledger.by_id[entry.op_id] = entry
            ledger.total_logged_ops += 1
            if entry.completed:
                for ref in entry.outputs:
                    ledger.current[ref.item()] = ref.version
                    ledger._producers[(ref.operand, ref.tile, ref.version)] = entry.op_id
                for ref in entry.inputs:
                    ledger._readers.setdefault(
                        (ref.operand, ref.tile, ref.version), []).append(entry.op_id)
            ledger._next_id = max(ledger._next_id, entry.op_id + 1)
        ledger.host_reads = list(host_reads)
        ledger.truncated = truncated
        ledger._final_cache.clear()
        return ledger

    def plan_op(self, job_id: int, kernel_idx: int, tile: int,
                input_items: Sequence[Tuple[int, int]],
                output_items: Sequence[Tuple[int, int]],
                dep_complete: bool = True) -> int:
        op_id = self._next_id
        self._next_id += 1
        inputs = [DataRef(operand, item_tile, self.current.get((operand, item_tile), 0))
                  for operand, item_tile in input_items]
        outputs = [DataRef(operand, item_tile, 0) for operand, item_tile in output_items]
        entry = OpEntry(op_id=op_id, job_id=int(job_id), kernel_idx=int(kernel_idx),
                        tile=int(tile), inputs=inputs, outputs=outputs,
                        dep_complete=dep_complete)
        self.ops.append(entry)
        self.by_id[op_id] = entry
        self._final_cache.clear()
        self._evict_if_needed()
        return op_id

    def record_issue(self, op_id: int, t_issue: int) -> None:
        entry = self.by_id[op_id]
        entry.t_issue = int(t_issue)
        entry.inputs = [DataRef(ref.operand, ref.tile, self.current.get(ref.item(), 0))
                        for ref in entry.inputs]
        self.total_logged_ops += 1

    def record_complete(self, op_id: int, t_complete: int) -> None:
        entry = self.by_id[op_id]
        if entry.completed:
            return
        entry.completed = True
        entry.t_complete = int(t_complete)
        for index, ref in enumerate(entry.outputs):
            version = op_id
            entry.outputs[index] = DataRef(ref.operand, ref.tile, version)
            self.current[ref.item()] = version
            self._producers[(ref.operand, ref.tile, version)] = op_id
        for ref in entry.inputs:
            self._readers.setdefault((ref.operand, ref.tile, ref.version), []).append(op_id)
        self._final_cache.clear()

    def add_completed(self, job_id: int, kernel_idx: int, tile: int,
                      input_items: Sequence[Tuple[int, int]],
                      output_items: Sequence[Tuple[int, int]],
                      t_issue: int, t_complete: int,
                      dep_complete: bool = True) -> int:
        op_id = self.plan_op(job_id, kernel_idx, tile, input_items, output_items,
                             dep_complete=dep_complete)
        self.record_issue(op_id, t_issue)
        self.record_complete(op_id, t_complete)
        return op_id

    def record_host_read(self, operand: int, tile: int, version: int,
                         t_issue: int, t_complete: int) -> None:
        self.host_reads.append(HostRead(int(operand), int(tile), int(version),
                                        int(t_issue), int(t_complete)))

    def _evict_if_needed(self) -> None:
        while len(self.ops) > self.max_log_entries:
            old = self.ops.pop(0)
            self.by_id.pop(old.op_id, None)
            self.truncated = True

    # ---- queries ------------------------------------------------------
    def op(self, op_id: int) -> OpEntry:
        return self.by_id[op_id]

    def jobs(self) -> List[int]:
        return sorted({op.job_id for op in self.ops})

    def completed_ops(self) -> List[OpEntry]:
        return [op for op in self.ops if op.completed]

    def completed_ops_in_job(self, job_id: int) -> List[OpEntry]:
        return [op for op in self.ops if op.completed and op.job_id == job_id]

    def remaining_ops(self) -> List[OpEntry]:
        return [op for op in self.ops if not op.completed]

    def live_items(self) -> Dict[Tuple[int, int], int]:
        items: Dict[Tuple[int, int], int] = {}
        for op in self.ops:
            for ref in op.inputs:
                items[ref.item()] = self.current.get(ref.item(), 0)
            if op.completed:
                for ref in op.outputs:
                    items[ref.item()] = self.current.get(ref.item(), ref.version)
        return items

    def producer_of(self, ref: DataRef) -> Optional[int]:
        return self._producers.get((ref.operand, ref.tile, ref.version))

    def readers_of(self, ref: DataRef) -> List[int]:
        return list(self._readers.get((ref.operand, ref.tile, ref.version), ()))

    def _final_items_for(self, job_id: int) -> Set[Tuple[int, int]]:
        cached = self._final_cache.get(job_id)
        if cached is not None:
            return cached
        produced = set()
        consumed = set()
        for op in self.ops:
            if op.job_id != job_id:
                continue
            for ref in op.inputs:
                consumed.add(ref.item())
            if op.completed:
                for out in op.outputs:
                    produced.add(out.item())
        result = produced - consumed
        self._final_cache[job_id] = result
        return result

    def is_final_item(self, job_id: int, operand: int, tile: int) -> bool:
        return (operand, tile) in self._final_items_for(job_id)

    def final_items(self, job_id: int) -> List[Tuple[int, int]]:
        return sorted(self._final_items_for(job_id))

    def job_initial_inputs(self, job_id: int) -> List[DataRef]:
        items: Dict[Tuple[int, int], DataRef] = {}
        for op in self.ops:
            if op.job_id != job_id:
                continue
            for ref in op.inputs:
                if self.current.get(ref.item(), 0) == 0:
                    items[ref.item()] = DataRef(ref.operand, ref.tile, 0)
        return sorted(items.values())

    def write_discipline_reasons(self) -> List[str]:
        reasons: Set[str] = set()
        produced: Dict[Tuple[int, int, int], int] = {}
        for op in self.ops:
            input_items = {ref.item() for ref in op.inputs}
            for out in op.outputs:
                if out.item() in input_items:
                    reasons.add("IN_PLACE_UNSUPPORTED")
                key = (op.job_id, out.operand, out.tile)
                produced[key] = produced.get(key, 0) + 1
                if produced[key] > 1:
                    reasons.add("IN_PLACE_UNSUPPORTED")
        return sorted(reasons)

    def bytes_estimate(self) -> int:
        total = 0
        for op in self.ops:
            total += 8 + 4 + 2 + 4 + 16 + 1 + 8 * (len(op.inputs) + len(op.outputs))
        return total

    @property
    def peak_held_items(self) -> int:
        return len(self.current)


# ---------------------------------------------------------------------------
# Analysis
# ---------------------------------------------------------------------------


def birth_cycle(ledger: Ledger, ref: DataRef) -> int:
    """Cycle at which ``ref`` becomes live (Fix-2)."""
    if ref.version == 0:
        return 0
    producer = ledger.producer_of(ref)
    if producer is None:
        return 0
    return int(ledger.op(producer).t_complete)


def item_window_start(ledger: Ledger, ref: DataRef, report: PublicReport,
                      config: EngineConfig) -> int:
    """Per-item lower bound ``ws(d)`` for the suspicious set."""
    birth = birth_cycle(ledger, ref)
    use_birth = (config.window_mode == WINDOW_BIRTH
                 or (config.window_mode == WINDOW_AUTO
                     and report.region_type in ("page", "codeword")))
    if use_birth:
        return birth
    lower = report.report_cycle - config.delta_cycles \
        if report.report_cycle > config.delta_cycles else 0
    return max(birth, lower)


def ref_was_read_in_window(ledger: Ledger, ref: DataRef,
                           window_start: int, t_ack: int) -> bool:
    """True if a logged op read or host read intersects [ws, t_ack]."""
    key = (ref.operand, ref.tile, ref.version)
    for op in ledger.ops:
        if op.t_issue == UNSTARTED or op.t_issue > t_ack:
            continue
        finish = op.t_complete if op.t_complete != UNSTARTED else t_ack + 1
        if finish < window_start:
            continue
        if any((inp.operand, inp.tile, inp.version) == key for inp in op.inputs):
            return True
    for read in ledger.host_reads:
        if read.key() != key:
            continue
        if read.t_issue <= t_ack and read.t_complete >= window_start:
            return True
    return False


def _op_reads_any(op: OpEntry, suspects: Set[Tuple[int, int, int]],
                  window_start: int, window_end: int) -> bool:
    if not op.is_active_in(window_start, window_end):
        return False
    return any((ref.operand, ref.tile, ref.version) in suspects for ref in op.inputs)


def _op_reads_any_per_item(op: OpEntry, windows: Dict[Tuple[int, int, int], int],
                           t_ack: int) -> bool:
    """Seed test with a different lower bound for each suspect item (Fix-2)."""
    if op.t_issue == UNSTARTED or op.t_issue > t_ack:
        return False
    finish = op.t_complete if op.t_complete != UNSTARTED else t_ack + 1
    for inp in op.inputs:
        window_start = windows.get((inp.operand, inp.tile, inp.version))
        if window_start is None:
            continue
        if finish >= window_start:
            return True
    return False


def _group_runs(entries: Sequence[OpEntry]) -> List[dict]:
    runs: List[dict] = []
    ordered = sorted(entries, key=lambda op: (op.job_id, op.kernel_idx, op.tile))
    for op in ordered:
        if (runs and runs[-1]["job_id"] == op.job_id
                and runs[-1]["kernel_idx"] == op.kernel_idx
                and runs[-1]["tile_begin"] + runs[-1]["tile_count"] == op.tile):
            runs[-1]["tile_count"] += 1
        else:
            runs.append({"job_id": op.job_id, "kernel_idx": op.kernel_idx,
                         "tile_begin": op.tile, "tile_count": 1})
    return runs


@dataclass
class ExecutionPlan:
    replay_runs: List[dict] = field(default_factory=list)
    remaining_runs: List[dict] = field(default_factory=list)
    replay_tiles: int = 0
    restore_items: int = 0
    restore_bytes: int = 0
    replayed_ops: List[int] = field(default_factory=list)
    poison_outputs: List[DataRef] = field(default_factory=list)
    coalesced_extra_tiles: int = 0


def _coalesce_runs(runs: List[dict], decision: Decision, ledger: Ledger,
                   config: EngineConfig, gate: Optional["Gate"]) -> Tuple[List[dict], int]:
    """Merge adjacent replay intervals when the fixed cost dominates (Fix-4)."""
    if not runs or not config.c_fix_by_kernel or not config.c_tile_by_kernel:
        return runs, 0
    if decision.fallback_reasons:
        return runs, 0
    gate = gate or Gate()
    untrusted = {(ref.operand, ref.tile, ref.version)
                 for ref in decision.r_read + decision.radius_suspect_items}
    ops_by_kernel_tile: Dict[Tuple[int, int], OpEntry] = {}
    for op in ledger.completed_ops():
        ops_by_kernel_tile[(op.kernel_idx, op.tile)] = op
    merged: List[dict] = []
    extra = 0
    for run in runs:
        if not merged:
            merged.append(dict(run))
            continue
        prev = merged[-1]
        if (prev["job_id"] != run["job_id"]
                or prev["kernel_idx"] != run["kernel_idx"]):
            merged.append(dict(run))
            continue
        gap = run["tile_begin"] - (prev["tile_begin"] + prev["tile_count"])
        costs = config.kernel_costs(int(run["kernel_idx"]))
        if gap <= 0 or costs is None:
            merged.append(dict(run))
            continue
        c_fix, c_tile = costs
        if gap * c_tile >= c_fix:
            merged.append(dict(run))
            continue
        trusted = True
        for tile in range(prev["tile_begin"] + prev["tile_count"], run["tile_begin"]):
            op = ops_by_kernel_tile.get((int(run["kernel_idx"]), tile))
            if op is None:
                trusted = False
                break
            if any((ref.operand, ref.tile, ref.version) in untrusted
                   for ref in op.inputs):
                trusted = False
                break
            if any(gate.state(out) != "HELD" for out in op.outputs):
                trusted = False
                break
        if not trusted:
            merged.append(dict(run))
            continue
        prev["tile_count"] += gap + run["tile_count"]
        extra += gap
    return merged, extra


def plan_execution(decision: Decision, ledger: Ledger,
                   config: Optional[EngineConfig] = None,
                   gate: Optional["Gate"] = None) -> ExecutionPlan:
    plan = ExecutionPlan()
    if decision.status == STATUS_NO_TRUSTED:
        plan.poison_outputs = _affected_outputs(decision, ledger, None)
        return plan
    restore = list(decision.restore_list)
    plan.restore_items = len(restore)
    plan.restore_bytes = 4096 * len(restore)
    affected = [ledger.op(op_id) for op_id in decision.affected if op_id in ledger.by_id]
    plan.replayed_ops = [op.op_id for op in affected]
    plan.replay_tiles = len(affected)
    runs = _group_runs(affected)
    if config is not None:
        runs, extra = _coalesce_runs(runs, decision, ledger, config, gate)
        plan.coalesced_extra_tiles = extra
        if extra:
            expanded: List[int] = []
            for run in runs:
                for tile in range(run["tile_begin"],
                                  run["tile_begin"] + run["tile_count"]):
                    op = next((candidate for candidate in ledger.completed_ops()
                               if candidate.job_id == run["job_id"]
                               and candidate.kernel_idx == run["kernel_idx"]
                               and candidate.tile == tile), None)
                    if op is not None:
                        expanded.append(op.op_id)
            plan.replayed_ops = sorted(set(plan.replayed_ops) | set(expanded))
            plan.replay_tiles = sum(run["tile_count"] for run in runs)
    plan.replay_runs = runs
    remaining = [ledger.op(op_id) for op_id in decision.remaining if op_id in ledger.by_id]
    plan.remaining_runs = _group_runs(remaining)
    return plan


def _affected_outputs(decision: Decision, ledger: Ledger,
                      gate: Optional["Gate"]) -> List[DataRef]:
    outputs: Dict[Tuple[int, int], DataRef] = {}
    jobs = set()
    for op_id in decision.affected:
        if op_id not in ledger.by_id:
            continue
        op = ledger.op(op_id)
        jobs.add(op.job_id)
        for out in op.outputs:
            if ledger.is_final_item(op.job_id, out.operand, out.tile):
                outputs[out.item()] = out
    for ref in decision.suspect_items + decision.radius_suspect_items:
        for job_id in ledger.jobs():
            if ledger.is_final_item(job_id, ref.operand, ref.tile):
                outputs[ref.item()] = ref
    return sorted(outputs.values())


class Gate:
    """Per-data-item output lifecycle: HELD -> PUBLISHED -> CONSUMED, plus POISONED."""

    def __init__(self):
        self.states: Dict[Tuple[int, int, int], str] = {}
        self.publish_cycles: Dict[Tuple[int, int, int], int] = {}
        self.frozen = False

    @staticmethod
    def _key(ref: DataRef) -> Tuple[int, int, int]:
        return (ref.operand, ref.tile, ref.version)

    def state(self, ref: DataRef) -> str:
        return self.states.get(self._key(ref), "HELD")

    def freeze(self) -> None:
        self.frozen = True

    def unfreeze(self) -> None:
        self.frozen = False

    def hold(self, ref: DataRef) -> None:
        key = self._key(ref)
        if self.states.get(key) not in ("CONSUMED", "POISONED"):
            self.states[key] = "HELD"

    def publish(self, ref: DataRef, cycle: int) -> None:
        key = self._key(ref)
        if self.states.get(key, "HELD") == "HELD":
            self.states[key] = "PUBLISHED"
            self.publish_cycles[key] = int(cycle)

    def consume(self, ref: DataRef, cycle: int) -> bool:
        key = self._key(ref)
        if self.states.get(key) != "PUBLISHED":
            return False
        self.states[key] = "CONSUMED"
        self.publish_cycles.setdefault(key, int(cycle))
        return True

    def mark_suspect(self, refs: Iterable[DataRef]) -> None:
        for ref in refs:
            key = self._key(ref)
            if self.states.get(key) == "PUBLISHED":
                self.states[key] = "HELD"

    def poison(self, refs: Iterable[DataRef]) -> None:
        for ref in refs:
            self.states[self._key(ref)] = "POISONED"

    def release_due(self, cycle: int,
                    schedule: Dict[Tuple[int, int, int], int]) -> List[DataRef]:
        """Release items strictly after their scheduled cycle (Fix-1)."""
        if self.frozen:
            return []
        released: List[DataRef] = []
        for key, release_cycle in sorted(schedule.items()):
            if cycle <= release_cycle:
                continue
            ref = DataRef(*key)
            if self.states.get(key, "HELD") == "HELD":
                self.publish(ref, cycle)
                released.append(ref)
        return released

    def consumed_refs(self) -> Set[Tuple[int, int, int]]:
        return {key for key, value in self.states.items() if value == "CONSUMED"}


def analyze(ledger: Ledger,
            report: PublicReport,
            t_ack: int,
            config: EngineConfig,
            gate: Optional[Gate] = None,
            checkpoint: Optional[CheckpointView] = None,
            report_overflow: bool = False,
            geometry: Optional[Geometry] = None) -> Decision:
    """Deterministic selective-recovery analysis, run after pause ACK."""
    config.validate()
    if checkpoint is None:
        checkpoint = CheckpointView()
    codec = Scheme8Codec(geometry or Geometry())
    footprint = report_footprint(report, codec)

    live = ledger.live_items()
    suspect: List[DataRef] = []
    radius_suspect: List[DataRef] = []
    for (operand, tile), version in sorted(live.items()):
        if item_overlaps(ledger.layout, operand, tile, footprint):
            suspect.append(DataRef(operand, tile, version))
        elif config.policy == POLICY_B2R and report.error_class == "CE":
            if item_touches_radius(ledger.layout, operand, tile, footprint,
                                   config.radius_rows):
                radius_suspect.append(DataRef(operand, tile, version))

    item_windows = [(ref, item_window_start(ledger, ref, report, config))
                    for ref in sorted(suspect + radius_suspect)]
    # Fix-3: a report is caused by a read.  Only items actually read in their
    # per-item window can be corrupt; otherwise fall back to the full spatial R.
    r_read = [ref for ref in suspect
              if ref_was_read_in_window(ledger, ref,
                                        item_window_start(ledger, ref, report, config),
                                        t_ack)]
    fallback_reasons: List[str] = []
    if suspect and not r_read:
        fallback_reasons.append("UNLOGGED_READER")
        effective_suspect = list(suspect)
    else:
        effective_suspect = list(r_read)

    suspect_set = {(ref.operand, ref.tile, ref.version) for ref in effective_suspect}
    radius_set = {(ref.operand, ref.tile, ref.version) for ref in radius_suspect}
    window_start = report.report_cycle - config.delta_cycles \
        if report.report_cycle > config.delta_cycles else 0
    window_end = int(t_ack)
    window_by_key = {
        (ref.operand, ref.tile, ref.version):
            item_window_start(ledger, ref, report, config)
        for ref in effective_suspect
    }

    seeds: Set[int] = set()
    if config.policy == POLICY_B1:
        seeds = {op.op_id for op in ledger.completed_ops()
                 if _op_reads_any_per_item(op, window_by_key, window_end)}
    elif config.policy == POLICY_B2:
        if report.error_class == "UE":
            seeds = {op.op_id for op in ledger.completed_ops()
                     if _op_reads_any_per_item(op, window_by_key, window_end)}
    elif config.policy == POLICY_B2R:
        if report.error_class == "UE":
            seeds = {op.op_id for op in ledger.completed_ops()
                     if _op_reads_any_per_item(op, window_by_key, window_end)}
        else:
            seeds = {op.op_id for op in ledger.completed_ops()
                     if _op_reads_any(op, radius_set, 0, window_end)}

    restore: Dict[Tuple[int, int], DataRef] = {}
    regenerate: Set[int] = set()
    missing_inputs: List[DataRef] = []
    producer_missing = False
    suspicious = {ref.item(): ref for ref in effective_suspect + radius_suspect}
    for item, ref in sorted(suspicious.items()):
        if ref.version == 0:
            if checkpoint.has(ref.operand, ref.tile):
                restore[item] = ref
            else:
                missing_inputs.append(ref)
        else:
            producer = ledger.producer_of(ref)
            if producer is None:
                producer_missing = True
            else:
                regenerate.add(producer)

    affected: Set[int] = set(seeds) | set(regenerate)
    queue = deque(sorted(affected))
    while queue:
        op_id = queue.popleft()
        if op_id not in ledger.by_id:
            continue
        op = ledger.op(op_id)
        for out in op.outputs:
            for reader in ledger.readers_of(out):
                if reader not in affected:
                    affected.add(reader)
                    queue.append(reader)
        for ref in op.inputs:
            if ref.version == 0:
                continue
            if (ref.operand, ref.tile, ref.version) not in suspect_set | radius_set:
                continue
            producer = ledger.producer_of(ref)
            if producer is not None and producer not in affected:
                affected.add(producer)
                queue.append(producer)

    if ledger.truncated:
        fallback_reasons.append("LEDGER_TRUNCATED")
    if report_overflow:
        fallback_reasons.append("REPORT_OVERFLOW")
    if ledger.write_discipline_reasons():
        fallback_reasons.append("IN_PLACE_UNSUPPORTED")
    if producer_missing:
        fallback_reasons.append("PRODUCER_MISSING")
    if any(ledger.by_id[op_id].dep_complete is False for op_id in affected
           if op_id in ledger.by_id):
        fallback_reasons.append("DEPENDENCY_INCOMPLETE")

    if fallback_reasons:
        involved_jobs = {ledger.by_id[op_id].job_id for op_id in affected
                         if op_id in ledger.by_id}
        if not involved_jobs:
            involved_jobs = set(ledger.jobs())
        affected = {op.op_id for job_id in involved_jobs
                    for op in ledger.completed_ops_in_job(job_id)}
        restore = {}
        for job_id in sorted(involved_jobs):
            for ref in ledger.job_initial_inputs(job_id):
                if checkpoint.has(ref.operand, ref.tile):
                    restore[ref.item()] = ref
                elif ref not in missing_inputs:
                    missing_inputs.append(ref)

    gate = gate or Gate()
    escaped: List[DataRef] = []
    if missing_inputs:
        status = STATUS_NO_TRUSTED
    elif fallback_reasons:
        status = STATUS_FALLBACK
    else:
        probe = Decision(status=STATUS_SELECTIVE, affected=sorted(affected),
                         suspect_items=suspect, radius_suspect_items=radius_suspect)
        outputs = _affected_outputs(probe, ledger, gate)
        escaped = sorted(ref for ref in outputs if gate.state(ref) == "CONSUMED")
        if escaped:
            status = STATUS_ESCAPE
        elif affected:
            status = STATUS_SELECTIVE
        else:
            status = STATUS_SOURCE_ONLY

    remaining = sorted(op.op_id for op in ledger.remaining_ops())
    decision = Decision(
        status=status,
        window_start=window_start,
        window_end=window_end,
        suspect_items=sorted(suspect),
        radius_suspect_items=sorted(radius_suspect),
        r_read=sorted(r_read),
        seeds=sorted(seeds),
        regenerate=sorted(regenerate),
        affected=sorted(affected),
        restore_list=sorted(restore.values()),
        escaped_outputs=escaped,
        remaining=remaining,
        fallback_reasons=fallback_reasons,
        item_windows=item_windows,
    )
    plan = plan_execution(decision, ledger, config, gate)
    decision.replay_runs = plan.replay_runs
    decision.coalesced_extra_tiles = plan.coalesced_extra_tiles
    return decision


def plan_whole_job(ledger: Ledger, job_id: Optional[int] = None,
                   reason: str = "B0_BASELINE",
                   checkpoint: Optional[CheckpointView] = None) -> Decision:
    if checkpoint is None:
        checkpoint = CheckpointView()
    jobs = [job_id] if job_id is not None else ledger.jobs()
    affected = sorted(op.op_id for j in jobs for op in ledger.completed_ops_in_job(j))
    restore = {}
    for j in jobs:
        for ref in ledger.job_initial_inputs(j):
            restore[ref.item()] = ref
    decision = Decision(
        status=STATUS_BASELINE,
        suspect_items=[],
        radius_suspect_items=[],
        r_read=[],
        seeds=[],
        regenerate=[],
        affected=affected,
        restore_list=sorted(restore.values()),
        escaped_outputs=[],
        remaining=sorted(op.op_id for op in ledger.remaining_ops()),
        fallback_reasons=[reason],
    )
    decision.replay_runs = _group_runs(
        [ledger.op(op_id) for op_id in affected if op_id in ledger.by_id])
    return decision


def ancestor_complete_time(ledger: Ledger, op_id: int, memo: Optional[Dict[int, int]] = None) -> int:
    """Latest t_complete among an op and all of its ancestors."""
    if memo is None:
        memo = {}
    if op_id in memo:
        return memo[op_id]
    op = ledger.op(op_id)
    latest = op.t_complete
    for ref in op.inputs:
        producer = ledger.producer_of(ref)
        if producer is not None and producer != op_id:
            latest = max(latest, ancestor_complete_time(ledger, producer, memo))
    memo[op_id] = latest
    return latest


def build_release_schedule(ledger: Ledger, release: str, delta_cycles: int,
                           hold_cycles: int = 0) -> Dict[Tuple[int, int, int], int]:
    """Return DataRef key -> release cycle.  Gate releases strictly after it."""
    schedule: Dict[Tuple[int, int, int], int] = {}
    if release == "GLOBAL_HOLD":
        job_end: Dict[int, int] = {}
        for op in ledger.completed_ops():
            job_end[op.job_id] = max(job_end.get(op.job_id, 0), op.t_complete)
        for op in ledger.completed_ops():
            for out in op.outputs:
                schedule[(out.operand, out.tile, out.version)] = \
                    job_end[op.job_id] + hold_cycles
    elif release == "DEP_RELEASE":
        memo: Dict[int, int] = {}
        for op in ledger.completed_ops():
            release_cycle = ancestor_complete_time(ledger, op.op_id, memo) + delta_cycles
            for out in op.outputs:
                schedule[(out.operand, out.tile, out.version)] = release_cycle
    else:
        raise ValueError(f"unknown release policy: {release}")
    return schedule

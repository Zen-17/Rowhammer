"""L1 model-side tests U1-U15 for the Phase 2 recovery engine."""
from __future__ import annotations

import json

from phase2.engine import (
    CheckpointView,
    DataRef,
    EngineConfig,
    Gate,
    Ledger,
    POLICY_B1,
    POLICY_B2,
    POLICY_B2R,
    STATUS_ESCAPE,
    STATUS_FALLBACK,
    STATUS_NO_TRUSTED,
    STATUS_SELECTIVE,
    STATUS_SOURCE_ONLY,
    analyze,
    plan_execution,
    plan_whole_job,
)
from phase2.layout import (
    Geometry,
    Layout,
    OPERAND_A,
    OPERAND_B,
    OPERAND_O,
    OPERAND_T,
    OPERAND_U,
    OPERAND_X,
    OPERAND_Y,
    PublicReport,
    WorkloadLayouts,
    page_region_id,
)

# U10-only operand ids.
OPERAND_C = 40
OPERAND_D = 41


def w1(n_tiles: int, base: int = 100, step: int = 10, duration: int = 5,
       job: int = 1) -> tuple[Ledger, Layout]:
    layout = WorkloadLayouts.default().w1
    ledger = Ledger(layout)
    for tile in range(n_tiles):
        ledger.add_completed(job, 0, tile,
                             [(OPERAND_A, tile), (OPERAND_X, tile)],
                             [(OPERAND_B, tile)],
                             base + tile * step, base + tile * step + duration)
    return ledger, layout


def w2(n_tiles: int, mul0_dep: bool = True, job: int = 1) -> tuple[Ledger, Layout]:
    layout = WorkloadLayouts.default().w2
    ledger = Ledger(layout)
    for tile in range(n_tiles):
        ledger.add_completed(job, 0, tile,
                             [(OPERAND_A, tile), (OPERAND_X, tile)],
                             [(OPERAND_T, tile)],
                             100 + tile * 10, 105 + tile * 10)
    for tile in range(n_tiles):
        ledger.add_completed(job, 1, tile,
                             [(OPERAND_T, tile), (OPERAND_Y, tile)],
                             [(OPERAND_U, tile)],
                             200 + tile * 10, 205 + tile * 10,
                             dep_complete=mul0_dep or tile != 0)
    for tile in range(n_tiles):
        ledger.add_completed(job, 2, tile,
                             [(OPERAND_U, tile)],
                             [(OPERAND_O, tile)],
                             300 + tile * 10, 305 + tile * 10)
    return ledger, layout


def page_report(layout: Layout, operand: int, tile: int, cycle: int,
                error_class: str = "UE", seq: int = 1) -> PublicReport:
    return PublicReport(seq, cycle, error_class, "page",
                        page_region_id(layout, operand, tile))


def bank_report(bank: int, cycle: int, error_class: str = "UE", seq: int = 1) -> PublicReport:
    return PublicReport(seq, cycle, error_class, "bank", bank)


def test_u1_single_tile_selective() -> None:
    ledger, layout = w1(4)
    decision = analyze(ledger, page_report(layout, OPERAND_A, 1, 150), 200,
                       EngineConfig(policy=POLICY_B1, delta_cycles=100))
    assert decision.status == STATUS_SELECTIVE
    assert decision.affected == [2]
    assert decision.seeds == [2]
    assert decision.restore_list == [DataRef(OPERAND_A, 1, 0)]
    assert decision.regenerate == []
    plan = plan_execution(decision, ledger)
    assert plan.replay_tiles == 1
    assert plan.restore_bytes == 4096
    assert plan.replay_runs == [{"job_id": 1, "kernel_idx": 0, "tile_begin": 1,
                                 "tile_count": 1}]


def test_u2_forward_closure_w2() -> None:
    ledger, layout = w2(4)
    decision = analyze(ledger, page_report(layout, OPERAND_A, 0, 400), 400,
                       EngineConfig(policy=POLICY_B1, delta_cycles=1000))
    assert decision.affected == [1, 5, 9]
    assert decision.affected == sorted(decision.affected)
    assert decision.restore_list == [DataRef(OPERAND_A, 0, 0)]


def test_u3_regenerate_producer_of_intermediate() -> None:
    ledger, layout = w2(4)
    decision = analyze(ledger, page_report(layout, OPERAND_T, 2, 400), 400,
                       EngineConfig(policy=POLICY_B1, delta_cycles=1000))
    assert decision.regenerate == [3]
    assert set(decision.affected) >= {3, 7}
    assert decision.restore_list == []
    assert decision.status != STATUS_NO_TRUSTED


def test_u4_ce_class_aware_vs_b1() -> None:
    ledger, layout = w1(4)
    report = page_report(layout, OPERAND_A, 1, 150, error_class="CE")
    b2 = analyze(ledger, report, 200, EngineConfig(policy=POLICY_B2, delta_cycles=100))
    assert b2.status == STATUS_SOURCE_ONLY
    assert b2.affected == []
    assert b2.restore_list == [DataRef(OPERAND_A, 1, 0)]
    b1 = analyze(ledger, report, 200, EngineConfig(policy=POLICY_B1, delta_cycles=100))
    assert b1.affected == [2]


def test_u5_incomplete_dependency_falls_back() -> None:
    ledger, layout = w2(4, mul0_dep=False)
    decision = analyze(ledger, page_report(layout, OPERAND_A, 0, 400), 400,
                       EngineConfig(policy=POLICY_B1, delta_cycles=1000))
    assert decision.status == STATUS_FALLBACK
    assert "DEPENDENCY_INCOMPLETE" in decision.fallback_reasons
    assert decision.affected == list(range(1, 13))


def test_u6_truncated_ledger_falls_back() -> None:
    ledger, layout = w1(4)
    ledger.truncated = True
    decision = analyze(ledger, page_report(layout, OPERAND_A, 1, 150), 200,
                       EngineConfig(policy=POLICY_B1, delta_cycles=100))
    assert decision.status == STATUS_FALLBACK
    assert "LEDGER_TRUNCATED" in decision.fallback_reasons
    assert decision.affected == [1, 2, 3, 4]


def test_u7_report_overflow_falls_back() -> None:
    ledger, layout = w1(4)
    decision = analyze(ledger, page_report(layout, OPERAND_A, 1, 150), 200,
                       EngineConfig(policy=POLICY_B1, delta_cycles=100),
                       report_overflow=True)
    assert decision.status == STATUS_FALLBACK
    assert "REPORT_OVERFLOW" in decision.fallback_reasons


def test_u8_consumed_output_is_escape() -> None:
    ledger, layout = w1(4)
    gate = Gate()
    output = DataRef(OPERAND_B, 1, 2)
    gate.publish(output, 150)
    assert gate.consume(output, 160)
    decision = analyze(ledger, page_report(layout, OPERAND_A, 1, 150), 200,
                       EngineConfig(policy=POLICY_B1, delta_cycles=100), gate=gate)
    assert decision.status == STATUS_ESCAPE
    assert decision.escaped_outputs == [output]
    assert decision.affected == [2]


def test_u9_missing_checkpoint_blocks_execution() -> None:
    ledger, layout = w1(4)
    checkpoint = CheckpointView(absent={(OPERAND_A, 1)})
    decision = analyze(ledger, page_report(layout, OPERAND_A, 1, 150), 200,
                       EngineConfig(policy=POLICY_B1, delta_cycles=100),
                       checkpoint=checkpoint)
    assert decision.status == STATUS_NO_TRUSTED
    plan = plan_execution(decision, ledger)
    assert plan.replay_tiles == 0
    assert plan.restore_bytes == 0
    assert plan.poison_outputs == [DataRef(OPERAND_B, 1, 2)]


def test_u10_time_window_includes_report_to_ack_reads() -> None:
    layout = Layout("U10", ((OPERAND_A, 0), (OPERAND_C, 256), (OPERAND_D, 384)),
                    OPERAND_D)
    ledger = Ledger(layout)
    ledger.add_completed(1, 0, 1, [(OPERAND_A, 1)], [(OPERAND_C, 1)], 800, 850)
    ledger.add_completed(1, 1, 1, [(OPERAND_A, 1)], [(OPERAND_D, 1)], 1050, 1090)
    decision = analyze(ledger, page_report(layout, OPERAND_A, 1, 1000), 1100,
                       EngineConfig(policy=POLICY_B1, delta_cycles=100,
                                    window_mode="DELTA"))
    assert decision.window_start == 900
    assert decision.window_end == 1100
    assert decision.seeds == [2]
    assert decision.affected == [2]


def test_u11_bank_report_seeds_follow_time_window() -> None:
    layout = WorkloadLayouts.default().w1
    ledger = Ledger(layout)
    for tile in range(3):
        ledger.add_completed(1, 0, tile, [(OPERAND_A, tile), (OPERAND_X, tile)],
                             [(OPERAND_B, tile)], 100 + tile * 10, 102 + tile * 10)
    pending = ledger.plan_op(1, 0, 3, [(OPERAND_A, 3), (OPERAND_X, 3)],
                             [(OPERAND_B, 3)])
    ledger.record_issue(pending, 130)
    decision = analyze(ledger, bank_report(0, 125), 125,
                       EngineConfig(policy=POLICY_B1, delta_cycles=5))
    assert decision.seeds == [3]
    assert decision.remaining == [4]
    assert len(decision.suspect_items) >= 9


def test_u12_radius_ce_ignores_lower_time_bound() -> None:
    ledger, layout = w1(8)
    decision = analyze(ledger, page_report(layout, OPERAND_A, 1, 400, error_class="CE"),
                       400, EngineConfig(policy=POLICY_B2R, radius_rows=1,
                                         delta_cycles=1000))
    assert decision.suspect_items == [DataRef(OPERAND_A, 1, 0)]
    assert decision.radius_suspect_items == [
        DataRef(OPERAND_A, 0, 0), DataRef(OPERAND_A, 2, 0),
        DataRef(OPERAND_A, 3, 0), DataRef(OPERAND_A, 4, 0),
        DataRef(OPERAND_A, 5, 0), DataRef(OPERAND_A, 6, 0),
        DataRef(OPERAND_A, 7, 0),
    ]
    assert decision.seeds == [1, 3, 4, 5, 6, 7, 8]
    assert decision.window_start == 0


def test_u13_report_before_delta_has_zero_window_start() -> None:
    ledger, layout = w1(4)
    decision = analyze(ledger, page_report(layout, OPERAND_A, 1, 5), 20,
                       EngineConfig(policy=POLICY_B1, delta_cycles=50))
    assert decision.window_start == 0
    assert decision.window_end == 20


def test_u14_in_place_update_falls_back() -> None:
    layout = WorkloadLayouts.default().w1
    ledger = Ledger(layout)
    ledger.add_completed(1, 0, 0, [(OPERAND_A, 0)], [(OPERAND_A, 0)], 100, 105)
    decision = analyze(ledger, page_report(layout, OPERAND_A, 0, 150), 200,
                       EngineConfig(policy=POLICY_B1, delta_cycles=100))
    assert decision.status == STATUS_FALLBACK
    assert "IN_PLACE_UNSUPPORTED" in decision.fallback_reasons


def test_u15_repeat_is_byte_identical() -> None:
    ledger, layout = w1(4)
    report = page_report(layout, OPERAND_A, 1, 150)
    config = EngineConfig(policy=POLICY_B1, delta_cycles=100)
    first = analyze(ledger, report, 200, config).to_json()
    second = analyze(ledger, report, 200, config).to_json()
    assert first == second
    other_ledger, _ = w1(4)
    third = analyze(other_ledger, report, 200, config).to_json()
    assert first == third
    parsed = json.loads(first)
    assert parsed["status"] == STATUS_SELECTIVE
    assert parsed["affected"] == [2]


def test_whole_job_plan_is_deterministic() -> None:
    ledger, _layout = w2(4)
    first = plan_whole_job(ledger).to_json()
    second = plan_whole_job(ledger).to_json()
    assert first == second
    decision = plan_whole_job(ledger)
    assert decision.affected == list(range(1, 13))
    assert len(decision.restore_list) == 12
    assert decision.fallback_reasons == ["B0_BASELINE"]


# ---------------------------------------------------------------------------
# v2.1 tests U16-U24
# ---------------------------------------------------------------------------


def test_u16_release_at_same_cycle_stays_held() -> None:
    gate = Gate()
    ref = DataRef(OPERAND_B, 0, 1)
    gate.hold(ref)
    schedule = {(ref.operand, ref.tile, ref.version): 100}
    gate.release_due(100, schedule)
    assert gate.state(ref) == "HELD"
    gate.release_due(101, schedule)
    assert gate.state(ref) == "PUBLISHED"


def test_u17_release_before_scheduled_cycle_stays_held() -> None:
    gate = Gate()
    ref = DataRef(OPERAND_B, 0, 1)
    gate.hold(ref)
    schedule = {(ref.operand, ref.tile, ref.version): 100}
    gate.release_due(99, schedule)
    assert gate.state(ref) == "HELD"


def test_u18_report_before_ack_freezes_all_release() -> None:
    gate = Gate()
    refs = [DataRef(OPERAND_B, tile, tile + 1) for tile in range(3)]
    schedule = {(ref.operand, ref.tile, ref.version): 100 + ref.tile
                for ref in refs}
    for ref in refs:
        gate.hold(ref)
    gate.freeze()
    for cycle in range(0, 200):
        gate.release_due(cycle, schedule)
    assert all(gate.state(ref) == "HELD" for ref in refs)
    gate.unfreeze()
    gate.release_due(201, schedule)
    assert all(gate.state(ref) == "PUBLISHED" for ref in refs)


def test_u19_page_auto_uses_birth_not_delta() -> None:
    layout = Layout("U19", ((OPERAND_A, 0), (OPERAND_C, 256)), OPERAND_C)
    ledger = Ledger(layout)
    ledger.add_completed(1, 0, 1, [(OPERAND_A, 1)], [(OPERAND_C, 1)], 100, 150)
    report = page_report(layout, OPERAND_A, 1, 1000)
    auto = analyze(ledger, report, 1100,
                   EngineConfig(policy=POLICY_B1, delta_cycles=500,
                                window_mode="AUTO"))
    delta = analyze(ledger, report, 1100,
                    EngineConfig(policy=POLICY_B1, delta_cycles=500,
                                 window_mode="DELTA"))
    assert auto.seeds == [1]
    assert auto.affected == [1]
    assert 1 not in delta.seeds


def test_u20_bank_auto_keeps_delta_window() -> None:
    ledger, _layout = w1(3)
    # Re-time the W1 ops: tile 0 early, tile 1 inside the delta window.
    ledger.ops[0].t_issue, ledger.ops[0].t_complete = 100, 150
    ledger.ops[1].t_issue, ledger.ops[1].t_complete = 600, 650
    ledger.ops[2].t_issue, ledger.ops[2].t_complete = 300, 350
    decision = analyze(ledger, bank_report(0, 1000), 1100,
                       EngineConfig(policy=POLICY_B1, delta_cycles=500,
                                    window_mode="AUTO"))
    assert decision.seeds == [2]
    assert decision.seeds != [1, 2, 3]


def test_u21_bank_report_filters_to_read_items() -> None:
    layout = WorkloadLayouts.default().w1
    ledger = Ledger(layout)
    ledger.add_completed(1, 0, 0, [(OPERAND_A, 0)], [(OPERAND_B, 0)], 100, 105)
    ledger.add_completed(1, 0, 1, [(OPERAND_A, 1)], [(OPERAND_B, 1)], 200, 205)
    ledger.add_completed(1, 0, 2, [(OPERAND_A, 2)], [(OPERAND_B, 2)], 500, 505)
    decision = analyze(ledger, bank_report(0, 600), 600,
                       EngineConfig(policy=POLICY_B1, delta_cycles=200,
                                    window_mode="DELTA"))
    assert decision.r_read == [DataRef(OPERAND_A, 2, 0)]
    assert decision.seeds == [3]


def test_u22_empty_read_set_falls_back_with_reason() -> None:
    layout = WorkloadLayouts.default().w1
    ledger = Ledger(layout)
    ledger.add_completed(1, 0, 0, [(OPERAND_A, 0)], [(OPERAND_B, 0)], 100, 105)
    decision = analyze(ledger, bank_report(0, 1000), 1000,
                       EngineConfig(policy=POLICY_B1, delta_cycles=100,
                                    window_mode="DELTA"))
    assert decision.r_read == []
    assert "UNLOGGED_READER" in decision.fallback_reasons
    assert decision.status == STATUS_FALLBACK


def test_u23_adjacent_replay_intervals_are_coalesced() -> None:
    ledger, _layout = w1(4)
    decision = analyze(
        ledger, page_report(WorkloadLayouts.default().w1, OPERAND_A, 1, 150), 200,
        EngineConfig(policy=POLICY_B1, delta_cycles=100,
                     c_fix_by_kernel=(1000.0,), c_tile_by_kernel=(100.0,)))
    assert decision.affected == [2]
    # Manually add tile 3 as a second affected op.
    decision.affected = [2, 4]
    plan = plan_execution(decision, ledger, decision_config := EngineConfig(
        policy=POLICY_B1, c_fix_by_kernel=(1000.0,), c_tile_by_kernel=(100.0,)),
        Gate())
    assert plan.replay_runs == [{"job_id": 1, "kernel_idx": 0,
                                 "tile_begin": 1, "tile_count": 3}]
    assert plan.coalesced_extra_tiles == 1
    assert plan.replay_tiles == 3
    assert plan.replayed_ops == [2, 3, 4]
    _ = decision_config


def test_u24_published_middle_tile_blocks_coalescing() -> None:
    ledger, _layout = w1(4)
    decision = analyze(
        ledger, page_report(WorkloadLayouts.default().w1, OPERAND_A, 1, 150), 200,
        EngineConfig(policy=POLICY_B1, delta_cycles=100,
                     c_fix_by_kernel=(1000.0,), c_tile_by_kernel=(100.0,)))
    decision.affected = [2, 4]
    gate = Gate()
    gate.publish(DataRef(OPERAND_B, 2, 3), 180)
    plan = plan_execution(decision, ledger, EngineConfig(
        policy=POLICY_B1, c_fix_by_kernel=(1000.0,), c_tile_by_kernel=(100.0,)), gate)
    assert len(plan.replay_runs) == 2
    assert plan.coalesced_extra_tiles == 0

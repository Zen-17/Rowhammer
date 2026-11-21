#include <cstdlib>
#include <algorithm>
#include <fstream>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "gtest/gtest.h"

#include "Phase1Address.h"
#include "Phase2Checkpoint.h"
#include "Phase2Engine.h"
#include "Phase2Gate.h"
#include "Phase2Layout.h"
#include "Phase2Ledger.h"
#include "Phase2Types.h"

using namespace DRAMSim;
using namespace DRAMSim::phase2;

namespace
{
Phase1Geometry p2Geometry()
{
    Phase1Geometry geometry;
    geometry.num_channels = 1;
    geometry.num_ranks = 1;
    geometry.num_banks = 16;
    geometry.num_bank_groups = 4;
    geometry.num_rows = 16384;
    geometry.num_cols = 128;
    geometry.burst_length = 4;
    geometry.jedec_data_bus_bits = 64;
    return geometry;
}

Phase1PublicReport p2Report(int seq, uint64_t cycle, const std::string& error_class,
                            const std::string& region_type, uint64_t region_id)
{
    Phase1PublicReport report;
    report.report_seq = seq;
    report.report_cycle = cycle;
    report.error_class = error_class;
    report.region_type = region_type;
    report.region_id = region_id;
    return report;
}

void p2BuildW1(Ledger& ledger, int n_tiles, int base = 100, int step = 10, int duration = 5)
{
    ledger.setBaseRow(0, 0);
    ledger.setBaseRow(1, 128);
    ledger.setBaseRow(2, 256);
    for (int tile = 0; tile < n_tiles; tile++)
    {
        ledger.addCompleted(1, 0, tile, {{0, tile}, {1, tile}}, {{2, tile}},
                            base + tile * step, base + tile * step + duration);
    }
}

void p2BuildW2(Ledger& ledger, int n_tiles, bool mul0_dep = true)
{
    ledger.setBaseRow(0, 0);
    ledger.setBaseRow(1, 128);
    ledger.setBaseRow(3, 256);
    ledger.setBaseRow(4, 384);
    ledger.setBaseRow(5, 512);
    ledger.setBaseRow(6, 640);
    for (int tile = 0; tile < n_tiles; tile++)
        ledger.addCompleted(1, 0, tile, {{0, tile}, {1, tile}}, {{3, tile}},
                            100 + tile * 10, 105 + tile * 10);
    for (int tile = 0; tile < n_tiles; tile++)
        ledger.addCompleted(1, 1, tile, {{3, tile}, {4, tile}}, {{5, tile}},
                            200 + tile * 10, 205 + tile * 10,
                            mul0_dep || tile != 0);
    for (int tile = 0; tile < n_tiles; tile++)
        ledger.addCompleted(1, 2, tile, {{5, tile}}, {{6, tile}},
                            300 + tile * 10, 305 + tile * 10);
}

std::vector<DataRef> p2Refs(std::initializer_list<DataRef> values)
{
    return std::vector<DataRef>(values);
}

Decision p2Analyze(const Ledger& ledger, const Phase1PublicReport& report, uint64_t ack,
                   Policy policy, unsigned radius, uint64_t delta,
                   const Gate& gate = Gate(), const CheckpointView& checkpoint = CheckpointView(),
                   bool overflow = false)
{
    Phase1Address codec(p2Geometry());
    RecoveryEngine engine;
    EngineConfig config;
    config.policy = policy;
    config.radius_rows = radius;
    config.delta_cycles = delta;
    return engine.analyze(report, ack, config, ledger, gate, checkpoint, overflow, codec);
}

}  // namespace

TEST(Phase2Engine, U1SingleTileSelective)
{
    Ledger ledger;
    p2BuildW1(ledger, 4);
    Phase1Address codec(p2Geometry());
    const Phase1PublicReport report =
        p2Report(1, 150, "UE", "page", pageRegionId(0, 1));
    const Decision decision = p2Analyze(ledger, report, 200, Policy::B1_SELECTIVE, 0, 100);
    EXPECT_EQ(decision.status, P2Status::SELECTIVE_RECOVERED);
    EXPECT_EQ(decision.affected, std::vector<uint64_t>({2}));
    EXPECT_EQ(decision.seeds, std::vector<uint64_t>({2}));
    ASSERT_EQ(decision.restore_list.size(), 1u);
    EXPECT_EQ(decision.restore_list[0], (DataRef{0, 1, 0}));
    RecoveryEngine engine;
    const ExecutionPlan plan = engine.planExecution(decision, ledger);
    EXPECT_EQ(plan.replay_tiles, 1u);
    EXPECT_EQ(plan.restore_bytes, 4096u);
}

TEST(Phase2Engine, U2ForwardClosureW2)
{
    Ledger ledger;
    p2BuildW2(ledger, 4);
    const Phase1PublicReport report = p2Report(1, 400, "UE", "page", pageRegionId(0, 0));
    const Decision decision = p2Analyze(ledger, report, 400, Policy::B1_SELECTIVE, 0, 1000);
    EXPECT_EQ(decision.affected, std::vector<uint64_t>({1, 5, 9}));
    ASSERT_EQ(decision.restore_list.size(), 1u);
    EXPECT_EQ(decision.restore_list[0], (DataRef{0, 0, 0}));
}

TEST(Phase2Engine, U3RegenerateProducer)
{
    Ledger ledger;
    p2BuildW2(ledger, 4);
    const Phase1PublicReport report = p2Report(1, 400, "UE", "page", pageRegionId(256, 2));
    const Decision decision = p2Analyze(ledger, report, 400, Policy::B1_SELECTIVE, 0, 1000);
    EXPECT_EQ(decision.regenerate, std::vector<uint64_t>({3}));
    EXPECT_NE(std::find(decision.affected.begin(), decision.affected.end(), 3u),
              decision.affected.end());
    EXPECT_NE(std::find(decision.affected.begin(), decision.affected.end(), 7u),
              decision.affected.end());
    EXPECT_TRUE(decision.restore_list.empty());
    EXPECT_NE(decision.status, P2Status::OUTSIDE_WINDOW_NO_TRUSTED_INPUT);
}

TEST(Phase2Engine, U4ClassAwareVersusB1)
{
    Ledger ledger;
    p2BuildW1(ledger, 4);
    const Phase1PublicReport report = p2Report(1, 150, "CE", "page", pageRegionId(0, 1));
    const Decision b2 =
        p2Analyze(ledger, report, 200, Policy::B2_CLASS_AWARE, 0, 100);
    EXPECT_EQ(b2.status, P2Status::SOURCE_REPAIRED_ONLY);
    EXPECT_TRUE(b2.affected.empty());
    ASSERT_EQ(b2.restore_list.size(), 1u);
    EXPECT_EQ(b2.restore_list[0], (DataRef{0, 1, 0}));
    const Decision b1 = p2Analyze(ledger, report, 200, Policy::B1_SELECTIVE, 0, 100);
    EXPECT_EQ(b1.affected, std::vector<uint64_t>({2}));
}

TEST(Phase2Engine, U5IncompleteDependencyFallsBack)
{
    Ledger ledger;
    p2BuildW2(ledger, 4, false);
    const Phase1PublicReport report = p2Report(1, 400, "UE", "page", pageRegionId(0, 0));
    const Decision decision = p2Analyze(ledger, report, 400, Policy::B1_SELECTIVE, 0, 1000);
    EXPECT_EQ(decision.status, P2Status::WHOLE_JOB_FALLBACK);
    EXPECT_NE(std::find(decision.fallback_reasons.begin(), decision.fallback_reasons.end(),
                        "DEPENDENCY_INCOMPLETE"),
              decision.fallback_reasons.end());
    EXPECT_EQ(decision.affected.size(), 12u);
}

TEST(Phase2Engine, U6TruncatedLedgerFallsBack)
{
    Ledger ledger;
    p2BuildW1(ledger, 4);
    ledger.truncateForTest();
    const Phase1PublicReport report = p2Report(1, 150, "UE", "page", pageRegionId(0, 1));
    const Decision decision = p2Analyze(ledger, report, 200, Policy::B1_SELECTIVE, 0, 100);
    EXPECT_EQ(decision.status, P2Status::WHOLE_JOB_FALLBACK);
    EXPECT_NE(std::find(decision.fallback_reasons.begin(), decision.fallback_reasons.end(),
                        "LEDGER_TRUNCATED"),
              decision.fallback_reasons.end());
}

TEST(Phase2Engine, U7ReportOverflowFallsBack)
{
    Ledger ledger;
    p2BuildW1(ledger, 4);
    const Phase1PublicReport report = p2Report(1, 150, "UE", "page", pageRegionId(0, 1));
    const Decision decision =
        p2Analyze(ledger, report, 200, Policy::B1_SELECTIVE, 0, 100, Gate(),
                  CheckpointView(), true);
    EXPECT_EQ(decision.status, P2Status::WHOLE_JOB_FALLBACK);
}

TEST(Phase2Engine, U8ConsumedOutputEscape)
{
    Ledger ledger;
    p2BuildW1(ledger, 4);
    Gate gate;
    const DataRef output{2, 1, 2};
    gate.publish(output, 150);
    EXPECT_TRUE(gate.consume(output, 160));
    const Phase1PublicReport report = p2Report(1, 150, "UE", "page", pageRegionId(0, 1));
    const Decision decision =
        p2Analyze(ledger, report, 200, Policy::B1_SELECTIVE, 0, 100, gate);
    EXPECT_EQ(decision.status, P2Status::PARTIAL_WITH_ESCAPE);
    ASSERT_EQ(decision.escaped_outputs.size(), 1u);
    EXPECT_EQ(decision.escaped_outputs[0], output);
    EXPECT_EQ(decision.affected, std::vector<uint64_t>({2}));
}

TEST(Phase2Engine, U9MissingCheckpointBlocksExecution)
{
    Ledger ledger;
    p2BuildW1(ledger, 4);
    CheckpointView checkpoint;
    checkpoint.setAbsent(0, 1);
    const Phase1PublicReport report = p2Report(1, 150, "UE", "page", pageRegionId(0, 1));
    const Decision decision =
        p2Analyze(ledger, report, 200, Policy::B1_SELECTIVE, 0, 100, Gate(), checkpoint);
    EXPECT_EQ(decision.status, P2Status::OUTSIDE_WINDOW_NO_TRUSTED_INPUT);
    RecoveryEngine engine;
    const ExecutionPlan plan = engine.planExecution(decision, ledger);
    EXPECT_EQ(plan.replay_tiles, 0u);
    EXPECT_EQ(plan.restore_bytes, 0u);
    ASSERT_EQ(plan.poison_outputs.size(), 1u);
    EXPECT_EQ(plan.poison_outputs[0], (DataRef{2, 1, 2}));
}

TEST(Phase2Engine, U10TimeWindowBetweenReportAndAck)
{
    Ledger ledger;
    ledger.setBaseRow(0, 0);
    ledger.setBaseRow(40, 256);
    ledger.setBaseRow(41, 384);
    ledger.addCompleted(1, 0, 1, {{0, 1}}, {{40, 1}}, 800, 850);
    ledger.addCompleted(1, 1, 1, {{0, 1}}, {{41, 1}}, 1050, 1090);
    const Phase1PublicReport report = p2Report(1, 1000, "UE", "page", pageRegionId(0, 1));
    EngineConfig config;
    config.policy = Policy::B1_SELECTIVE;
    config.delta_cycles = 100;
    config.window_mode = WindowMode::DELTA;
    Phase1Address codec(p2Geometry());
    RecoveryEngine engine;
    const Decision decision =
        engine.analyze(report, 1100, config, ledger, Gate(), CheckpointView(), false, codec);
    EXPECT_EQ(decision.window_start, 900u);
    EXPECT_EQ(decision.window_end, 1100u);
    EXPECT_EQ(decision.seeds, std::vector<uint64_t>({2}));
    EXPECT_EQ(decision.affected, std::vector<uint64_t>({2}));
}

TEST(Phase2Engine, U11BankReportSeedsUseWindow)
{
    Ledger ledger;
    ledger.setBaseRow(0, 0);
    ledger.setBaseRow(1, 128);
    ledger.setBaseRow(2, 256);
    for (int tile = 0; tile < 3; tile++)
        ledger.addCompleted(1, 0, tile, {{0, tile}, {1, tile}}, {{2, tile}},
                            100 + tile * 10, 102 + tile * 10);
    const uint64_t pending = ledger.planOp(1, 0, 3, {{0, 3}, {1, 3}}, {{2, 3}});
    ledger.recordIssue(pending, 130);
    const Phase1PublicReport report = p2Report(1, 125, "UE", "bank", 0);
    const Decision decision = p2Analyze(ledger, report, 125, Policy::B1_SELECTIVE, 0, 5);
    EXPECT_EQ(decision.seeds, std::vector<uint64_t>({3}));
    EXPECT_EQ(decision.remaining, std::vector<uint64_t>({4}));
}

TEST(Phase2Engine, U12RadiusCeHasNoLowerTimeBound)
{
    Ledger ledger;
    p2BuildW1(ledger, 8);
    const Phase1PublicReport report = p2Report(1, 400, "CE", "page", pageRegionId(0, 1));
    const Decision decision =
        p2Analyze(ledger, report, 400, Policy::B2R_RADIUS, 1, 1000);
    ASSERT_EQ(decision.suspect_items.size(), 1u);
    EXPECT_EQ(decision.suspect_items[0], (DataRef{0, 1, 0}));
    EXPECT_EQ(decision.radius_suspect_items.size(), 7u);
    EXPECT_EQ(decision.seeds, std::vector<uint64_t>({1, 3, 4, 5, 6, 7, 8}));
    EXPECT_EQ(decision.window_start, 0u);
}

TEST(Phase2Engine, U13ReportBeforeDelta)
{
    Ledger ledger;
    p2BuildW1(ledger, 4);
    const Phase1PublicReport report = p2Report(1, 5, "UE", "page", pageRegionId(0, 1));
    const Decision decision = p2Analyze(ledger, report, 20, Policy::B1_SELECTIVE, 0, 50);
    EXPECT_EQ(decision.window_start, 0u);
    EXPECT_EQ(decision.window_end, 20u);
}

TEST(Phase2Engine, U14InPlaceUpdateFallsBack)
{
    Ledger ledger;
    ledger.setBaseRow(0, 0);
    ledger.addCompleted(1, 0, 0, {{0, 0}}, {{0, 0}}, 100, 105);
    const Phase1PublicReport report = p2Report(1, 150, "UE", "page", pageRegionId(0, 0));
    const Decision decision = p2Analyze(ledger, report, 200, Policy::B1_SELECTIVE, 0, 100);
    EXPECT_EQ(decision.status, P2Status::WHOLE_JOB_FALLBACK);
    EXPECT_NE(std::find(decision.fallback_reasons.begin(), decision.fallback_reasons.end(),
                        "IN_PLACE_UNSUPPORTED"),
              decision.fallback_reasons.end());
}

TEST(Phase2Engine, U15RepeatIsByteIdentical)
{
    Ledger ledger;
    p2BuildW1(ledger, 4);
    const Phase1PublicReport report = p2Report(1, 150, "UE", "page", pageRegionId(0, 1));
    const Decision first = p2Analyze(ledger, report, 200, Policy::B1_SELECTIVE, 0, 100);
    const Decision second = p2Analyze(ledger, report, 200, Policy::B1_SELECTIVE, 0, 100);
    EXPECT_EQ(decisionToJson(first), decisionToJson(second));
    EXPECT_NE(decisionToJson(first).find("\"affected\":[2]"), std::string::npos);
}

TEST(Phase2Engine, WholeJobPlanDeterministic)
{
    Ledger ledger;
    p2BuildW2(ledger, 4);
    Phase1Address codec(p2Geometry());
    RecoveryEngine engine;
    CheckpointView checkpoint;
    const Decision first = engine.planWholeJob(ledger, checkpoint);
    const Decision second = engine.planWholeJob(ledger, checkpoint);
    EXPECT_EQ(decisionToJson(first), decisionToJson(second));
    EXPECT_EQ(first.affected.size(), 12u);
    EXPECT_EQ(first.restore_list.size(), 12u);
}

TEST(Phase2Engine, U16ReleaseAtSameCycleStaysHeld)
{
    Gate gate;
    const DataRef ref{2, 0, 1};
    gate.hold(ref);
    std::map<DataRef, uint64_t> schedule;
    schedule[ref] = 100;
    gate.releaseDue(100, schedule);
    EXPECT_EQ(gate.state(ref), Lifecycle::HELD);
    gate.releaseDue(101, schedule);
    EXPECT_EQ(gate.state(ref), Lifecycle::PUBLISHED);
}

TEST(Phase2Engine, U17ReleaseBeforeScheduledCycleStaysHeld)
{
    Gate gate;
    const DataRef ref{2, 0, 1};
    gate.hold(ref);
    std::map<DataRef, uint64_t> schedule;
    schedule[ref] = 100;
    gate.releaseDue(99, schedule);
    EXPECT_EQ(gate.state(ref), Lifecycle::HELD);
}

TEST(Phase2Engine, U18ReportBeforeAckFreezesRelease)
{
    Gate gate;
    std::vector<DataRef> refs = {{2, 0, 1}, {2, 1, 2}, {2, 2, 3}};
    std::map<DataRef, uint64_t> schedule;
    for (const DataRef& ref : refs)
    {
        gate.hold(ref);
        schedule[ref] = 100 + ref.tile;
    }
    gate.freeze();
    for (uint64_t cycle = 0; cycle < 200; cycle++)
        gate.releaseDue(cycle, schedule);
    for (const DataRef& ref : refs)
        EXPECT_EQ(gate.state(ref), Lifecycle::HELD);
    gate.unfreeze();
    gate.releaseDue(201, schedule);
    for (const DataRef& ref : refs)
        EXPECT_EQ(gate.state(ref), Lifecycle::PUBLISHED);
}

TEST(Phase2Engine, U19PageAutoUsesBirth)
{
    Ledger ledger;
    ledger.setBaseRow(0, 0);
    ledger.setBaseRow(40, 256);
    ledger.addCompleted(1, 0, 1, {{0, 1}}, {{40, 1}}, 100, 150);
    Phase1Address codec(p2Geometry());
    const Phase1PublicReport report = p2Report(1, 1000, "UE", "page", pageRegionId(0, 1));
    RecoveryEngine engine;
    EngineConfig auto_config;
    auto_config.policy = Policy::B1_SELECTIVE;
    auto_config.delta_cycles = 500;
    auto_config.window_mode = WindowMode::AUTO;
    const Decision auto_decision =
        engine.analyze(report, 1100, auto_config, ledger, Gate(), CheckpointView(), false, codec);
    EXPECT_EQ(auto_decision.seeds, std::vector<uint64_t>({1}));
    EXPECT_EQ(auto_decision.affected, std::vector<uint64_t>({1}));
    EngineConfig delta_config = auto_config;
    delta_config.window_mode = WindowMode::DELTA;
    const Decision delta_decision =
        engine.analyze(report, 1100, delta_config, ledger, Gate(), CheckpointView(), false, codec);
    EXPECT_TRUE(delta_decision.seeds.empty());
}

TEST(Phase2Engine, U20BankAutoKeepsDelta)
{
    Ledger ledger;
    p2BuildW1(ledger, 3);
    OpEntry* first = ledger.findMutable(1);
    OpEntry* second = ledger.findMutable(2);
    OpEntry* third = ledger.findMutable(3);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    ASSERT_NE(third, nullptr);
    first->t_issue = 100; first->t_complete = 150;
    second->t_issue = 600; second->t_complete = 650;
    third->t_issue = 300; third->t_complete = 350;
    EngineConfig config;
    config.policy = Policy::B1_SELECTIVE;
    config.delta_cycles = 500;
    config.window_mode = WindowMode::AUTO;
    Phase1Address codec(p2Geometry());
    const Decision decision = p2Analyze(ledger, p2Report(1, 1000, "UE", "bank", 0),
                                        1100, Policy::B1_SELECTIVE, 0, 500);
    EXPECT_EQ(decision.seeds, std::vector<uint64_t>({2}));
}

TEST(Phase2Engine, U21BankReportFiltersReadItems)
{
    Ledger ledger;
    ledger.setBaseRow(0, 0);
    ledger.setBaseRow(2, 256);
    ledger.addCompleted(1, 0, 0, {{0, 0}}, {{2, 0}}, 100, 105);
    ledger.addCompleted(1, 0, 1, {{0, 1}}, {{2, 1}}, 200, 205);
    ledger.addCompleted(1, 0, 2, {{0, 2}}, {{2, 2}}, 500, 505);
    EngineConfig config;
    config.policy = Policy::B1_SELECTIVE;
    config.delta_cycles = 200;
    config.window_mode = WindowMode::DELTA;
    Phase1Address codec(p2Geometry());
    RecoveryEngine engine;
    const Decision decision =
        engine.analyze(p2Report(1, 600, "UE", "bank", 0), 600, config, ledger, Gate(),
                       CheckpointView(), false, codec);
    ASSERT_EQ(decision.r_read.size(), 1u);
    EXPECT_EQ(decision.r_read[0], (DataRef{0, 2, 0}));
    EXPECT_EQ(decision.seeds, std::vector<uint64_t>({3}));
}

TEST(Phase2Engine, U22EmptyReadSetFallsBack)
{
    Ledger ledger;
    p2BuildW1(ledger, 1);
    EngineConfig config;
    config.policy = Policy::B1_SELECTIVE;
    config.delta_cycles = 100;
    config.window_mode = WindowMode::DELTA;
    Phase1Address codec(p2Geometry());
    RecoveryEngine engine;
    const Decision decision =
        engine.analyze(p2Report(1, 1000, "UE", "bank", 0), 1000, config, ledger, Gate(),
                       CheckpointView(), false, codec);
    EXPECT_TRUE(decision.r_read.empty());
    EXPECT_NE(std::find(decision.fallback_reasons.begin(), decision.fallback_reasons.end(),
                        "UNLOGGED_READER"),
              decision.fallback_reasons.end());
    EXPECT_EQ(decision.status, P2Status::WHOLE_JOB_FALLBACK);
}

TEST(Phase2Engine, U23AdjacentReplayIntervalsCoalesce)
{
    Ledger ledger;
    p2BuildW1(ledger, 4);
    Phase1Address codec(p2Geometry());
    RecoveryEngine engine;
    EngineConfig config;
    config.policy = Policy::B1_SELECTIVE;
    config.delta_cycles = 100;
    config.c_fix_by_kernel = {1000.0};
    config.c_tile_by_kernel = {100.0};
    Decision decision = engine.analyze(
        p2Report(1, 150, "UE", "page", pageRegionId(0, 1)), 200, config, ledger,
        Gate(), CheckpointView(), false, codec);
    ASSERT_EQ(decision.affected, std::vector<uint64_t>({2}));
    decision.affected = {2, 4};
    Gate gate;
    const ExecutionPlan plan = engine.planExecution(decision, ledger, &config, &gate);
    ASSERT_EQ(plan.replay_runs.size(), 1u);
    EXPECT_EQ(plan.replay_runs[0].tile_begin, 1u);
    EXPECT_EQ(plan.replay_runs[0].tile_count, 3u);
    EXPECT_EQ(plan.coalesced_extra_tiles, 1u);
    EXPECT_EQ(plan.replay_tiles, 3u);
    EXPECT_EQ(plan.replayed_ops, std::vector<uint64_t>({2, 3, 4}));
}

TEST(Phase2Engine, U24PublishedMiddleTileBlocksCoalescing)
{
    Ledger ledger;
    p2BuildW1(ledger, 4);
    Phase1Address codec(p2Geometry());
    RecoveryEngine engine;
    EngineConfig config;
    config.policy = Policy::B1_SELECTIVE;
    config.delta_cycles = 100;
    config.c_fix_by_kernel = {1000.0};
    config.c_tile_by_kernel = {100.0};
    Decision decision = engine.analyze(
        p2Report(1, 150, "UE", "page", pageRegionId(0, 1)), 200, config, ledger,
        Gate(), CheckpointView(), false, codec);
    decision.affected = {2, 4};
    Gate gate;
    gate.publish(DataRef{2, 2, 3}, 180);
    const ExecutionPlan plan = engine.planExecution(decision, ledger, &config, &gate);
    EXPECT_EQ(plan.replay_runs.size(), 2u);
    EXPECT_EQ(plan.coalesced_extra_tiles, 0u);
}

TEST(Phase2AddressVectors, Export)
{
    const Phase1Geometry geometry = p2Geometry();
    Phase1Address codec(geometry);
    const char* path_env = std::getenv("PHASE2_ADDRESS_OUT");
    const std::string path = path_env == nullptr
                                 ? "../../results/phase2/l1/address_vectors.csv"
                                 : std::string(path_env);
    std::ofstream output(path.c_str(), std::ios::out | std::ios::trunc);
    ASSERT_TRUE(output.good()) << "cannot open " << path;

    std::mt19937_64 rng(20261115ULL);
    output << "addr,channel,rank,bank,row,burst_col\n";
    for (unsigned i = 0; i < 2000; i++)
    {
        Phase1AddrTuple tuple;
        tuple.channel = 0;
        tuple.rank = 0;
        tuple.bank = static_cast<unsigned>(rng() % geometry.num_banks);
        tuple.row = static_cast<unsigned>(rng() % geometry.num_rows);
        tuple.burst_column = static_cast<unsigned>(rng() % geometry.burst_columns());
        const uint64_t address = codec.encode(tuple);
        const Phase1AddrTuple decoded = codec.decode(address);
        EXPECT_EQ(tuple, decoded);
        output << address << ',' << decoded.channel << ',' << decoded.rank << ','
               << decoded.bank << ',' << decoded.row << ',' << decoded.burst_column << '\n';
    }
    for (unsigned i = 0; i < 1000; i++)
    {
        const uint64_t address = (rng() & codec.addressMask()) & ~uint64_t(0x1f);
        const Phase1AddrTuple decoded = codec.decode(address);
        EXPECT_EQ(address, codec.encode(decoded));
        output << address << ',' << decoded.channel << ',' << decoded.rank << ','
               << decoded.bank << ',' << decoded.row << ',' << decoded.burst_column << '\n';
    }
    output.close();
}

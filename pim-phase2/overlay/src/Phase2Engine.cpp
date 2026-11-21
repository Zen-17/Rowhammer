#include "Phase2Engine.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <utility>

#include "Phase2Layout.h"

namespace DRAMSim
{
namespace phase2
{
namespace
{
bool readsDataRef(const OpEntry& op, const std::set<DataRef>& suspects)
{
    for (const DataRef& ref : op.inputs)
        if (suspects.find(ref) != suspects.end())
            return true;
    return false;
}

std::vector<Run> groupRunsLocal(std::vector<const OpEntry*> entries)
{
    std::sort(entries.begin(), entries.end(), [](const OpEntry* lhs, const OpEntry* rhs) {
        if (lhs->job_id != rhs->job_id) return lhs->job_id < rhs->job_id;
        if (lhs->kernel_idx != rhs->kernel_idx) return lhs->kernel_idx < rhs->kernel_idx;
        return lhs->tile < rhs->tile;
    });
    std::vector<Run> runs;
    for (const OpEntry* op : entries)
    {
        if (!runs.empty() && runs.back().job_id == op->job_id &&
            runs.back().kernel_idx == op->kernel_idx &&
            runs.back().tile_begin + runs.back().tile_count == op->tile)
        {
            runs.back().tile_count++;
        }
        else
        {
            Run run;
            run.job_id = op->job_id;
            run.kernel_idx = op->kernel_idx;
            run.tile_begin = op->tile;
            run.tile_count = 1;
            runs.push_back(run);
        }
    }
    return runs;
}

bool sameWindowMode(const EngineConfig& config, const Phase1PublicReport& report)
{
    if (config.window_mode == WindowMode::BIRTH)
        return true;
    if (config.window_mode == WindowMode::DELTA)
        return false;
    return report.region_type == "page" || report.region_type == "codeword";
}
}  // namespace

std::vector<Run> RecoveryEngine::groupRuns(std::vector<const OpEntry*> entries)
{
    return groupRunsLocal(std::move(entries));
}

bool RecoveryEngine::readsAny(const OpEntry& op, const std::set<DataRef>& suspects,
                              uint64_t window_start, uint64_t window_end)
{
    if (!op.completed)
        return false;
    if (op.t_issue > window_end || op.t_complete < window_start)
        return false;
    return readsDataRef(op, suspects);
}

uint64_t RecoveryEngine::birthCycle(const Ledger& ledger, const DataRef& ref)
{
    if (ref.version == 0)
        return 0;
    uint64_t producer = 0;
    if (!ledger.producerOf(ref, producer))
        return 0;
    const OpEntry* op = ledger.find(producer);
    return op == nullptr ? 0 : op->t_complete;
}

uint64_t RecoveryEngine::itemWindowStart(const Ledger& ledger, const DataRef& ref,
                                         const Phase1PublicReport& report,
                                         const EngineConfig& config)
{
    const uint64_t birth = birthCycle(ledger, ref);
    if (sameWindowMode(config, report))
        return birth;
    const uint64_t lower = report.report_cycle > config.delta_cycles
                               ? report.report_cycle - config.delta_cycles
                               : 0;
    return std::max(birth, lower);
}

bool RecoveryEngine::readInWindow(const Ledger& ledger, const DataRef& ref,
                                  uint64_t window_start, uint64_t t_ack)
{
    for (const OpEntry& op : ledger.ops())
    {
        if (op.t_issue == UINT64_MAX || op.t_issue > t_ack)
            continue;
        const uint64_t finish = op.completed ? op.t_complete : t_ack + 1;
        if (finish < window_start)
            continue;
        for (const DataRef& input : op.inputs)
            if (input == ref)
                return true;
    }
    for (const HostRead& read : ledger.hostReads())
    {
        if (read.ref() != ref)
            continue;
        if (read.t_issue <= t_ack && read.t_complete >= window_start)
            return true;
    }
    return false;
}

bool RecoveryEngine::readsAnyPerItem(const OpEntry& op,
                                     const std::map<DataRef, uint64_t>& windows,
                                     uint64_t t_ack)
{
    if (!op.completed)
        return false;
    if (op.t_issue == UINT64_MAX || op.t_issue > t_ack)
        return false;
    const uint64_t finish = op.completed ? op.t_complete : t_ack + 1;
    for (const DataRef& input : op.inputs)
    {
        auto found = windows.find(input);
        if (found != windows.end() && finish >= found->second)
            return true;
    }
    return false;
}

std::vector<Run> RecoveryEngine::coalesceRuns(const std::vector<Run>& runs,
                                              const Decision& decision,
                                              const Ledger& ledger,
                                              const EngineConfig& config,
                                              const Gate& gate,
                                              uint64_t& coalesced_extra)
{
    coalesced_extra = 0;
    if (runs.empty() || config.c_fix_by_kernel.empty() || config.c_tile_by_kernel.empty())
        return runs;
    if (!decision.fallback_reasons.empty())
        return runs;

    std::set<DataRef> untrusted(decision.r_read.begin(), decision.r_read.end());
    untrusted.insert(decision.radius_suspect_items.begin(),
                     decision.radius_suspect_items.end());
    std::map<std::pair<uint16_t, uint32_t>, const OpEntry*> ops_by_kernel_tile;
    for (const OpEntry& op : ledger.ops())
        if (op.completed)
            ops_by_kernel_tile[std::make_pair(op.kernel_idx, op.tile)] = &op;

    std::vector<Run> merged;
    for (const Run& run : runs)
    {
        if (merged.empty())
        {
            merged.push_back(run);
            continue;
        }
        Run& prev = merged.back();
        if (prev.job_id != run.job_id || prev.kernel_idx != run.kernel_idx)
        {
            merged.push_back(run);
            continue;
        }
        const int64_t gap = static_cast<int64_t>(run.tile_begin) -
                            static_cast<int64_t>(prev.tile_begin + prev.tile_count);
        double c_fix = 0.0;
        double c_tile = 0.0;
        if (gap <= 0 || !config.kernelCosts(run.kernel_idx, c_fix, c_tile))
        {
            merged.push_back(run);
            continue;
        }
        if (static_cast<double>(gap) * c_tile >= c_fix)
        {
            merged.push_back(run);
            continue;
        }
        bool trusted = true;
        for (uint32_t tile = prev.tile_begin + prev.tile_count; tile < run.tile_begin; tile++)
        {
            auto found = ops_by_kernel_tile.find(std::make_pair(run.kernel_idx, tile));
            if (found == ops_by_kernel_tile.end())
            {
                trusted = false;
                break;
            }
            const OpEntry* op = found->second;
            for (const DataRef& input : op->inputs)
                if (untrusted.find(input) != untrusted.end())
                {
                    trusted = false;
                    break;
                }
            if (!trusted)
                break;
            for (const DataRef& out : op->outputs)
                if (gate.state(out) != Lifecycle::HELD)
                {
                    trusted = false;
                    break;
                }
            if (!trusted)
                break;
        }
        if (!trusted)
        {
            merged.push_back(run);
            continue;
        }
        prev.tile_count += static_cast<uint32_t>(gap) + run.tile_count;
        coalesced_extra += static_cast<uint64_t>(gap);
    }
    return merged;
}

Decision RecoveryEngine::analyze(const Phase1PublicReport& report, uint64_t t_ack,
                                 const EngineConfig& config, const Ledger& ledger,
                                 const Gate& gate, const CheckpointView& checkpoint,
                                 bool report_overflow,
                                 const Phase1Address& address) const
{
    const ReportFootprint footprint = buildReportFootprint(report, address);
    const ReportIndex index = buildReportIndex(footprint);

    std::vector<DataRef> suspect;
    std::vector<DataRef> radius_suspect;
    for (const DataRef& ref : ledger.liveItems())
    {
        const unsigned base = ledger.baseRow(ref.operand);
        if (itemOverlaps(address, base, ref.tile, index))
            suspect.push_back(ref);
        else if (config.policy == Policy::B2R_RADIUS && report.error_class == "CE" &&
                 itemTouchesRadius(base, ref.tile, index, config.radius_rows))
            radius_suspect.push_back(ref);
    }

    std::vector<ItemWindow> item_windows;
    std::map<DataRef, uint64_t> window_by_ref;
    for (const DataRef& ref : suspect)
    {
        const uint64_t window = itemWindowStart(ledger, ref, report, config);
        item_windows.push_back(ItemWindow{ref, window});
        window_by_ref[ref] = window;
    }
    for (const DataRef& ref : radius_suspect)
    {
        const uint64_t window = itemWindowStart(ledger, ref, report, config);
        item_windows.push_back(ItemWindow{ref, window});
        window_by_ref[ref] = window;
    }
    std::sort(item_windows.begin(), item_windows.end(),
              [](const ItemWindow& lhs, const ItemWindow& rhs) {
                  return lhs.ref < rhs.ref;
              });

    // Fix-3: only items actually read in their per-item window can be corrupt.
    std::vector<DataRef> r_read;
    for (const DataRef& ref : suspect)
        if (readInWindow(ledger, ref, window_by_ref[ref], t_ack))
            r_read.push_back(ref);

    std::vector<std::string> fallback_reasons;
    std::vector<DataRef> effective_suspect;
    if (!suspect.empty() && r_read.empty())
    {
        fallback_reasons.push_back("UNLOGGED_READER");
        effective_suspect = suspect;
    }
    else
    {
        effective_suspect = r_read;
    }

    std::set<DataRef> suspect_set(effective_suspect.begin(), effective_suspect.end());
    std::set<DataRef> radius_set(radius_suspect.begin(), radius_suspect.end());
    std::map<DataRef, uint64_t> effective_windows;
    for (const DataRef& ref : effective_suspect)
        effective_windows[ref] = window_by_ref[ref];

    const uint64_t window_start =
        report.report_cycle > config.delta_cycles ? report.report_cycle - config.delta_cycles
                                                  : 0;
    const uint64_t window_end = t_ack;

    std::set<uint64_t> seeds;
    const bool ce = (report.error_class == "CE");
    if (config.policy == Policy::B1_SELECTIVE)
    {
        for (const OpEntry& op : ledger.ops())
            if (readsAnyPerItem(op, effective_windows, window_end))
                seeds.insert(op.op_id);
    }
    else if (config.policy == Policy::B2_CLASS_AWARE)
    {
        if (!ce)
            for (const OpEntry& op : ledger.ops())
                if (readsAnyPerItem(op, effective_windows, window_end))
                    seeds.insert(op.op_id);
    }
    else if (config.policy == Policy::B2R_RADIUS)
    {
        if (!ce)
        {
            for (const OpEntry& op : ledger.ops())
                if (readsAnyPerItem(op, effective_windows, window_end))
                    seeds.insert(op.op_id);
        }
        else
        {
            for (const OpEntry& op : ledger.ops())
                if (readsAny(op, radius_set, 0, window_end))
                    seeds.insert(op.op_id);
        }
    }
    else
    {
        throw std::invalid_argument("unknown Phase 2 policy");
    }

    std::map<std::pair<uint16_t, uint32_t>, DataRef> restore;
    std::set<uint64_t> regenerate;
    std::set<DataRef> missing;
    bool producer_missing = false;
    std::map<std::pair<uint16_t, uint32_t>, DataRef> suspicious;
    for (const DataRef& ref : effective_suspect)
        suspicious[ref.item()] = ref;
    for (const DataRef& ref : radius_suspect)
        suspicious[ref.item()] = ref;
    for (const auto& entry : suspicious)
    {
        const DataRef& ref = entry.second;
        if (ref.version == 0)
        {
            if (checkpoint.has(ref.operand, ref.tile))
                restore[entry.first] = ref;
            else
                missing.insert(ref);
        }
        else
        {
            uint64_t producer = 0;
            if (ledger.producerOf(ref, producer))
                regenerate.insert(producer);
            else
                producer_missing = true;
        }
    }

    std::set<uint64_t> affected(seeds.begin(), seeds.end());
    affected.insert(regenerate.begin(), regenerate.end());
    std::set<uint64_t> pending(affected.begin(), affected.end());
    while (!pending.empty())
    {
        const uint64_t op_id = *pending.begin();
        pending.erase(pending.begin());
        const OpEntry* op = ledger.find(op_id);
        if (op == nullptr)
            continue;
        for (const DataRef& out : op->outputs)
        {
            for (uint64_t reader : ledger.readersOf(out))
            {
                if (affected.insert(reader).second)
                    pending.insert(reader);
            }
        }
        for (const DataRef& ref : op->inputs)
        {
            if (ref.version == 0)
                continue;
            if (suspect_set.find(ref) == suspect_set.end() &&
                radius_set.find(ref) == radius_set.end())
                continue;
            uint64_t producer = 0;
            if (ledger.producerOf(ref, producer) && affected.insert(producer).second)
                pending.insert(producer);
        }
    }

    if (ledger.truncated())
        fallback_reasons.push_back("LEDGER_TRUNCATED");
    if (report_overflow)
        fallback_reasons.push_back("REPORT_OVERFLOW");
    if (!ledger.writeDisciplineReasons().empty())
        fallback_reasons.push_back("IN_PLACE_UNSUPPORTED");
    if (producer_missing)
        fallback_reasons.push_back("PRODUCER_MISSING");
    for (uint64_t op_id : affected)
    {
        const OpEntry* op = ledger.find(op_id);
        if (op != nullptr && !op->dep_complete)
        {
            fallback_reasons.push_back("DEPENDENCY_INCOMPLETE");
            break;
        }
    }

    if (!fallback_reasons.empty())
    {
        std::set<uint32_t> involved_jobs;
        for (uint64_t op_id : affected)
        {
            const OpEntry* op = ledger.find(op_id);
            if (op != nullptr)
                involved_jobs.insert(op->job_id);
        }
        if (involved_jobs.empty())
            for (uint64_t job : ledger.jobs())
                involved_jobs.insert(static_cast<uint32_t>(job));
        affected.clear();
        restore.clear();
        for (uint32_t job : involved_jobs)
            for (uint64_t op_id : ledger.jobCompletedOpIds(job))
                affected.insert(op_id);
        for (uint32_t job : involved_jobs)
        {
            for (const DataRef& ref : ledger.jobInitialInputs(job))
            {
                if (checkpoint.has(ref.operand, ref.tile))
                    restore[ref.item()] = ref;
                else
                    missing.insert(ref);
            }
        }
    }

    P2Status status = P2Status::SELECTIVE_RECOVERED;
    std::vector<DataRef> escaped;
    if (!missing.empty())
    {
        status = P2Status::OUTSIDE_WINDOW_NO_TRUSTED_INPUT;
    }
    else if (!fallback_reasons.empty())
    {
        status = P2Status::WHOLE_JOB_FALLBACK;
    }
    else
    {
        Decision probe;
        probe.affected.assign(affected.begin(), affected.end());
        probe.suspect_items = suspect;
        probe.radius_suspect_items = radius_suspect;
        for (const DataRef& ref : affectedOutputs(probe, ledger))
            if (gate.state(ref) == Lifecycle::CONSUMED)
                escaped.push_back(ref);
        if (!escaped.empty())
            status = P2Status::PARTIAL_WITH_ESCAPE;
        else if (!affected.empty())
            status = P2Status::SELECTIVE_RECOVERED;
        else
            status = P2Status::SOURCE_REPAIRED_ONLY;
    }

    Decision decision;
    decision.status = status;
    decision.window_start = window_start;
    decision.window_end = window_end;
    decision.suspect_items = suspect;
    decision.radius_suspect_items = radius_suspect;
    decision.r_read = r_read;
    decision.item_windows = item_windows;
    decision.seeds.assign(seeds.begin(), seeds.end());
    decision.regenerate.assign(regenerate.begin(), regenerate.end());
    decision.affected.assign(affected.begin(), affected.end());
    for (const auto& entry : restore)
        decision.restore_list.push_back(entry.second);
    decision.escaped_outputs = escaped;
    decision.remaining = ledger.remainingOpIds();
    decision.fallback_reasons = fallback_reasons;
    std::sort(decision.suspect_items.begin(), decision.suspect_items.end());
    std::sort(decision.radius_suspect_items.begin(), decision.radius_suspect_items.end());
    std::sort(decision.restore_list.begin(), decision.restore_list.end());
    std::sort(decision.escaped_outputs.begin(), decision.escaped_outputs.end());
    std::sort(decision.r_read.begin(), decision.r_read.end());

    const ExecutionPlan plan = planExecution(decision, ledger, &config, &gate);
    decision.replay_runs = plan.replay_runs;
    decision.coalesced_extra_tiles = plan.coalesced_extra_tiles;
    return decision;
}

Decision RecoveryEngine::planWholeJob(const Ledger& ledger,
                                      const CheckpointView& checkpoint,
                                      const std::string& reason, int64_t job_id) const
{
    (void)checkpoint;
    std::vector<uint32_t> jobs;
    if (job_id >= 0)
    {
        jobs.push_back(static_cast<uint32_t>(job_id));
    }
    else
    {
        for (uint64_t job : ledger.jobs())
            jobs.push_back(static_cast<uint32_t>(job));
    }
    std::set<uint64_t> affected;
    std::map<std::pair<uint16_t, uint32_t>, DataRef> restore;
    for (uint32_t job : jobs)
    {
        for (uint64_t op_id : ledger.jobCompletedOpIds(job))
            affected.insert(op_id);
        for (const DataRef& ref : ledger.jobInitialInputs(job))
            restore[ref.item()] = ref;
    }
    Decision decision;
    decision.status = P2Status::BASELINE_B0;
    decision.affected.assign(affected.begin(), affected.end());
    for (const auto& entry : restore)
        decision.restore_list.push_back(entry.second);
    decision.remaining = ledger.remainingOpIds();
    decision.fallback_reasons.push_back(reason);
    std::vector<const OpEntry*> ops;
    for (uint64_t op_id : decision.affected)
    {
        const OpEntry* op = ledger.find(op_id);
        if (op != nullptr)
            ops.push_back(op);
    }
    decision.replay_runs = groupRuns(std::move(ops));
    return decision;
}

std::vector<DataRef> RecoveryEngine::affectedOutputs(const Decision& decision,
                                                     const Ledger& ledger) const
{
    std::map<std::pair<uint16_t, uint32_t>, DataRef> outputs;
    for (uint64_t op_id : decision.affected)
    {
        const OpEntry* op = ledger.find(op_id);
        if (op == nullptr)
            continue;
        for (const DataRef& out : op->outputs)
            if (ledger.isFinalItem(op->job_id, out.operand, out.tile))
                outputs[out.item()] = out;
    }
    std::vector<DataRef> refs = decision.suspect_items;
    refs.insert(refs.end(), decision.radius_suspect_items.begin(),
                decision.radius_suspect_items.end());
    for (const DataRef& ref : refs)
    {
        for (uint64_t job : ledger.jobs())
            if (ledger.isFinalItem(static_cast<uint32_t>(job), ref.operand, ref.tile))
                outputs[ref.item()] = ref;
    }
    std::vector<DataRef> result;
    for (const auto& entry : outputs)
        result.push_back(entry.second);
    std::sort(result.begin(), result.end());
    return result;
}

ExecutionPlan RecoveryEngine::planExecution(const Decision& decision, const Ledger& ledger,
                                            const EngineConfig* config,
                                            const Gate* gate) const
{
    ExecutionPlan plan;
    if (decision.status == P2Status::OUTSIDE_WINDOW_NO_TRUSTED_INPUT)
    {
        plan.poison_outputs = affectedOutputs(decision, ledger);
        return plan;
    }
    plan.restore_items = decision.restore_list.size();
    plan.restore_bytes = 4096ULL * plan.restore_items;
    std::vector<const OpEntry*> replay_ops;
    for (uint64_t op_id : decision.affected)
    {
        const OpEntry* op = ledger.find(op_id);
        if (op != nullptr)
        {
            replay_ops.push_back(op);
            plan.replayed_ops.push_back(op_id);
        }
    }
    plan.replay_tiles = replay_ops.size();
    std::vector<Run> runs = groupRuns(std::move(replay_ops));
    if (config != nullptr)
    {
        Gate empty_gate;
        const Gate& state = gate == nullptr ? empty_gate : *gate;
        uint64_t extra = 0;
        runs = coalesceRuns(runs, decision, ledger, *config, state, extra);
        plan.coalesced_extra_tiles = extra;
        if (extra > 0)
        {
            std::set<uint64_t> expanded(plan.replayed_ops.begin(), plan.replayed_ops.end());
            for (const Run& run : runs)
            {
                for (uint32_t tile = run.tile_begin; tile < run.tile_begin + run.tile_count;
                     tile++)
                {
                    for (const OpEntry& op : ledger.ops())
                    {
                        if (op.job_id == run.job_id && op.kernel_idx == run.kernel_idx &&
                            op.tile == tile && op.completed)
                            expanded.insert(op.op_id);
                    }
                }
            }
            plan.replayed_ops.assign(expanded.begin(), expanded.end());
            plan.replay_tiles = 0;
            for (const Run& run : runs)
                plan.replay_tiles += run.tile_count;
        }
    }
    plan.replay_runs = runs;
    std::vector<const OpEntry*> remaining_ops;
    for (uint64_t op_id : decision.remaining)
    {
        const OpEntry* op = ledger.find(op_id);
        if (op != nullptr)
            remaining_ops.push_back(op);
    }
    plan.remaining_runs = groupRuns(std::move(remaining_ops));
    return plan;
}

uint64_t RecoveryEngine::ancestorCompleteTime(const Ledger& ledger, uint64_t op_id,
                                              std::map<uint64_t, uint64_t>& memo) const
{
    auto cached = memo.find(op_id);
    if (cached != memo.end())
        return cached->second;
    const OpEntry* op = ledger.find(op_id);
    if (op == nullptr)
        return 0;
    uint64_t latest = op->t_complete;
    for (const DataRef& ref : op->inputs)
    {
        uint64_t producer = 0;
        if (ledger.producerOf(ref, producer) && producer != op_id)
            latest = std::max(latest, ancestorCompleteTime(ledger, producer, memo));
    }
    memo[op_id] = latest;
    return latest;
}

std::map<DataRef, uint64_t> RecoveryEngine::globalHoldSchedule(const Ledger& ledger,
                                                               uint64_t hold_cycles) const
{
    std::map<uint32_t, uint64_t> job_end;
    for (const OpEntry& op : ledger.ops())
        if (op.completed)
            job_end[op.job_id] = std::max(job_end[op.job_id], op.t_complete);
    std::map<DataRef, uint64_t> schedule;
    for (const OpEntry& op : ledger.ops())
    {
        if (!op.completed)
            continue;
        for (const DataRef& out : op.outputs)
            schedule[out] = job_end[op.job_id] + hold_cycles;
    }
    return schedule;
}

std::map<DataRef, uint64_t> RecoveryEngine::depReleaseSchedule(const Ledger& ledger,
                                                               uint64_t delta_cycles) const
{
    std::map<uint64_t, uint64_t> memo;
    std::map<DataRef, uint64_t> schedule;
    for (const OpEntry& op : ledger.ops())
    {
        if (!op.completed)
            continue;
        const uint64_t release =
            ancestorCompleteTime(ledger, op.op_id, memo) + delta_cycles;
        for (const DataRef& out : op.outputs)
            schedule[out] = release;
    }
    return schedule;
}

}  // namespace phase2
}  // namespace DRAMSim

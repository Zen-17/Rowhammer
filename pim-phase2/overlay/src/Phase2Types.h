#ifndef PHASE2_TYPES_H
#define PHASE2_TYPES_H

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace DRAMSim
{
namespace phase2
{
enum class Policy
{
    B1_SELECTIVE,
    B2_CLASS_AWARE,
    B2R_RADIUS
};

enum class WindowMode
{
    DELTA,
    BIRTH,
    AUTO
};

enum class ReleasePolicy
{
    GLOBAL_HOLD,
    DEP_RELEASE
};

enum class P2Status
{
    OUTSIDE_WINDOW_NO_TRUSTED_INPUT,
    WHOLE_JOB_FALLBACK,
    PARTIAL_WITH_ESCAPE,
    SELECTIVE_RECOVERED,
    SOURCE_REPAIRED_ONLY,
    BASELINE_B0
};

enum class Lifecycle
{
    HELD,
    PUBLISHED,
    CONSUMED,
    POISONED
};

struct DataRef
{
    uint16_t operand = 0;
    uint32_t tile = 0;
    uint64_t version = 0;  // version 0 == INITIAL

    bool operator==(const DataRef& rhs) const
    {
        return operand == rhs.operand && tile == rhs.tile && version == rhs.version;
    }
    bool operator!=(const DataRef& rhs) const { return !(*this == rhs); }
    bool operator<(const DataRef& rhs) const
    {
        if (operand != rhs.operand) return operand < rhs.operand;
        if (tile != rhs.tile) return tile < rhs.tile;
        return version < rhs.version;
    }
    std::pair<uint16_t, uint32_t> item() const { return std::make_pair(operand, tile); }
};

struct HostRead
{
    uint16_t operand = 0;
    uint32_t tile = 0;
    uint64_t version = 0;
    uint64_t t_issue = 0;
    uint64_t t_complete = 0;
    DataRef ref() const { return DataRef{operand, tile, version}; }
};

struct BurstKey
{
    unsigned channel = 0;
    unsigned rank = 0;
    unsigned bank = 0;
    unsigned row = 0;
    unsigned burst_col = 0;

    bool operator==(const BurstKey& rhs) const
    {
        return channel == rhs.channel && rank == rhs.rank && bank == rhs.bank &&
               row == rhs.row && burst_col == rhs.burst_col;
    }
    bool operator<(const BurstKey& rhs) const
    {
        if (channel != rhs.channel) return channel < rhs.channel;
        if (rank != rhs.rank) return rank < rhs.rank;
        if (bank != rhs.bank) return bank < rhs.bank;
        if (row != rhs.row) return row < rhs.row;
        return burst_col < rhs.burst_col;
    }
};

struct OpEntry
{
    uint64_t op_id = 0;
    uint32_t job_id = 0;
    uint16_t kernel_idx = 0;
    uint32_t tile = 0;
    std::vector<DataRef> inputs;
    std::vector<DataRef> outputs;
    uint64_t t_issue = UINT64_MAX;
    uint64_t t_complete = UINT64_MAX;
    bool completed = false;
    bool dep_complete = true;
    bool is_replay = false;
};

struct EngineConfig
{
    Policy policy = Policy::B1_SELECTIVE;
    unsigned radius_rows = 0;
    uint64_t delta_cycles = 0;
    uint64_t max_log_entries = 100000;
    WindowMode window_mode = WindowMode::AUTO;
    std::vector<double> c_fix_by_kernel;
    std::vector<double> c_tile_by_kernel;

    bool kernelCosts(unsigned kernel_idx, double& c_fix, double& c_tile) const
    {
        if (kernel_idx >= c_fix_by_kernel.size() || kernel_idx >= c_tile_by_kernel.size())
            return false;
        c_fix = c_fix_by_kernel[kernel_idx];
        c_tile = c_tile_by_kernel[kernel_idx];
        return true;
    }
};

struct Run
{
    uint32_t job_id = 0;
    uint16_t kernel_idx = 0;
    uint32_t tile_begin = 0;
    uint32_t tile_count = 0;
};

struct ItemWindow
{
    DataRef ref;
    uint64_t window_start = 0;
};

struct Decision
{
    P2Status status = P2Status::SELECTIVE_RECOVERED;
    uint64_t window_start = 0;
    uint64_t window_end = 0;
    std::vector<DataRef> suspect_items;
    std::vector<DataRef> radius_suspect_items;
    std::vector<DataRef> r_read;
    std::vector<ItemWindow> item_windows;
    std::vector<uint64_t> seeds;
    std::vector<uint64_t> regenerate;
    std::vector<uint64_t> affected;
    std::vector<DataRef> restore_list;
    std::vector<DataRef> escaped_outputs;
    std::vector<uint64_t> remaining;
    std::vector<std::string> fallback_reasons;
    std::vector<Run> replay_runs;
    uint64_t coalesced_extra_tiles = 0;
};

struct P2Metrics
{
    P2Status status = P2Status::SELECTIVE_RECOVERED;
    uint64_t total_logged_ops = 0;
    uint64_t ledger_bytes_est = 0;
    uint64_t peak_held_items = 0;
    uint64_t affected_ops = 0;
    uint64_t replayed_tiles = 0;
    uint64_t replay_runs = 0;
    uint64_t coalesced_extra_tiles = 0;
    uint64_t restore_bytes = 0;
    uint64_t isolation_bytes = 0;
    uint64_t quiesce_cycles = 0;
    uint64_t restore_cycles = 0;
    uint64_t replay_cycles = 0;
    uint64_t resume_cycles = 0;
    uint64_t publish_cycles = 0;
    uint64_t recovery_cycles = 0;
    uint64_t recovery_cycles_excl_resume = 0;
    uint64_t fault_overhead_cycles = 0;
    uint64_t end_to_end_cycles = 0;
    uint64_t escaped_outputs = 0;
    uint64_t reports_during_recovery = 0;
    uint64_t reader_partial_at_ack = 0;
};

struct ExecutionPlan
{
    std::vector<Run> replay_runs;
    std::vector<Run> remaining_runs;
    uint64_t replay_tiles = 0;
    uint64_t restore_items = 0;
    uint64_t restore_bytes = 0;
    uint64_t coalesced_extra_tiles = 0;
    std::vector<uint64_t> replayed_ops;
    std::vector<DataRef> poison_outputs;
};

const char* p2StatusName(P2Status status);
const char* lifecycleName(Lifecycle state);
const char* policyName(Policy policy);
const char* windowModeName(WindowMode mode);

std::string dataRefCsv(const DataRef& ref);
std::string dataRefsCsv(const std::vector<DataRef>& refs);
std::string decisionToJson(const Decision& decision);

}  // namespace phase2
}  // namespace DRAMSim

#endif

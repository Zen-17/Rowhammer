#include "Phase2Types.h"

#include <sstream>

namespace DRAMSim
{
namespace phase2
{
const char* p2StatusName(P2Status status)
{
    switch (status)
    {
        case P2Status::OUTSIDE_WINDOW_NO_TRUSTED_INPUT:
            return "OUTSIDE_WINDOW_NO_TRUSTED_INPUT";
        case P2Status::WHOLE_JOB_FALLBACK: return "WHOLE_JOB_FALLBACK";
        case P2Status::PARTIAL_WITH_ESCAPE: return "PARTIAL_WITH_ESCAPE";
        case P2Status::SELECTIVE_RECOVERED: return "SELECTIVE_RECOVERED";
        case P2Status::SOURCE_REPAIRED_ONLY: return "SOURCE_REPAIRED_ONLY";
        case P2Status::BASELINE_B0: return "BASELINE_B0";
        case P2Status::STALE_EPOCH: return "STALE_EPOCH";
    }
    return "UNKNOWN";
}

const char* lifecycleName(Lifecycle state)
{
    switch (state)
    {
        case Lifecycle::HELD: return "HELD";
        case Lifecycle::PUBLISHED: return "PUBLISHED";
        case Lifecycle::CONSUMED: return "CONSUMED";
        case Lifecycle::POISONED: return "POISONED";
    }
    return "UNKNOWN";
}

const char* policyName(Policy policy)
{
    switch (policy)
    {
        case Policy::B1_SELECTIVE: return "B1_SELECTIVE";
        case Policy::B2_CLASS_AWARE: return "B2_CLASS_AWARE";
        case Policy::B2R_RADIUS: return "B2R_RADIUS";
    }
    return "UNKNOWN";
}

const char* windowModeName(WindowMode mode)
{
    switch (mode)
    {
        case WindowMode::DELTA: return "DELTA";
        case WindowMode::BIRTH: return "BIRTH";
        case WindowMode::AUTO: return "AUTO";
    }
    return "UNKNOWN";
}

std::string dataRefCsv(const DataRef& ref)
{
    std::ostringstream os;
    os << ref.operand << ':' << ref.tile << ':' << ref.version;
    return os.str();
}

std::string dataRefsCsv(const std::vector<DataRef>& refs)
{
    std::ostringstream os;
    for (std::size_t i = 0; i < refs.size(); i++)
    {
        if (i != 0) os << ';';
        os << dataRefCsv(refs[i]);
    }
    return os.str();
}

namespace
{
void appendUintList(std::ostringstream& os, const std::vector<uint64_t>& values)
{
    os << '[';
    for (std::size_t i = 0; i < values.size(); i++)
    {
        if (i != 0) os << ',';
        os << values[i];
    }
    os << ']';
}

void appendStringList(std::ostringstream& os, const std::vector<std::string>& values)
{
    os << '[';
    for (std::size_t i = 0; i < values.size(); i++)
    {
        if (i != 0) os << ',';
        os << '"' << values[i] << '"';
    }
    os << ']';
}

void appendRefList(std::ostringstream& os, const std::vector<DataRef>& values)
{
    os << '[';
    for (std::size_t i = 0; i < values.size(); i++)
    {
        if (i != 0) os << ',';
        os << "{\"operand\":" << values[i].operand << ",\"tile\":" << values[i].tile
           << ",\"version\":" << values[i].version << '}';
    }
    os << ']';
}

void appendItemWindows(std::ostringstream& os, const std::vector<ItemWindow>& values)
{
    os << '[';
    for (std::size_t i = 0; i < values.size(); i++)
    {
        if (i != 0) os << ',';
        os << "{\"operand\":" << values[i].ref.operand
           << ",\"tile\":" << values[i].ref.tile
           << ",\"version\":" << values[i].ref.version
           << ",\"window_start\":" << values[i].window_start << '}';
    }
    os << ']';
}

void appendRuns(std::ostringstream& os, const std::vector<Run>& values)
{
    os << '[';
    for (std::size_t i = 0; i < values.size(); i++)
    {
        if (i != 0) os << ',';
        os << "{\"job_id\":" << values[i].job_id
           << ",\"kernel_idx\":" << values[i].kernel_idx
           << ",\"tile_begin\":" << values[i].tile_begin
           << ",\"tile_count\":" << values[i].tile_count << '}';
    }
    os << ']';
}
}  // namespace

std::string decisionToJson(const Decision& decision)
{
    // Compact, keys in the same alphabetical order as Python's
    // json.dumps(..., sort_keys=True, separators=(',', ':')).
    std::ostringstream os;
    os << "{\"affected\":";
    appendUintList(os, decision.affected);
    os << ",\"coalesced_extra_tiles\":" << decision.coalesced_extra_tiles;
    os << ",\"escaped_outputs\":";
    appendRefList(os, decision.escaped_outputs);
    os << ",\"fallback_reasons\":";
    appendStringList(os, decision.fallback_reasons);
    os << ",\"item_windows\":";
    appendItemWindows(os, decision.item_windows);
    os << ",\"r_read\":";
    appendRefList(os, decision.r_read);
    os << ",\"radius_suspect_items\":";
    appendRefList(os, decision.radius_suspect_items);
    os << ",\"regenerate\":";
    appendUintList(os, decision.regenerate);
    os << ",\"remaining\":";
    appendUintList(os, decision.remaining);
    os << ",\"replay_runs\":";
    appendRuns(os, decision.replay_runs);
    os << ",\"restore_list\":";
    appendRefList(os, decision.restore_list);
    os << ",\"seeds\":";
    appendUintList(os, decision.seeds);
    os << ",\"status\":\"" << p2StatusName(decision.status) << '"';
    os << ",\"suspect_items\":";
    appendRefList(os, decision.suspect_items);
    os << ",\"window_end\":" << decision.window_end;
    os << ",\"window_start\":" << decision.window_start;
    os << '}';
    return os.str();
}

}  // namespace phase2
}  // namespace DRAMSim

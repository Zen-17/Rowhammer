#include "Phase1Driver.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

#include "Utils.h"

namespace DRAMSim
{
namespace
{
std::string trim(const std::string& input)
{
    const std::string whitespace = " \t\r\n";
    const std::size_t first = input.find_first_not_of(whitespace);
    if (first == std::string::npos)
        return "";
    const std::size_t last = input.find_last_not_of(whitespace);
    return input.substr(first, last - first + 1);
}

unsigned parseUnsigned(const std::string& key, const std::string& value)
{
    try
    {
        std::size_t consumed = 0;
        const unsigned long parsed = std::stoul(value, &consumed);
        if (consumed != value.size())
            throw std::invalid_argument("trailing characters");
        return static_cast<unsigned>(parsed);
    }
    catch (const std::exception&)
    {
        throw std::runtime_error("invalid unsigned parameter " + key + "=" + value);
    }
}

uint64_t parseUint64(const std::string& key, const std::string& value)
{
    try
    {
        std::size_t consumed = 0;
        const unsigned long long parsed = std::stoull(value, &consumed);
        if (consumed != value.size())
            throw std::invalid_argument("trailing characters");
        return static_cast<uint64_t>(parsed);
    }
    catch (const std::exception&)
    {
        throw std::runtime_error("invalid uint64 parameter " + key + "=" + value);
    }
}
}  // namespace

Phase1RunConfig loadPhase1Config(const std::string& path)
{
    std::ifstream input(path);
    if (!input)
        throw std::runtime_error("cannot open Phase1 config: " + path);

    std::map<std::string, std::string> values;
    static const std::unordered_set<std::string> allowed = {
        "schema_version", "experiment", "workload", "mode", "device_ini", "system_ini",
        "channels", "ranks", "capacity_mb", "n_tiles", "seed", "feeder_outstanding_limit",
        "feeder_issue_per_cycle", "watchdog_cycles", "dim_bursts", "elements", "output_dir",
        "trace_mode"};
    std::string line;
    unsigned lineNumber = 0;
    while (std::getline(input, line))
    {
        lineNumber++;
        line = trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#')
            continue;
        const std::size_t equal = line.find('=');
        if (equal == std::string::npos)
            throw std::runtime_error("malformed Phase1 config line " + std::to_string(lineNumber));
        const std::string key = trim(line.substr(0, equal));
        const std::string value = trim(line.substr(equal + 1));
        if (allowed.count(key) == 0)
            throw std::runtime_error("unknown Phase1 config key: " + key);
        values[key] = value;
    }

    auto require = [&](const std::string& key) -> const std::string& {
        auto it = values.find(key);
        if (it == values.end())
            throw std::runtime_error("missing Phase1 config key: " + key);
        return it->second;
    };

    Phase1RunConfig config;
    config.experiment = require("experiment");
    config.workload = require("workload");
    config.mode = require("mode");
    config.device_ini = require("device_ini");
    config.system_ini = require("system_ini");
    auto traceIt = values.find("trace_mode");
    config.trace_mode = (traceIt == values.end()) ? "ext_off" : traceIt->second;
    config.channels = parseUnsigned("channels", require("channels"));
    config.ranks = parseUnsigned("ranks", require("ranks"));
    config.capacity_mb = parseUnsigned("capacity_mb", require("capacity_mb"));
    config.n_tiles = parseUnsigned("n_tiles", require("n_tiles"));
    config.seed = parseUnsigned("seed", require("seed"));
    config.feeder_outstanding_limit =
        parseUnsigned("feeder_outstanding_limit", require("feeder_outstanding_limit"));
    config.feeder_issue_per_cycle =
        parseUnsigned("feeder_issue_per_cycle", require("feeder_issue_per_cycle"));
    config.watchdog_cycles = parseUint64("watchdog_cycles", require("watchdog_cycles"));

    if (config.mode != "direct" && config.mode != "feeder")
        throw std::runtime_error("mode must be direct or feeder");
    if (config.trace_mode != "ext_off" && config.trace_mode != "ext_on")
        throw std::runtime_error("trace_mode must be ext_off or ext_on");
    if (config.workload != "W0" && config.workload != "W1" && config.workload != "ADDRESS")
        throw std::runtime_error("workload must be W0, W1, or ADDRESS");
    if (config.channels != 1 && config.channels != 2)
        throw std::runtime_error("G1 supports only 1 or 2 channels");
    if (config.ranks != 1)
        throw std::runtime_error("G1 supports exactly 1 rank per channel");
    if (config.feeder_issue_per_cycle != 1)
        throw std::runtime_error("G1 feeder issue rate must be 1");
    if (config.watchdog_cycles == 0)
        throw std::runtime_error("watchdog_cycles must be non-zero");
    if (config.workload == "W1" && config.n_tiles != 1 && config.n_tiles != 8)
        throw std::runtime_error("G1 W1 requires n_tiles 1 or 8");

    return config;
}

bool Phase1Driver::directIdle(const MultiChannelMemorySystem& memory)
{
    if (memory.hasPendingTransactions() != 0)
        return false;
    for (const MemorySystem* channel : memory.channels)
    {
        if (channel == nullptr || !channel->pendingTransactions.empty())
            return false;
        if (channel->memoryController == nullptr ||
            !channel->memoryController->transactionQueue.empty())
            return false;
        if (channel->ranks == nullptr)
            return false;
        for (const Rank* rank : *channel->ranks)
        {
            if (rank == nullptr || rank->outgoingDataPacket != nullptr ||
                !rank->readReturnPacket.empty() || !rank->readReturnCountdown.empty())
                return false;
        }
    }
    return true;
}

Phase1RunStats Phase1Driver::runDirect(MultiChannelMemorySystem& memory, uint64_t watchdogCycles)
{
    Phase1RunStats stats;
    while (!directIdle(memory))
    {
        if (stats.updates >= watchdogCycles)
        {
            stats.timeout = true;
            break;
        }
        memory.update();
        stats.updates++;
        stats.cycles = stats.updates;
    }
    for (const MemorySystem* channel : memory.channels)
        if (channel != nullptr)
            stats.pending_at_end += channel->pendingTransactions.size();
    return stats;
}

Phase1RunStats Phase1Driver::runFeeder(Phase1FeederMemory& memory, uint64_t watchdogCycles)
{
    Phase1RunStats stats;
    stats.feeder_used = true;
    while (!memory.isIdle())
    {
        if (stats.updates >= watchdogCycles)
        {
            stats.timeout = true;
            break;
        }
        memory.tick();
        memory.update();
        stats.updates++;
        stats.cycles = stats.updates;
    }
    stats.pending_at_end = memory.pendingCount();
    stats.outstanding_at_end = memory.outstandingCount();
    stats.max_external_pending = memory.maxPending();
    stats.max_accepted_outstanding = memory.maxOutstanding();
    stats.controller_rejections = memory.controllerRejections();
    stats.barriers_retired = memory.barriersRetired();
    return stats;
}

}  // namespace DRAMSim

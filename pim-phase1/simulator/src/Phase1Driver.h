#ifndef PHASE1_DRIVER_H
#define PHASE1_DRIVER_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>

#include "MultiChannelMemorySystem.h"
#include "Phase1Feeder.h"

namespace DRAMSim
{
struct Phase1RunConfig
{
    std::string experiment;
    std::string workload;
    std::string mode;
    std::string trace_mode;
    std::string device_ini;
    std::string system_ini;
    unsigned channels = 0;
    unsigned ranks = 0;
    unsigned capacity_mb = 0;
    unsigned n_tiles = 0;
    unsigned seed = 0;
    unsigned feeder_outstanding_limit = 8;
    unsigned feeder_issue_per_cycle = 1;
    uint64_t watchdog_cycles = 100000;
};

struct Phase1RunStats
{
    uint64_t cycles = 0;
    uint64_t updates = 0;
    bool timeout = false;
    bool feeder_used = false;
    std::size_t pending_at_end = 0;
    std::size_t outstanding_at_end = 0;
    std::size_t max_external_pending = 0;
    std::size_t max_accepted_outstanding = 0;
    uint64_t controller_rejections = 0;
    uint64_t barriers_retired = 0;
};

// Parse the flat Phase1 params.ini.  Unknown keys are rejected so a typo
// cannot silently select a different experiment.
Phase1RunConfig loadPhase1Config(const std::string& path);

class Phase1Driver
{
  public:
    static Phase1RunStats runDirect(MultiChannelMemorySystem& memory, uint64_t watchdogCycles);
    static Phase1RunStats runFeeder(Phase1FeederMemory& memory, uint64_t watchdogCycles);
    static bool directIdle(const MultiChannelMemorySystem& memory);
};
}  // namespace DRAMSim

#endif

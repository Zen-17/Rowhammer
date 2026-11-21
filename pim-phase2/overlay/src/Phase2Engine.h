#ifndef PHASE2_ENGINE_H
#define PHASE2_ENGINE_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "Phase1Address.h"
#include "Phase1Reports.h"
#include "Phase2Checkpoint.h"
#include "Phase2Gate.h"
#include "Phase2Ledger.h"
#include "Phase2Types.h"

namespace DRAMSim
{
namespace phase2
{
class RecoveryEngine
{
  public:
    Decision analyze(const Phase1PublicReport& report, uint64_t t_ack,
                     const EngineConfig& config, const Ledger& ledger,
                     const Gate& gate, const CheckpointView& checkpoint,
                     bool report_overflow, const Phase1Address& address) const;

    Decision planWholeJob(const Ledger& ledger, const CheckpointView& checkpoint,
                          const std::string& reason = "B0_BASELINE",
                          int64_t job_id = -1) const;

    ExecutionPlan planExecution(const Decision& decision, const Ledger& ledger,
                                const EngineConfig* config = nullptr,
                                const Gate* gate = nullptr) const;

    std::vector<DataRef> affectedOutputs(const Decision& decision,
                                         const Ledger& ledger) const;

    uint64_t ancestorCompleteTime(const Ledger& ledger, uint64_t op_id,
                                  std::map<uint64_t, uint64_t>& memo) const;

    std::map<DataRef, uint64_t> globalHoldSchedule(const Ledger& ledger,
                                                   uint64_t hold_cycles) const;
    std::map<DataRef, uint64_t> depReleaseSchedule(const Ledger& ledger,
                                                   uint64_t delta_cycles) const;

  private:
    static bool readsAny(const OpEntry& op, const std::set<DataRef>& suspects,
                         uint64_t window_start, uint64_t window_end);
    static uint64_t birthCycle(const Ledger& ledger, const DataRef& ref);
    static uint64_t itemWindowStart(const Ledger& ledger, const DataRef& ref,
                                    const Phase1PublicReport& report,
                                    const EngineConfig& config);
    static bool readInWindow(const Ledger& ledger, const DataRef& ref,
                             uint64_t window_start, uint64_t t_ack);
    static bool readsAnyPerItem(const OpEntry& op,
                                const std::map<DataRef, uint64_t>& windows,
                                uint64_t t_ack);
    static std::vector<Run> groupRuns(std::vector<const OpEntry*> entries);
    static std::vector<Run> coalesceRuns(const std::vector<Run>& runs,
                                         const Decision& decision,
                                         const Ledger& ledger,
                                         const EngineConfig& config,
                                         const Gate& gate,
                                         uint64_t& coalesced_extra);
};

}  // namespace phase2
}  // namespace DRAMSim

#endif

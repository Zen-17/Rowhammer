#ifndef PHASE1_RUNTIME_H
#define PHASE1_RUNTIME_H

#include <cstdint>
#include <string>

#include "Phase1Reports.h"

namespace DRAMSim
{
enum class OutputState
{
    ALLOCATED,
    PRODUCING,
    READY_UNPUBLISHED,
    PUBLISHED,
    SUSPECT,
    QUIESCED,
    RESTORING,
    REEXECUTING,
    POISONED,
    ABORTED
};

enum class ConsumerType
{
    HOST,
    PIM
};

struct OutputHandle
{
    uint64_t version = 0;
    bool valid = false;
};

const char* outputStateName(OutputState state);

// G5 output-version state machine and consumer admission gate.  It has no
// reference to fault schedules, golden output or storage internals.  Pause,
// quiesce and restore transitions are deliberately not implemented here.
class Phase1Runtime
{
  public:
    explicit Phase1Runtime(uint64_t holdCycles = 0);

    OutputHandle allocateOutput(uint64_t bytes, uint64_t cycle);
    void beginProducing(OutputHandle handle, uint64_t cycle);
    void producerComplete(OutputHandle handle, uint64_t cycle);
    void quiesceOutput(OutputHandle handle, uint64_t cycle);
    void beginRestore(OutputHandle handle, uint64_t cycle);
    void beginReexecute(OutputHandle handle, uint64_t cycle);
    void onReport(const Phase1PublicReport& report, uint64_t cycle);
    void tick(uint64_t cycle);

    bool canConsume(OutputHandle handle, ConsumerType consumer, uint64_t cycle) const;
    bool canAdmit(OutputHandle handle, ConsumerType consumer, uint64_t cycle) const;
    bool consume(OutputHandle handle, ConsumerType consumer, uint64_t cycle);

    // Fail-fast guard for test/evaluator bypass paths.  Normal host/PIM paths
    // call canConsume/canAdmit before entering the storage model.
    void assertConsumerAllowed(OutputHandle handle, ConsumerType consumer, uint64_t cycle) const;

    OutputState state(OutputHandle handle) const;
    bool isStale(OutputHandle handle) const;
    bool isPublished(OutputHandle handle) const;

    uint64_t currentVersion() const { return currentVersion_; }
    uint64_t blockedConsumerReads() const { return blocked_consumer_reads_; }
    uint64_t lateReports() const { return late_reports_; }
    uint64_t lockCount() const { return lock_count_; }
    uint64_t consumedReads() const { return consumed_reads_; }
    uint64_t publishCycle() const { return publish_cycle_; }
    uint64_t lockCycle() const { return lock_cycle_; }
    uint64_t producerDoneCycle() const { return producer_done_cycle_; }
    uint64_t publishReadyCycle() const { return publish_ready_cycle_; }

  private:
    struct Version
    {
        uint64_t version = 0;
        uint64_t bytes = 0;
        OutputState state = OutputState::ALLOCATED;
        uint64_t allocated_cycle = 0;
        uint64_t producer_done_cycle = 0;
        uint64_t publish_ready_cycle = 0;
        uint64_t publish_cycle = 0;
        uint64_t lock_cycle = 0;
        uint64_t consumed = 0;
    };

    Version& currentVersionRecord();
    const Version& currentVersionRecord() const;
    bool handleCurrent(OutputHandle handle) const;

    uint64_t hold_cycles_ = 0;
    uint64_t currentVersion_ = 0;
    Version current_;
    uint64_t blocked_consumer_reads_ = 0;
    uint64_t late_reports_ = 0;
    uint64_t lock_count_ = 0;
    uint64_t consumed_reads_ = 0;
    uint64_t publish_cycle_ = 0;
    uint64_t lock_cycle_ = 0;
    uint64_t producer_done_cycle_ = 0;
    uint64_t publish_ready_cycle_ = 0;
};
}  // namespace DRAMSim

#endif

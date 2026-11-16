#include "Phase1Runtime.h"

#include <stdexcept>

namespace DRAMSim
{
const char* outputStateName(OutputState state)
{
    switch (state)
    {
        case OutputState::ALLOCATED: return "ALLOCATED";
        case OutputState::PRODUCING: return "PRODUCING";
        case OutputState::READY_UNPUBLISHED: return "READY_UNPUBLISHED";
        case OutputState::PUBLISHED: return "PUBLISHED";
        case OutputState::SUSPECT: return "SUSPECT";
        case OutputState::QUIESCED: return "QUIESCED";
        case OutputState::RESTORING: return "RESTORING";
        case OutputState::REEXECUTING: return "REEXECUTING";
        case OutputState::POISONED: return "POISONED";
        case OutputState::ABORTED: return "ABORTED";
    }
    return "UNKNOWN";
}

Phase1Runtime::Phase1Runtime(uint64_t holdCycles) : hold_cycles_(holdCycles) {}

Phase1Runtime::Version& Phase1Runtime::currentVersionRecord()
{
    return current_;
}

const Phase1Runtime::Version& Phase1Runtime::currentVersionRecord() const
{
    return current_;
}

bool Phase1Runtime::handleCurrent(OutputHandle handle) const
{
    return handle.valid && currentVersion_ != 0 && handle.version == currentVersion_;
}

OutputHandle Phase1Runtime::allocateOutput(uint64_t bytes, uint64_t cycle)
{
    current_ = Version();
    current_.version = ++currentVersion_;
    current_.bytes = bytes;
    current_.state = OutputState::ALLOCATED;
    current_.allocated_cycle = cycle;
    publish_cycle_ = 0;
    lock_cycle_ = 0;
    producer_done_cycle_ = 0;
    publish_ready_cycle_ = 0;
    OutputHandle handle;
    handle.version = currentVersion_;
    handle.valid = true;
    return handle;
}

void Phase1Runtime::beginProducing(OutputHandle handle, uint64_t cycle)
{
    if (!handleCurrent(handle))
        throw std::logic_error("STALE_HANDLE");
    (void)cycle;
    if (current_.state != OutputState::ALLOCATED)
        throw std::logic_error("output version is not ALLOCATED");
    current_.state = OutputState::PRODUCING;
}

void Phase1Runtime::producerComplete(OutputHandle handle, uint64_t cycle)
{
    if (!handleCurrent(handle))
        throw std::logic_error("STALE_HANDLE");
    if (current_.state != OutputState::PRODUCING &&
        current_.state != OutputState::READY_UNPUBLISHED &&
        current_.state != OutputState::SUSPECT &&
        current_.state != OutputState::POISONED &&
        current_.state != OutputState::REEXECUTING)
        throw std::logic_error("producer completion in invalid output state");
    current_.producer_done_cycle = cycle;
    current_.publish_ready_cycle = cycle + hold_cycles_;
    producer_done_cycle_ = cycle;
    publish_ready_cycle_ = current_.publish_ready_cycle;
    if (current_.state == OutputState::PRODUCING ||
        current_.state == OutputState::REEXECUTING)
        current_.state = OutputState::READY_UNPUBLISHED;
}

void Phase1Runtime::quiesceOutput(OutputHandle handle, uint64_t cycle)
{
    if (!handleCurrent(handle))
        throw std::logic_error("STALE_HANDLE");
    if (current_.state != OutputState::PRODUCING &&
        current_.state != OutputState::SUSPECT &&
        current_.state != OutputState::READY_UNPUBLISHED)
        throw std::logic_error("output version is not quiescible");
    (void)cycle;
    current_.state = OutputState::QUIESCED;
}

void Phase1Runtime::beginRestore(OutputHandle handle, uint64_t cycle)
{
    if (!handleCurrent(handle))
        throw std::logic_error("STALE_HANDLE");
    if (current_.state == OutputState::PUBLISHED)
        throw std::logic_error("ALREADY_COMMITTED");
    if (current_.state != OutputState::QUIESCED)
        throw std::logic_error("output version is not QUIESCED");
    (void)cycle;
    current_.state = OutputState::RESTORING;
}

void Phase1Runtime::beginReexecute(OutputHandle handle, uint64_t cycle)
{
    if (!handleCurrent(handle))
        throw std::logic_error("STALE_HANDLE");
    if (current_.state != OutputState::ALLOCATED &&
        current_.state != OutputState::RESTORING)
        throw std::logic_error("output version is not ready for re-execution");
    (void)cycle;
    current_.state = OutputState::REEXECUTING;
}

void Phase1Runtime::onReport(const Phase1PublicReport& report, uint64_t cycle)
{
    if (currentVersion_ == 0 || current_.state == OutputState::ABORTED)
        return;
    if (report.error_class != "CE" && report.error_class != "UE")
        return;
    if (current_.state == OutputState::PUBLISHED)
    {
        late_reports_++;
        return;
    }
    if (current_.state == OutputState::POISONED)
        return;
    if (current_.state == OutputState::SUSPECT && report.error_class != "UE")
        return;
    if (report.error_class == "UE")
    {
        current_.state = OutputState::POISONED;
    }
    else
    {
        current_.state = OutputState::SUSPECT;
    }
    lock_cycle_ = cycle;
    current_.lock_cycle = cycle;
    lock_count_++;
}

void Phase1Runtime::tick(uint64_t cycle)
{
    if (currentVersion_ == 0)
        return;
    if (current_.state != OutputState::READY_UNPUBLISHED)
        return;
    if (cycle < current_.publish_ready_cycle)
        return;
    current_.state = OutputState::PUBLISHED;
    current_.publish_cycle = cycle;
    publish_cycle_ = cycle;
}

bool Phase1Runtime::canConsume(OutputHandle handle, ConsumerType consumer, uint64_t cycle) const
{
    (void)consumer;
    (void)cycle;
    return handleCurrent(handle) && current_.state == OutputState::PUBLISHED;
}

bool Phase1Runtime::canAdmit(OutputHandle handle, ConsumerType consumer, uint64_t cycle) const
{
    return canConsume(handle, consumer, cycle);
}

bool Phase1Runtime::consume(OutputHandle handle, ConsumerType consumer, uint64_t cycle)
{
    if (!canConsume(handle, consumer, cycle))
    {
        blocked_consumer_reads_++;
        return false;
    }
    current_.consumed++;
    consumed_reads_++;
    return true;
}

void Phase1Runtime::assertConsumerAllowed(OutputHandle handle, ConsumerType consumer,
                                          uint64_t cycle) const
{
    if (!handleCurrent(handle))
        throw std::logic_error("STALE_HANDLE");
    if (!canConsume(handle, consumer, cycle))
        throw std::logic_error(std::string("UNPUBLISHED_OUTPUT:") +
                               outputStateName(current_.state));
}

OutputState Phase1Runtime::state(OutputHandle handle) const
{
    if (!handleCurrent(handle))
        return OutputState::ABORTED;
    return current_.state;
}

bool Phase1Runtime::isStale(OutputHandle handle) const
{
    return !handleCurrent(handle);
}

bool Phase1Runtime::isPublished(OutputHandle handle) const
{
    return handleCurrent(handle) && current_.state == OutputState::PUBLISHED;
}

}  // namespace DRAMSim

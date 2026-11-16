#include "Phase1Pause.h"

#include <stdexcept>

namespace DRAMSim
{
Phase1PauseController::Phase1PauseController(Phase1FeederMemory& feeder, unsigned channel,
                                             uint64_t handlerDelay)
    : feeder_(feeder), channel_(channel), handler_delay_(handlerDelay)
{
    if (channel_ >= feeder_.channels.size())
        throw std::out_of_range("pause channel out of range");
}

void Phase1PauseController::request(uint64_t cycle, const std::string& reason)
{
    if (requested_)
        return;
    requested_ = true;
    request_cycle_ = cycle;
    reason_ = reason;
    feeder_.freezeChannel(channel_);
}

void Phase1PauseController::notifyReport(uint64_t reportCycle)
{
    if (requested_ || scheduled_)
        return;
    scheduled_ = true;
    scheduled_cycle_ = reportCycle + handler_delay_;
    reason_ = "REPORT_TRIGGERED";
}

void Phase1PauseController::tick(uint64_t cycle)
{
    if (scheduled_ && !requested_ && cycle >= scheduled_cycle_)
    {
        scheduled_ = false;
        request(cycle, reason_);
    }
    if (requested_ && !acked_ && feeder_.channelIdle(channel_))
    {
        acked_ = true;
        ack_cycle_ = cycle;
    }
}
}  // namespace DRAMSim

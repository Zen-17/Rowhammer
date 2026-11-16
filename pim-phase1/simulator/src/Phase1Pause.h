#ifndef PHASE1_PAUSE_H
#define PHASE1_PAUSE_H

#include <cstdint>
#include <string>

#include "Phase1Feeder.h"

namespace DRAMSim
{
// G6 channel quiesce controller.  It stops new feeder submissions for the
// target channel, waits for accepted transactions to drain, and reports ACK.
// It deliberately does not stop the DRAM clock, refresh, or other channels.
class Phase1PauseController
{
  public:
    Phase1PauseController(Phase1FeederMemory& feeder, unsigned channel,
                          uint64_t handlerDelay = 0);

    void request(uint64_t cycle, const std::string& reason = "SYNTHETIC_CONTROL");
    void notifyReport(uint64_t reportCycle);
    void tick(uint64_t cycle);

    bool requested() const { return requested_; }
    bool acked() const { return acked_; }
    bool scheduled() const { return scheduled_; }
    unsigned channel() const { return channel_; }
    const std::string& reason() const { return reason_; }
    uint64_t requestCycle() const { return request_cycle_; }
    uint64_t ackCycle() const { return ack_cycle_; }
    uint64_t quiesceCycles() const { return ack_cycle_ - request_cycle_; }
    uint64_t scheduledCycle() const { return scheduled_cycle_; }

  private:
    Phase1FeederMemory& feeder_;
    unsigned channel_ = 0;
    uint64_t handler_delay_ = 0;
    bool requested_ = false;
    bool acked_ = false;
    bool scheduled_ = false;
    std::string reason_;
    uint64_t scheduled_cycle_ = 0;
    uint64_t request_cycle_ = 0;
    uint64_t ack_cycle_ = 0;
};
}  // namespace DRAMSim

#endif

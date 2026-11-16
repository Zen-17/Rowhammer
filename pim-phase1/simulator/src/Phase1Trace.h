#ifndef PHASE1_TRACE_H
#define PHASE1_TRACE_H

#include <cstdint>
#include <fstream>
#include <string>

namespace DRAMSim
{
// Passive event writer for the Phase1 data protocol.  It never changes memory
// state and is not consulted by the driver/runtime.  A disabled trace still
// creates a header-only events.csv so every run has the same schema.
class Phase1Trace
{
  public:
    Phase1Trace(const std::string& path, bool enabled);
    ~Phase1Trace();

    bool enabled() const { return enabled_; }
    uint64_t eventCount() const { return sequence_; }

    void event(uint64_t cycle, const std::string& phase, const std::string& eventType,
               const std::string& kernelId, const std::string& versionId,
               const std::string& accessId, int channel, int rank, int actualBank, int row,
               int column, const std::string& accessPath, int bytes, const std::string& status);

  private:
    std::ofstream output_;
    bool enabled_ = false;
    uint64_t sequence_ = 0;
};
}  // namespace DRAMSim

#endif

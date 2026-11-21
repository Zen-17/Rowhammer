#ifndef PHASE2_GATE_H
#define PHASE2_GATE_H

#include <cstdint>
#include <map>
#include <vector>

#include "Phase2Types.h"

namespace DRAMSim
{
namespace phase2
{
class Gate
{
  public:
    Lifecycle state(const DataRef& ref) const;
    void hold(const DataRef& ref);
    void publish(const DataRef& ref, uint64_t cycle);
    bool consume(const DataRef& ref, uint64_t cycle);
    void markSuspect(const std::vector<DataRef>& refs);
    void poison(const std::vector<DataRef>& refs);
    std::vector<DataRef> consumedRefs() const;
    void freeze() { frozen_ = true; }
    void unfreeze() { frozen_ = false; }
    bool frozen() const { return frozen_; }
    void releaseDue(uint64_t cycle, const std::map<DataRef, uint64_t>& schedule);

  private:
    std::map<DataRef, Lifecycle> states_;
    std::map<DataRef, uint64_t> publish_cycles_;
    bool frozen_ = false;
};

}  // namespace phase2
}  // namespace DRAMSim

#endif

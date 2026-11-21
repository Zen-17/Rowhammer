#ifndef PHASE2_ISOLATION_H
#define PHASE2_ISOLATION_H

#include <cstdint>
#include <set>
#include <vector>

#include "Phase1Address.h"
#include "Phase2Layout.h"
#include "Phase2Types.h"

namespace DRAMSim
{
namespace phase2
{
class SourceIsolationPolicy
{
  public:
    virtual ~SourceIsolationPolicy() = default;
    virtual uint64_t isolate(const std::vector<BurstKey>& footprint) = 0;
    virtual bool isIsolated(const BurstKey& key) const = 0;
};

// v1: logical marking only.  No remap; bytes = bursts x 32.
class SimpleRegionIsolationPolicy : public SourceIsolationPolicy
{
  public:
    uint64_t isolate(const std::vector<BurstKey>& footprint) override;
    uint64_t isolate(const ReportFootprint& footprint, const Phase1Address& address);
    bool isIsolated(const BurstKey& key) const override;
    uint64_t isolatedBytes() const { return isolated_bytes_; }

  private:
    std::set<BurstKey> keys_;
    std::set<std::pair<std::pair<unsigned, unsigned>, unsigned>> banks_;
    uint64_t isolated_bytes_ = 0;
};

}  // namespace phase2
}  // namespace DRAMSim

#endif

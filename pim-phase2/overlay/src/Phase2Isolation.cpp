#include "Phase2Isolation.h"

namespace DRAMSim
{
namespace phase2
{
uint64_t SimpleRegionIsolationPolicy::isolate(const std::vector<BurstKey>& footprint)
{
    for (const BurstKey& key : footprint)
        keys_.insert(key);
    const uint64_t bytes = static_cast<uint64_t>(footprint.size()) * 32ULL;
    isolated_bytes_ += bytes;
    return bytes;
}

uint64_t SimpleRegionIsolationPolicy::isolate(const ReportFootprint& footprint,
                                              const Phase1Address& address)
{
    if (footprint.kind == ReportFootprint::Kind::BANK)
    {
        banks_.insert(std::make_pair(std::make_pair(footprint.channel, footprint.rank),
                                     footprint.bank));
        const uint64_t bytes = static_cast<uint64_t>(address.geometry().num_rows) *
                               address.geometry().burst_columns() * 32ULL;
        isolated_bytes_ += bytes;
        return bytes;
    }
    return isolate(footprint.keys);
}

bool SimpleRegionIsolationPolicy::isIsolated(const BurstKey& key) const
{
    if (keys_.find(key) != keys_.end())
        return true;
    return banks_.find(std::make_pair(std::make_pair(key.channel, key.rank), key.bank)) !=
           banks_.end();
}

}  // namespace phase2
}  // namespace DRAMSim

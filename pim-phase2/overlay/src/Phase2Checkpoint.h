#ifndef PHASE2_CHECKPOINT_H
#define PHASE2_CHECKPOINT_H

#include <cstdint>
#include <map>
#include <set>
#include <utility>
#include <vector>

#include "Burst.h"

namespace DRAMSim
{
namespace phase2
{
// Engine-visible availability mask: which INITIAL (operand, tile) copies exist.
class CheckpointView
{
  public:
    CheckpointView() = default;
    explicit CheckpointView(bool complete) : complete_(complete) {}

    void setComplete(bool complete) { complete_ = complete; }
    void setAbsent(uint16_t operand, uint32_t tile)
    {
        absent_.insert(std::make_pair(operand, tile));
    }
    bool has(uint16_t operand, uint32_t tile) const;

  private:
    bool complete_ = true;
    std::set<std::pair<uint16_t, uint32_t>> absent_;
};

// Trusted host copies of INITIAL input tiles (128 bursts = 4 KiB per tile).
class Phase2CheckpointStore
{
  public:
    void addTile(uint16_t operand, uint32_t tile, const std::vector<BurstType>& bursts)
    {
        tiles_[std::make_pair(operand, tile)] = bursts;
    }
    bool has(uint16_t operand, uint32_t tile) const
    {
        return tiles_.find(std::make_pair(operand, tile)) != tiles_.end();
    }
    const std::vector<BurstType>* tile(uint16_t operand, uint32_t tile) const
    {
        auto found = tiles_.find(std::make_pair(operand, tile));
        return found == tiles_.end() ? nullptr : &found->second;
    }
    uint64_t bytes() const
    {
        uint64_t total = 0;
        for (const auto& entry : tiles_)
            total += entry.second.size() * sizeof(BurstType);
        return total;
    }
    std::size_t tileCount() const { return tiles_.size(); }

  private:
    std::map<std::pair<uint16_t, uint32_t>, std::vector<BurstType>> tiles_;
};

}  // namespace phase2
}  // namespace DRAMSim

#endif

#include "Phase2Checkpoint.h"

namespace DRAMSim
{
namespace phase2
{
bool CheckpointView::has(uint16_t operand, uint32_t tile) const
{
    if (!complete_)
        return false;
    return absent_.find(std::make_pair(operand, tile)) == absent_.end();
}

}  // namespace phase2
}  // namespace DRAMSim

#ifndef PHASE1_CHECKPOINT_H
#define PHASE1_CHECKPOINT_H

#include <cstdint>
#include <vector>

#include "Burst.h"

namespace DRAMSim
{
enum class RecoveryStatus
{
    OK,
    CHECKPOINT_MISSING,
    VERSION_MISMATCH,
    CHECKPOINT_CORRUPT,
    ALREADY_COMMITTED,
    STALE_HANDLE,
    RESTORE_FAILED
};

const char* recoveryStatusName(RecoveryStatus status);

// Trusted-host checkpoint of the immutable W1 inputs.  The checkpoint is a
// model-side protected host area; it is not a reference-output file and the
// runtime does not read it.
class Phase1CheckpointStore
{
  public:
    bool create(uint64_t version, const std::vector<BurstType>& inputA,
                const std::vector<BurstType>& inputX);

    RecoveryStatus validate(uint64_t expectedVersion) const;
    uint64_t version() const { return version_; }
    bool present() const { return present_; }
    uint64_t checksum() const { return checksum_; }
    std::size_t bytes() const;
    const std::vector<BurstType>& inputA() const { return inputA_; }
    const std::vector<BurstType>& inputX() const { return inputX_; }

    // Test-only corruption of the stored copy.  This does not update the
    // checksum, so validate() must report CHECKPOINT_CORRUPT.
    void corruptForTest();

  private:
    bool present_ = false;
    uint64_t version_ = 0;
    std::vector<BurstType> inputA_;
    std::vector<BurstType> inputX_;
    uint64_t checksum_ = 0;
};
}  // namespace DRAMSim

#endif

#include "Phase1Checkpoint.h"

#include <cstring>

namespace DRAMSim
{
const char* recoveryStatusName(RecoveryStatus status)
{
    switch (status)
    {
        case RecoveryStatus::OK: return "OK";
        case RecoveryStatus::CHECKPOINT_MISSING: return "CHECKPOINT_MISSING";
        case RecoveryStatus::VERSION_MISMATCH: return "VERSION_MISMATCH";
        case RecoveryStatus::CHECKPOINT_CORRUPT: return "CHECKPOINT_CORRUPT";
        case RecoveryStatus::ALREADY_COMMITTED: return "ALREADY_COMMITTED";
        case RecoveryStatus::STALE_HANDLE: return "STALE_HANDLE";
        case RecoveryStatus::RESTORE_FAILED: return "RESTORE_FAILED";
    }
    return "UNKNOWN";
}

namespace
{
uint64_t fnv1a(const std::vector<BurstType>& a, const std::vector<BurstType>& x)
{
    uint64_t hash = 1469598103934665603ULL;
    auto feed = [&](const std::vector<BurstType>& values) {
        for (const BurstType& value : values)
        {
            for (unsigned i = 0; i < sizeof(value.u8Data_); i++)
            {
                hash ^= value.u8Data_[i];
                hash *= 1099511628211ULL;
            }
        }
    };
    feed(a);
    feed(x);
    return hash;
}
}  // namespace

bool Phase1CheckpointStore::create(uint64_t version, const std::vector<BurstType>& inputA,
                                   const std::vector<BurstType>& inputX)
{
    if (inputA.empty() || inputX.empty())
        return false;
    inputA_ = inputA;
    inputX_ = inputX;
    version_ = version;
    checksum_ = fnv1a(inputA_, inputX_);
    present_ = true;
    return true;
}

RecoveryStatus Phase1CheckpointStore::validate(uint64_t expectedVersion) const
{
    if (!present_)
        return RecoveryStatus::CHECKPOINT_MISSING;
    if (version_ != expectedVersion)
        return RecoveryStatus::VERSION_MISMATCH;
    if (checksum_ != fnv1a(inputA_, inputX_))
        return RecoveryStatus::CHECKPOINT_CORRUPT;
    return RecoveryStatus::OK;
}

std::size_t Phase1CheckpointStore::bytes() const
{
    return (inputA_.size() + inputX_.size()) * sizeof(BurstType);
}

void Phase1CheckpointStore::corruptForTest()
{
    if (!inputA_.empty())
        inputA_[0].u8Data_[0] ^= 1;
}
}  // namespace DRAMSim

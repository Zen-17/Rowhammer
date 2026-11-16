#ifndef PHASE1_PROFILE_H
#define PHASE1_PROFILE_H

namespace DRAMSim
{
enum class Phase1Profile
{
    NATIVE,
    LOCAL_VISIBLE,
    LOCAL_HIDDEN,
    HOST_ONLY
};

enum class Phase1AccessType
{
    HOST,
    PIM_OPERAND,
    PIM_RESULT,
    HAB
};

enum class Phase1PacketStatus
{
    OK = 0,
    CORRECTED = 1,
    UNPROTECTED_FORMAT = 2,
    UNCORRECTABLE = 3
};
}  // namespace DRAMSim

#endif

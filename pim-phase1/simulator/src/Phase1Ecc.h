#ifndef PHASE1_ECC_H
#define PHASE1_ECC_H

#include <array>
#include <cstdint>

namespace DRAMSim
{
enum class Phase1EccStatus
{
    CLEAN,
    CORRECTED,
    UNCORRECTABLE,
    INVALID,
    UNPROTECTED_FORMAT
};

struct Phase1EccResult
{
    uint64_t data = 0;
    Phase1EccStatus status = Phase1EccStatus::CLEAN;
    unsigned syndrome = 0;
    unsigned overall_parity = 0;
};

// Explicit extended Hamming SECDED(72,64) research model.
//
// Codeword positions are 1..72:
//   1,2,4,8,16,32,64 -> Hamming parity
//   other 1..71      -> data bits 0..63 in ascending position order
//   72               -> overall even parity over positions 1..71
//
// The 8-bit parity byte uses bit 0..6 for the Hamming parity positions
// 1,2,4,8,16,32,64 and bit 7 for the position-72 overall parity bit.  This
// layout is a Phase1 model choice, not a claim about a commercial HBM ECC
// codeword layout.
class Phase1Ecc
{
  public:
    static uint8_t encodeParity(uint64_t data);
    static Phase1EccResult decode(uint64_t data, uint8_t parity);

    static const std::array<unsigned, 7>& hammingParityPositions();
    static const std::array<unsigned, 64>& dataBitPositions();
};
}  // namespace DRAMSim

#endif

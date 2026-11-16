#include "Phase1Ecc.h"

namespace DRAMSim
{
namespace
{
constexpr unsigned kDataBits = 64;
constexpr unsigned kCodewordBits = 72;

int bitAt(const std::array<unsigned, kCodewordBits>& bits, unsigned position)
{
    return (position >= 1 && position <= kCodewordBits) ? static_cast<int>(bits[position - 1]) : 0;
}

void putDataBits(std::array<unsigned, kCodewordBits>& bits, uint64_t data)
{
    const auto& positions = Phase1Ecc::dataBitPositions();
    for (unsigned i = 0; i < kDataBits; i++)
        bits[positions[i] - 1] = static_cast<unsigned>((data >> i) & 1u);
}

void putParityBits(std::array<unsigned, kCodewordBits>& bits, uint8_t parity)
{
    const auto& positions = Phase1Ecc::hammingParityPositions();
    for (unsigned i = 0; i < positions.size(); i++)
        bits[positions[i] - 1] = (parity >> i) & 1u;
    bits[71] = (parity >> 7) & 1u;
}

uint64_t extractData(const std::array<unsigned, kCodewordBits>& bits)
{
    const auto& positions = Phase1Ecc::dataBitPositions();
    uint64_t data = 0;
    for (unsigned i = 0; i < kDataBits; i++)
        data |= static_cast<uint64_t>(bits[positions[i] - 1]) << i;
    return data;
}
}  // namespace

const std::array<unsigned, 7>& Phase1Ecc::hammingParityPositions()
{
    static const std::array<unsigned, 7> positions = {1, 2, 4, 8, 16, 32, 64};
    return positions;
}

const std::array<unsigned, 64>& Phase1Ecc::dataBitPositions()
{
    static const std::array<unsigned, 64> positions = [] {
        std::array<unsigned, 64> result{};
        unsigned next = 0;
        for (unsigned position = 1; position <= 71; position++)
        {
            if (position == 1 || position == 2 || position == 4 || position == 8 || position == 16 ||
                position == 32 || position == 64)
                continue;
            result[next++] = position;
        }
        return result;
    }();
    return positions;
}

uint8_t Phase1Ecc::encodeParity(uint64_t data)
{
    std::array<unsigned, kCodewordBits> bits{};
    putDataBits(bits, data);
    const auto& parityPositions = hammingParityPositions();
    uint8_t parity = 0;
    for (unsigned i = 0; i < parityPositions.size(); i++)
    {
        const unsigned p = parityPositions[i];
        unsigned value = 0;
        for (unsigned position = 1; position <= 71; position++)
        {
            if ((position & p) != 0)
                value ^= bits[position - 1];
        }
        bits[p - 1] = value;
        parity |= static_cast<uint8_t>(value << i);
    }
    unsigned overall = 0;
    for (unsigned position = 1; position <= 71; position++)
        overall ^= bits[position - 1];
    bits[71] = overall;
    parity |= static_cast<uint8_t>(overall << 7);
    return parity;
}

Phase1EccResult Phase1Ecc::decode(uint64_t data, uint8_t parity)
{
    std::array<unsigned, kCodewordBits> bits{};
    putDataBits(bits, data);
    putParityBits(bits, parity);

    unsigned syndrome = 0;
    for (unsigned position = 1; position <= 71; position++)
        if (bitAt(bits, position) != 0)
            syndrome ^= position;
    unsigned overall = 0;
    for (unsigned position = 1; position <= 72; position++)
        overall ^= static_cast<unsigned>(bitAt(bits, position));

    Phase1EccResult result;
    result.data = data;
    result.syndrome = syndrome;
    result.overall_parity = overall;

    if (syndrome == 0 && overall == 0)
    {
        result.status = Phase1EccStatus::CLEAN;
        return result;
    }
    if (syndrome == 0 && overall == 1)
    {
        // Only the position-72 overall parity bit is wrong.
        result.status = Phase1EccStatus::CORRECTED;
        return result;
    }
    if (syndrome != 0 && overall == 1 && syndrome <= 71)
    {
        // Correct the single position indicated by the syndrome.  If the
        // position is a Hamming parity bit, the data bits are unchanged.
        bits[syndrome - 1] ^= 1u;
        result.data = extractData(bits);
        result.status = Phase1EccStatus::CORRECTED;
        return result;
    }
    if (syndrome > 71 && overall == 1)
    {
        result.status = Phase1EccStatus::INVALID;
        return result;
    }
    result.status = Phase1EccStatus::UNCORRECTABLE;
    return result;
}

}  // namespace DRAMSim

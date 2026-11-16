#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "Phase1Ecc.h"
#include "Phase1Storage.h"

using namespace DRAMSim;

namespace
{
struct Pattern
{
    std::string name;
    uint64_t data;
};

const std::vector<Pattern>& patterns()
{
    static const std::vector<Pattern> values = {
        {"zeros", 0x0000000000000000ULL},
        {"ones", 0xFFFFFFFFFFFFFFFFULL},
        {"alt01", 0x5555555555555555ULL},
        {"alt10", 0xAAAAAAAAAAAAAAAAULL},
        {"rand0", 0x0123456789ABCDEFULL},
        {"rand1", 0xFEDCBA9876543210ULL},
        {"rand2", 0x0F1E2D3C4B5A6978ULL},
        {"rand3", 0x8877665544332211ULL},
    };
    return values;
}

std::string hex64(uint64_t value)
{
    std::ostringstream os;
    os << std::hex << std::setfill('0') << std::setw(16) << value;
    return os.str();
}

std::string hex8(uint8_t value)
{
    std::ostringstream os;
    os << std::hex << std::setfill('0') << std::setw(2) << static_cast<unsigned>(value);
    return os.str();
}

std::string statusName(Phase1EccStatus status)
{
    switch (status)
    {
        case Phase1EccStatus::CLEAN:
            return "CLEAN";
        case Phase1EccStatus::CORRECTED:
            return "CORRECTED";
        case Phase1EccStatus::UNCORRECTABLE:
            return "UNCORRECTABLE";
        case Phase1EccStatus::INVALID:
            return "INVALID";
        case Phase1EccStatus::UNPROTECTED_FORMAT:
            return "UNPROTECTED_FORMAT";
    }
    return "UNKNOWN";
}

std::array<unsigned, 72> codewordFrom(uint64_t data, uint8_t parity)
{
    std::array<unsigned, 72> bits{};
    const auto& dataPositions = Phase1Ecc::dataBitPositions();
    for (unsigned i = 0; i < 64; i++)
        bits[dataPositions[i] - 1] = static_cast<unsigned>((data >> i) & 1u);
    const auto& parityPositions = Phase1Ecc::hammingParityPositions();
    for (unsigned i = 0; i < parityPositions.size(); i++)
        bits[parityPositions[i] - 1] = (parity >> i) & 1u;
    bits[71] = (parity >> 7) & 1u;
    return bits;
}

void fromCodeword(const std::array<unsigned, 72>& bits, uint64_t& data, uint8_t& parity)
{
    data = 0;
    const auto& dataPositions = Phase1Ecc::dataBitPositions();
    for (unsigned i = 0; i < 64; i++)
        data |= static_cast<uint64_t>(bits[dataPositions[i] - 1]) << i;
    parity = 0;
    const auto& parityPositions = Phase1Ecc::hammingParityPositions();
    for (unsigned i = 0; i < parityPositions.size(); i++)
        parity |= static_cast<uint8_t>(bits[parityPositions[i] - 1] << i);
    parity |= static_cast<uint8_t>(bits[71] << 7);
}

std::string encodeKey(const Phase1StorageKey& key)
{
    return phase1StorageKeyToString(key);
}

void writeCsvRow(std::ostream& out, unsigned patternId, const Pattern& pattern, uint8_t parity,
                 const std::string& kind, int bit1, int bit2, const Phase1EccResult& result,
                 uint64_t original)
{
    out << patternId << ',' << pattern.name << ',' << hex64(pattern.data) << ',' << hex8(parity)
        << ',' << kind << ',';
    if (bit1 >= 0)
        out << bit1;
    out << ',';
    if (bit2 >= 0)
        out << bit2;
    out << ',' << result.syndrome << ',' << result.overall_parity << ',' << statusName(result.status)
        << ',' << hex64(result.data) << ',' << (result.data == original ? 1 : 0) << '\n';
}
}  // namespace

TEST(Phase1Ecc, ExhaustiveSingleAndDoubleBit)
{
    const char* outputPath = std::getenv("PHASE1_ECC_OUT");
    std::ofstream csv;
    if (outputPath != nullptr && outputPath[0] != '\0')
    {
        csv.open(outputPath, std::ios::out | std::ios::trunc);
        ASSERT_TRUE(csv) << "cannot open PHASE1_ECC_OUT=" << outputPath;
        csv << "pattern_id,pattern_name,data_hex,parity_hex,flip_kind,bit1,bit2,syndrome,"
               "overall_parity,status,corrected_hex,corrected_matches_original\n";
    }

    const auto& dataPositions = Phase1Ecc::dataBitPositions();
    (void)dataPositions;
    unsigned patternId = 0;
    for (const Pattern& pattern : patterns())
    {
        const uint8_t parity = Phase1Ecc::encodeParity(pattern.data);
        const auto originalCodeword = codewordFrom(pattern.data, parity);

        const Phase1EccResult clean = Phase1Ecc::decode(pattern.data, parity);
        ASSERT_EQ(Phase1EccStatus::CLEAN, clean.status);
        ASSERT_EQ(pattern.data, clean.data);
        ASSERT_EQ(0u, clean.syndrome);
        ASSERT_EQ(0u, clean.overall_parity);
        if (csv)
            writeCsvRow(csv, patternId, pattern, parity, "none", -1, -1, clean, pattern.data);

        for (unsigned position = 1; position <= 72; position++)
        {
            auto codeword = originalCodeword;
            codeword[position - 1] ^= 1u;
            uint64_t mutatedData = 0;
            uint8_t mutatedParity = 0;
            fromCodeword(codeword, mutatedData, mutatedParity);
            const Phase1EccResult result = Phase1Ecc::decode(mutatedData, mutatedParity);
            ASSERT_EQ(Phase1EccStatus::CORRECTED, result.status)
                << "pattern=" << pattern.name << " single bit=" << position;
            ASSERT_EQ(pattern.data, result.data)
                << "pattern=" << pattern.name << " single bit=" << position;
            if (csv)
                writeCsvRow(csv, patternId, pattern, parity, "single", position, -1, result,
                            pattern.data);
        }

        for (unsigned first = 1; first <= 72; first++)
        {
            for (unsigned second = first + 1; second <= 72; second++)
            {
                auto codeword = originalCodeword;
                codeword[first - 1] ^= 1u;
                codeword[second - 1] ^= 1u;
                uint64_t mutatedData = 0;
                uint8_t mutatedParity = 0;
                fromCodeword(codeword, mutatedData, mutatedParity);
                const Phase1EccResult result = Phase1Ecc::decode(mutatedData, mutatedParity);
                ASSERT_EQ(Phase1EccStatus::UNCORRECTABLE, result.status)
                    << "pattern=" << pattern.name << " double bits=" << first << "," << second;
                if (csv)
                    writeCsvRow(csv, patternId, pattern, parity, "double", first, second, result,
                                pattern.data);
            }
        }
        patternId++;
    }
}

TEST(Phase1Storage, F1WitnessAndNoScrub)
{
    Phase1Storage storage;
    Phase1StorageKey key;
    key.channel = 0;
    key.rank = 0;
    key.actual_bank = 0;
    key.row = 0;
    key.burst_column = 0;
    key.codeword_index = 0;
    const uint64_t original = 0x0123456789ABCDEFULL;
    storage.initialize(key, original);

    const uint8_t parityBefore = storage.readRawParity(key);
    storage.injectDataBit(key, 15);  // FP16 lane 0 sign bit within 64-bit codeword
    const uint64_t rawAfter = storage.readRawData(key);
    ASSERT_NE(original, rawAfter);
    ASSERT_EQ(parityBefore, storage.readRawParity(key));

    const Phase1EccResult first = storage.readProtected(key);
    ASSERT_EQ(Phase1EccStatus::CORRECTED, first.status);
    ASSERT_EQ(original, first.data);
    ASSERT_EQ(original, storage.readProtected(key).data);
    ASSERT_EQ(Phase1EccStatus::CORRECTED, storage.readProtected(key).status);
    ASSERT_EQ(rawAfter, storage.readRawData(key));  // no automatic scrub

    storage.writeLegal(key, original);
    const Phase1EccResult afterWrite = storage.readProtected(key);
    ASSERT_EQ(Phase1EccStatus::CLEAN, afterWrite.status);
    ASSERT_EQ(original, afterWrite.data);

    const char* witnessPath = std::getenv("PHASE1_G3_WITNESS");
    if (witnessPath != nullptr && witnessPath[0] != '\0')
    {
        std::ofstream witness(witnessPath, std::ios::out | std::ios::trunc);
        ASSERT_TRUE(witness) << "cannot open PHASE1_G3_WITNESS=" << witnessPath;
        witness << "{\n"
                << "  \"key\": \"" << encodeKey(key) << "\",\n"
                << "  \"f1_data_bit\": 15,\n"
                << "  \"data_before_hex\": \"" << hex64(original) << "\",\n"
                << "  \"raw_after_injection_hex\": \"" << hex64(rawAfter) << "\",\n"
                << "  \"parity_hex\": \"" << hex8(parityBefore) << "\",\n"
                << "  \"status\": \"CORRECTED\",\n"
                << "  \"corrected_data_hex\": \"" << hex64(first.data) << "\",\n"
                << "  \"corrected_equals_original\": true,\n"
                << "  \"second_read_status\": \"CORRECTED\",\n"
                << "  \"scrubbed\": false,\n"
                << "  \"legal_write_status\": \"CLEAN\"\n"
                << "}\n";
    }
}

TEST(Phase1Storage, DoubleBitAndReencode)
{
    Phase1Storage storage;
    Phase1StorageKey key;
    key.channel = 0;
    key.rank = 0;
    key.actual_bank = 3;
    key.row = 7;
    key.burst_column = 1;
    key.codeword_index = 2;
    const uint64_t original = 0x0F1E2D3C4B5A6978ULL;
    storage.initialize(key, original);
    storage.injectDataBit(key, 0);
    storage.injectDataBit(key, 1);
    const Phase1EccResult doubleBit = storage.readProtected(key);
    ASSERT_EQ(Phase1EccStatus::UNCORRECTABLE, doubleBit.status);

    storage.writeLegal(key, original);
    const Phase1EccResult afterWrite = storage.readProtected(key);
    ASSERT_EQ(Phase1EccStatus::CLEAN, afterWrite.status);
    ASSERT_EQ(original, afterWrite.data);

    Phase1StorageKey other = key;
    other.codeword_index = 3;
    storage.initialize(other, 0xFEDCBA9876543210ULL);
    storage.injectDataBit(other, 63);
    ASSERT_EQ(Phase1EccStatus::CORRECTED, storage.readProtected(other).status);
    ASSERT_EQ(Phase1EccStatus::CLEAN, storage.readProtected(key).status);
}

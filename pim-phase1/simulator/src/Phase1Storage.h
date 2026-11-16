#ifndef PHASE1_STORAGE_H
#define PHASE1_STORAGE_H

#include <cstdint>
#include <map>
#include <string>

#include "Phase1Ecc.h"

namespace DRAMSim
{
// Stable storage identity for one 64-bit ECC codeword.  A 32 B burst has four
// independent codewords; the caller supplies codeword_index 0..3.
struct Phase1StorageKey
{
    unsigned channel = 0;
    unsigned rank = 0;
    unsigned actual_bank = 0;
    unsigned row = 0;
    unsigned burst_column = 0;
    unsigned codeword_index = 0;

    bool operator<(const Phase1StorageKey& rhs) const
    {
        if (channel != rhs.channel) return channel < rhs.channel;
        if (rank != rhs.rank) return rank < rhs.rank;
        if (actual_bank != rhs.actual_bank) return actual_bank < rhs.actual_bank;
        if (row != rhs.row) return row < rhs.row;
        if (burst_column != rhs.burst_column) return burst_column < rhs.burst_column;
        return codeword_index < rhs.codeword_index;
    }
};

std::string phase1StorageKeyToString(const Phase1StorageKey& key);

// Research storage model with explicit raw data+parity metadata.  Reads do not
// scrub; a persistent bit flip can therefore produce a CE on every read.
// Mutation/raw-inspection methods are test/evaluator-only interfaces.
class Phase1Storage
{
  public:
    void initialize(const Phase1StorageKey& key, uint64_t data);
    void writeLegal(const Phase1StorageKey& key, uint64_t data);
    // PIM/HAB writeback in HOST_ONLY: raw data without a valid ECC format.
    void writeUnprotected(const Phase1StorageKey& key, uint64_t data);
    Phase1EccResult readProtected(const Phase1StorageKey& key) const;

    bool contains(const Phase1StorageKey& key) const;
    uint64_t readRawData(const Phase1StorageKey& key) const;
    uint8_t readRawParity(const Phase1StorageKey& key) const;

    // Test-only raw mutation.  These do not re-encode parity.
    void injectDataBit(const Phase1StorageKey& key, unsigned bit);
    void injectParityBit(const Phase1StorageKey& key, unsigned bit);

  private:
    struct Entry
    {
        uint64_t data = 0;
        uint8_t parity = 0;
        bool ecc_valid = false;
    };

    const Entry& at(const Phase1StorageKey& key) const;
    std::map<Phase1StorageKey, Entry> entries_;
};
}  // namespace DRAMSim

#endif

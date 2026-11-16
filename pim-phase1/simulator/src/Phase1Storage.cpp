#include "Phase1Storage.h"

#include <sstream>
#include <stdexcept>

namespace DRAMSim
{
std::string phase1StorageKeyToString(const Phase1StorageKey& key)
{
    std::ostringstream os;
    os << "ch=" << key.channel << ",rank=" << key.rank << ",bank=" << key.actual_bank
       << ",row=" << key.row << ",burst_col=" << key.burst_column << ",cw=" << key.codeword_index;
    return os.str();
}

const Phase1Storage::Entry& Phase1Storage::at(const Phase1StorageKey& key) const
{
    auto found = entries_.find(key);
    if (found == entries_.end())
        throw std::out_of_range("Phase1Storage key not initialized: " + phase1StorageKeyToString(key));
    return found->second;
}

bool Phase1Storage::contains(const Phase1StorageKey& key) const
{
    return entries_.find(key) != entries_.end();
}

void Phase1Storage::initialize(const Phase1StorageKey& key, uint64_t data)
{
    Entry entry;
    entry.data = data;
    entry.parity = Phase1Ecc::encodeParity(data);
    entry.ecc_valid = true;
    entries_[key] = entry;
}

void Phase1Storage::writeLegal(const Phase1StorageKey& key, uint64_t data)
{
    initialize(key, data);
}

void Phase1Storage::writeUnprotected(const Phase1StorageKey& key, uint64_t data)
{
    Entry entry;
    entry.data = data;
    entry.parity = 0;
    entry.ecc_valid = false;
    entries_[key] = entry;
}

Phase1EccResult Phase1Storage::readProtected(const Phase1StorageKey& key) const
{
    const Entry& entry = at(key);
    if (!entry.ecc_valid)
    {
        Phase1EccResult result;
        result.data = entry.data;
        result.status = Phase1EccStatus::UNPROTECTED_FORMAT;
        result.syndrome = 0;
        result.overall_parity = 0;
        return result;
    }
    return Phase1Ecc::decode(entry.data, entry.parity);
}

uint64_t Phase1Storage::readRawData(const Phase1StorageKey& key) const
{
    return at(key).data;
}

uint8_t Phase1Storage::readRawParity(const Phase1StorageKey& key) const
{
    return at(key).parity;
}

void Phase1Storage::injectDataBit(const Phase1StorageKey& key, unsigned bit)
{
    if (bit >= 64)
        throw std::out_of_range("Phase1Storage data bit must be in [0,63]");
    auto found = entries_.find(key);
    if (found == entries_.end())
        throw std::out_of_range("Phase1Storage key not initialized: " + phase1StorageKeyToString(key));
    found->second.data ^= (uint64_t(1) << bit);
}

void Phase1Storage::injectParityBit(const Phase1StorageKey& key, unsigned bit)
{
    if (bit >= 8)
        throw std::out_of_range("Phase1Storage parity bit must be in [0,7]");
    auto found = entries_.find(key);
    if (found == entries_.end())
        throw std::out_of_range("Phase1Storage key not initialized: " + phase1StorageKeyToString(key));
    found->second.parity ^= static_cast<uint8_t>(1u << bit);
}

}  // namespace DRAMSim

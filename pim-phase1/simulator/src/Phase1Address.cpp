#include "Phase1Address.h"

#include <sstream>
#include <stdexcept>

#include "SystemConfiguration.h"
#include "Utils.h"

namespace DRAMSim
{
bool Phase1Geometry::valid() const
{
    if (num_channels == 0 || num_ranks == 0 || num_banks == 0 || num_bank_groups == 0 ||
        num_rows == 0 || num_cols == 0 || burst_length == 0 || jedec_data_bus_bits == 0)
        return false;
    if (!isPowerOfTwo(num_channels) || !isPowerOfTwo(num_ranks) || !isPowerOfTwo(num_banks) ||
        !isPowerOfTwo(num_bank_groups) || !isPowerOfTwo(num_rows) || !isPowerOfTwo(num_cols) ||
        !isPowerOfTwo(burst_length))
        return false;
    if (num_banks % num_bank_groups != 0)
        return false;
    if (num_cols % burst_length != 0)
        return false;
    if ((jedec_data_bus_bits % 8) != 0 || (burst_length * jedec_data_bus_bits) % 8 != 0)
        return false;
    if (num_bank_groups > num_banks)
        return false;
    return true;
}

Phase1Address::Phase1Address(const Phase1Geometry& geometry) : geometry_(geometry)
{
    if (!geometry_.valid())
        throw std::invalid_argument("invalid Phase1Geometry");
    deriveBitWidths();
}

uint64_t Phase1Address::mask(unsigned bits)
{
    if (bits == 0)
        return 0;
    if (bits >= 64)
        return ~uint64_t(0);
    return (uint64_t(1) << bits) - 1;
}

void Phase1Address::deriveBitWidths()
{
    channel_bits_ = uLog2(geometry_.num_channels);
    rank_bits_ = uLog2(geometry_.num_ranks);
    bank_bits_ = uLog2(geometry_.num_banks);
    bankgroup_bits_ = uLog2(geometry_.num_bank_groups);
    bank_within_group_bits_ = bank_bits_ - bankgroup_bits_;
    row_bits_ = uLog2(geometry_.num_rows);
    column_bits_ = uLog2(geometry_.num_cols / geometry_.burst_length);
    offset_bits_ = uLog2(geometry_.burst_bytes());
}

bool Phase1Address::inRange(const Phase1AddrTuple& tuple) const
{
    if (tuple.channel >= geometry_.num_channels || tuple.rank >= geometry_.num_ranks ||
        tuple.bank >= geometry_.num_banks || tuple.row >= geometry_.num_rows ||
        tuple.burst_column >= geometry_.burst_columns())
        return false;
    return true;
}

uint64_t Phase1Address::encode(const Phase1AddrTuple& tuple) const
{
    if (!inRange(tuple))
        throw std::out_of_range("Phase1AddrTuple outside geometry");
    const unsigned bank_within_group = tuple.bank & ((1u << bank_within_group_bits_) - 1u);
    const unsigned bank_group = tuple.bank >> bank_within_group_bits_;

    uint64_t address = tuple.rank & mask(rank_bits_);
    address = (address << row_bits_) | (tuple.row & mask(row_bits_));
    address = (address << column_bits_) | (tuple.burst_column & mask(column_bits_));
    address = (address << bankgroup_bits_) | (bank_group & mask(bankgroup_bits_));
    address = (address << bank_within_group_bits_) | bank_within_group;
    address = (address << channel_bits_) | (tuple.channel & mask(channel_bits_));
    address = (address << offset_bits_);
    return address;
}

Phase1AddrTuple Phase1Address::decode(uint64_t address) const
{
    Phase1AddrTuple tuple;
    uint64_t value = address;
    const uint64_t offset = value & mask(offset_bits_);
    value >>= offset_bits_;
    tuple.channel = static_cast<unsigned>(value & mask(channel_bits_));
    value >>= channel_bits_;
    const unsigned bank_within_group = static_cast<unsigned>(value & mask(bank_within_group_bits_));
    value >>= bank_within_group_bits_;
    const unsigned bank_group = static_cast<unsigned>(value & mask(bankgroup_bits_));
    value >>= bankgroup_bits_;
    tuple.burst_column = static_cast<unsigned>(value & mask(column_bits_));
    value >>= column_bits_;
    tuple.row = static_cast<unsigned>(value & mask(row_bits_));
    value >>= row_bits_;
    tuple.rank = static_cast<unsigned>(value & mask(rank_bits_));
    value >>= rank_bits_;
    tuple.bank = (bank_group << bank_within_group_bits_) | bank_within_group;
    if (offset != 0 || value != 0)
        throw std::out_of_range("Phase1 address has non-zero offset or high bits");
    return tuple;
}

uint64_t Phase1Address::addressMask() const
{
    const unsigned total_bits = channel_bits_ + rank_bits_ + bank_bits_ + row_bits_ +
                                column_bits_ + offset_bits_;
    return mask(total_bits);
}

Phase1Geometry Phase1Address::fromConfiguration()
{
    Phase1Geometry geometry;
    geometry.num_channels = getConfigParam(UINT, "NUM_CHANS");
    geometry.num_ranks = getConfigParam(UINT, "NUM_RANKS");
    geometry.num_banks = getConfigParam(UINT, "NUM_BANKS");
    geometry.num_bank_groups = getConfigParam(UINT, "NUM_BANK_GROUPS");
    geometry.num_rows = getConfigParam(UINT, "NUM_ROWS");
    geometry.num_cols = getConfigParam(UINT, "NUM_COLS");
    geometry.burst_length = getConfigParam(UINT, "BL");
    geometry.jedec_data_bus_bits = getConfigParam(UINT, "JEDEC_DATA_BUS_BITS");
    return geometry;
}

std::string phase1AddrToString(const Phase1AddrTuple& tuple)
{
    std::ostringstream os;
    os << "ch=" << tuple.channel << ",rank=" << tuple.rank << ",bank=" << tuple.bank
       << ",row=" << tuple.row << ",burst_col=" << tuple.burst_column;
    return os.str();
}

}  // namespace DRAMSim

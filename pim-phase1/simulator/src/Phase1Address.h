#ifndef PHASE1_ADDRESS_H
#define PHASE1_ADDRESS_H

#include <cstdint>
#include <string>

namespace DRAMSim
{
// A Scheme8 address tuple.  The column is the 32 B burst-column index used by
// BusPacket/Bank, not the raw JEDEC column.
struct Phase1AddrTuple
{
    unsigned channel = 0;
    unsigned rank = 0;
    unsigned bank = 0;
    unsigned row = 0;
    unsigned burst_column = 0;

    bool operator==(const Phase1AddrTuple& rhs) const
    {
        return channel == rhs.channel && rank == rhs.rank && bank == rhs.bank && row == rhs.row &&
               burst_column == rhs.burst_column;
    }
    bool operator!=(const Phase1AddrTuple& rhs) const { return !(*this == rhs); }
};

struct Phase1Geometry
{
    unsigned num_channels = 0;
    unsigned num_ranks = 0;
    unsigned num_banks = 0;
    unsigned num_bank_groups = 0;
    unsigned num_rows = 0;
    unsigned num_cols = 0;
    unsigned burst_length = 0;
    unsigned jedec_data_bus_bits = 0;

    unsigned burst_bytes() const { return burst_length * jedec_data_bus_bits / 8; }
    unsigned burst_columns() const { return num_cols / burst_length; }
    bool valid() const;
};

// Encode/decode the fixed upstream Scheme8 layout used by PIMAddrManager:
// rank:row:col:bg:bank_within_group:channel:byte_offset.
//
// This is a Phase1 test/address utility.  It is an explicit model-side
// assumption, not a claim that the upstream simulator exposes a general
// tuple->address API.  Round-trip tests compare it with the upstream
// AddrMapping::addressMapping decoder.
class Phase1Address
{
  public:
    explicit Phase1Address(const Phase1Geometry& geometry);

    uint64_t encode(const Phase1AddrTuple& tuple) const;
    Phase1AddrTuple decode(uint64_t address) const;
    bool inRange(const Phase1AddrTuple& tuple) const;

    const Phase1Geometry& geometry() const { return geometry_; }
    uint64_t addressMask() const;

    static Phase1Geometry fromConfiguration();

  private:
    Phase1Geometry geometry_;
    unsigned channel_bits_ = 0;
    unsigned rank_bits_ = 0;
    unsigned bank_bits_ = 0;
    unsigned bankgroup_bits_ = 0;
    unsigned bank_within_group_bits_ = 0;
    unsigned row_bits_ = 0;
    unsigned column_bits_ = 0;
    unsigned offset_bits_ = 0;

    static uint64_t mask(unsigned bits);
    void deriveBitWidths();
};

std::string phase1AddrToString(const Phase1AddrTuple& tuple);

}  // namespace DRAMSim

#endif

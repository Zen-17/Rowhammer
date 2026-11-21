#include "Phase2Layout.h"

#include <algorithm>
#include <stdexcept>

namespace DRAMSim
{
namespace phase2
{
unsigned tileRowOffset(uint32_t tile)
{
    return (8u * tile) / 32u;
}

unsigned tileFirstColumn(uint32_t tile)
{
    return (8u * tile) % 32u;
}

std::vector<BurstKey> tileFootprint(unsigned base_row, uint32_t tile, unsigned num_banks)
{
    std::vector<BurstKey> keys;
    keys.reserve(static_cast<std::size_t>(num_banks) * 8u);
    const unsigned row = base_row + tileRowOffset(tile);
    const unsigned first = tileFirstColumn(tile);
    for (unsigned bank = 0; bank < num_banks; bank++)
        for (unsigned col = first; col < first + 8u; col++)
            keys.push_back(BurstKey{0, 0, bank, row, col});
    return keys;
}

uint64_t pageRegionId(unsigned base_row, uint32_t tile)
{
    const unsigned row = base_row + tileRowOffset(tile);
    return static_cast<uint64_t>(row) * 4ULL + tileFirstColumn(tile) / 8u;
}

uint64_t codewordRegionId(const Phase1Address& address, unsigned bank, unsigned row,
                          unsigned burst_col, unsigned codeword, unsigned channel)
{
    const Phase1Geometry& geometry = address.geometry();
    return (((static_cast<uint64_t>(channel) * geometry.num_banks + bank) * geometry.num_rows +
             row) *
                geometry.burst_columns() +
            burst_col) *
               4ULL +
           codeword;
}

ReportFootprint buildReportFootprint(const Phase1PublicReport& report,
                                     const Phase1Address& address)
{
    const Phase1Geometry& geometry = address.geometry();
    ReportFootprint footprint;
    if (report.region_type == "bank")
    {
        footprint.kind = ReportFootprint::Kind::BANK;
        footprint.channel = static_cast<unsigned>(report.region_id / geometry.num_banks);
        footprint.bank = static_cast<unsigned>(report.region_id % geometry.num_banks);
        footprint.rank = 0;
        return footprint;
    }
    if (report.region_type == "page")
    {
        const uint64_t base = report.region_id * 4096ULL;
        footprint.kind = ReportFootprint::Kind::KEYS;
        for (uint64_t offset = 0; offset < 4096ULL; offset += 32ULL)
        {
            const Phase1AddrTuple tuple = address.decode(base + offset);
            footprint.keys.push_back(BurstKey{tuple.channel, tuple.rank, tuple.bank,
                                              tuple.row, tuple.burst_column});
        }
        return footprint;
    }
    if (report.region_type == "codeword")
    {
        uint64_t value = report.region_id;
        const unsigned codeword = static_cast<unsigned>(value % 4ULL);
        value /= 4ULL;
        const unsigned burst_col = static_cast<unsigned>(value % geometry.burst_columns());
        value /= geometry.burst_columns();
        const unsigned row = static_cast<unsigned>(value % geometry.num_rows);
        value /= geometry.num_rows;
        const unsigned bank = static_cast<unsigned>(value % geometry.num_banks);
        const unsigned channel = static_cast<unsigned>(value / geometry.num_banks);
        footprint.kind = ReportFootprint::Kind::KEYS;
        footprint.keys.push_back(BurstKey{channel, 0, bank, row, burst_col});
        return footprint;
    }
    throw std::invalid_argument("unknown Phase 2 report region type");
}

ReportIndex buildReportIndex(const ReportFootprint& footprint)
{
    ReportIndex index;
    index.bank_kind = (footprint.kind == ReportFootprint::Kind::BANK);
    index.channel = footprint.channel;
    index.rank = footprint.rank;
    index.bank = footprint.bank;
    std::map<std::pair<unsigned, unsigned>, std::set<unsigned>> grouped;
    std::map<unsigned, std::set<unsigned>> rows;
    for (const BurstKey& key : footprint.keys)
    {
        grouped[std::make_pair(key.bank, key.row)].insert(key.burst_col);
        rows[key.bank].insert(key.row);
    }
    for (const auto& entry : grouped)
    {
        RowCols rc;
        rc.bank = entry.first.first;
        rc.row = entry.first.second;
        rc.cols.assign(entry.second.begin(), entry.second.end());
        index.entries.push_back(rc);
    }
    for (const auto& entry : rows)
        index.rows_by_bank.push_back(
            std::make_pair(entry.first, std::vector<unsigned>(entry.second.begin(),
                                                             entry.second.end())));
    return index;
}

bool itemOverlaps(const Phase1Address& address, unsigned base_row, uint32_t tile,
                  const ReportIndex& index)
{
    if (index.bank_kind)
        return index.channel == 0 && index.rank == 0 &&
               index.bank < address.geometry().num_banks;
    const unsigned row = base_row + tileRowOffset(tile);
    const unsigned first = tileFirstColumn(tile);
    for (const RowCols& entry : index.entries)
    {
        if (entry.row != row)
            continue;
        for (unsigned col : entry.cols)
            if (col >= first && col < first + 8u)
                return true;
    }
    return false;
}

bool itemTouchesRadius(unsigned base_row, uint32_t tile, const ReportIndex& index,
                       unsigned radius)
{
    if (index.bank_kind)
        return index.channel == 0 && index.rank == 0;
    const unsigned row = base_row + tileRowOffset(tile);
    for (const auto& entry : index.rows_by_bank)
        for (unsigned report_row : entry.second)
            if (row >= report_row && row - report_row <= radius)
                return true;
            else if (report_row >= row && report_row - row <= radius)
                return true;
    return false;
}

uint64_t footprintBursts(const ReportFootprint& footprint, const Phase1Address& address)
{
    if (footprint.kind == ReportFootprint::Kind::BANK)
        return static_cast<uint64_t>(address.geometry().num_rows) *
               address.geometry().burst_columns();
    return footprint.keys.size();
}

}  // namespace phase2
}  // namespace DRAMSim

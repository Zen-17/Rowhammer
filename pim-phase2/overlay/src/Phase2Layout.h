#ifndef PHASE2_LAYOUT_H
#define PHASE2_LAYOUT_H

#include <cstdint>
#include <map>
#include <set>
#include <utility>
#include <vector>

#include "Phase1Address.h"
#include "Phase1Reports.h"
#include "Phase2Types.h"

namespace DRAMSim
{
namespace phase2
{
struct ReportFootprint
{
    enum class Kind
    {
        BANK,
        KEYS
    };
    Kind kind = Kind::KEYS;
    unsigned channel = 0;
    unsigned rank = 0;
    unsigned bank = 0;
    std::vector<BurstKey> keys;
};

struct RowCols
{
    unsigned bank = 0;
    unsigned row = 0;
    std::vector<unsigned> cols;
};

struct ReportIndex
{
    bool bank_kind = false;
    unsigned channel = 0;
    unsigned rank = 0;
    unsigned bank = 0;
    std::vector<RowCols> entries;
    std::vector<std::pair<unsigned, std::vector<unsigned>>> rows_by_bank;
};

unsigned tileRowOffset(uint32_t tile);
unsigned tileFirstColumn(uint32_t tile);
std::vector<BurstKey> tileFootprint(unsigned base_row, uint32_t tile,
                                    unsigned num_banks = 16);
uint64_t pageRegionId(unsigned base_row, uint32_t tile);
uint64_t codewordRegionId(const Phase1Address& address, unsigned bank, unsigned row,
                          unsigned burst_col, unsigned codeword, unsigned channel = 0);

ReportFootprint buildReportFootprint(const Phase1PublicReport& report,
                                     const Phase1Address& address);
ReportIndex buildReportIndex(const ReportFootprint& footprint);
bool itemOverlaps(const Phase1Address& address, unsigned base_row, uint32_t tile,
                  const ReportIndex& index);
bool itemTouchesRadius(unsigned base_row, uint32_t tile, const ReportIndex& index,
                       unsigned radius);
uint64_t footprintBursts(const ReportFootprint& footprint, const Phase1Address& address);

}  // namespace phase2
}  // namespace DRAMSim

#endif

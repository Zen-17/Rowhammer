#include "RegionMap.h"

#include "ReadDisturbanceModel.h"

#include <cmath>
#include <stdexcept>

namespace
{
const double kHcMin = 14531.0;
const unsigned kDetect = 1024;
const double kFth = 1.0 / 16.0;
const unsigned kMisra = 15;

unsigned scaledPeriod(double value)
{
    unsigned period = static_cast<unsigned>(std::floor(value));
    return period < 1u ? 1u : period;
}
}  // namespace

void RegionMap::configure(const std::string& scheme, double g, double c, double scale)
{
    if (scheme != "FIX" && scheme != "ADA" && scheme != "OURS" && scheme != "PARA" && scheme != "NONE")
        throw std::invalid_argument("region scheme is not one of FIX, ADA, OURS, PARA, NONE");
    if (!(scale > 0.0 && scale <= 1.0))
        throw std::invalid_argument("threshold_scale must be in (0, 1]");
    if (g <= 0.0 || c <= 0.0)
        throw std::invalid_argument("g and c must be positive");
    scheme_ = scheme;
    const double hc = kHcMin * scale;
    const double safe = hc / g;
    const unsigned detect = scaledPeriod(static_cast<double>(kDetect) * scale);
    r1_ = scaledPeriod(safe / (c * static_cast<double>(kRegion)));
    r0_ = scaledPeriod((safe / c - static_cast<double>(detect)) / (kFth * static_cast<double>(kRegion)));
    detect_ = detect;
    // Alarm when a single-row count is strictly greater than E_det / 16.
    alarmAt_ = static_cast<unsigned>(std::floor(static_cast<double>(detect) * kFth));
    swaps_ = 0;
    commands_ = 0;
    paraCommands_ = 0;
    rng_ = 0x9E3779B97F4A7C15ull;
    sawL2_ = false;
    transitions_.clear();
    regions_.assign(kRegions, Region());
    misra_.assign(kBanks, std::vector<Slot>(kMisra));
    for (unsigned bank = 0; bank < kBanks; ++bank)
        filled_[bank] = 0;
}

unsigned RegionMap::keyed(const Region& region, unsigned offset) const
{
    const unsigned key = region.migrated[offset] ? region.keyNew : region.keyOld;
    return offset ^ key;
}

unsigned RegionMap::physical(unsigned logicalRow) const
{
    if (logicalRow >= kRows)
        throw std::out_of_range("logical row is outside the bank");
    const unsigned region = regionOf(logicalRow);
    return region * kRegion + keyed(regions_[region], offsetOf(logicalRow));
}

unsigned RegionMap::logical(unsigned physicalRow) const
{
    if (physicalRow >= kRows)
        throw std::out_of_range("physical row is outside the bank");
    const unsigned region = regionOf(physicalRow);
    const unsigned offset = offsetOf(physicalRow);
    for (unsigned candidate = 0; candidate < kRegion; ++candidate)
    {
        if (keyed(regions_[region], candidate) == offset)
            return region * kRegion + candidate;
    }
    throw std::logic_error("region map has no inverse");
}

unsigned RegionMap::level(unsigned region) const
{
    return regions_.at(region).level;
}

uint64_t RegionMap::epoch(unsigned region) const
{
    return regions_.at(region).epoch;
}

unsigned RegionMap::period(unsigned region) const
{
    if (scheme_ == "FIX")
        return r1_;
    if (scheme_ == "NONE" || scheme_ == "PARA")
        return 0;
    return regions_.at(region).level == 0 ? r0_ : r1_;
}

void RegionMap::setScrubber(ScrubFn function, void* context)
{
    scrub_ = function;
    scrubContext_ = context;
}

void RegionMap::swapOnce(unsigned regionIndex, ReadDisturbanceModel* model)
{
    Region& region = regions_.at(regionIndex);
    unsigned delta = region.keyOld ^ region.keyNew;
    if (delta == 0)
        delta = 1;
    unsigned left = region.nextPair;
    while (left < kRegion && (region.migrated[left] || left > (left ^ delta)))
        left += 1;
    if (left >= kRegion)
    {
        region.keyOld = region.keyNew;
        region.keyNew = (region.keyNew + 1u) & 63u;
        if (region.keyNew == region.keyOld)
            region.keyNew ^= 1u;
        for (unsigned offset = 0; offset < kRegion; ++offset)
            region.migrated[offset] = false;
        region.nextPair = 0;
        region.epoch += 1;
        delta = region.keyOld ^ region.keyNew;
        if (delta == 0)
            delta = 1;
        left = 0;
    }
    const unsigned right = left ^ delta;
    const unsigned source = regionIndex * kRegion + (left ^ region.keyOld);
    const unsigned dest = regionIndex * kRegion + (right ^ region.keyOld);
    for (unsigned bank = 0; bank < kBanks; ++bank)
    {
        if (scrub_ != nullptr)
            scrub_(bank, source, dest, scrubContext_);
        if (model != nullptr)
        {
            model->noteSwap(bank, source);
            if (dest != source)
                model->noteSwap(bank, dest);
        }
    }
    region.migrated[left] = true;
    region.migrated[right] = true;
    region.nextPair = left + 1;
    swaps_ += 1;
}

void RegionMap::observe(unsigned bank, unsigned row)
{
    std::vector<Slot>& slots = misra_.at(bank);
    for (unsigned i = 0; i < kMisra; ++i)
    {
        if (slots[i].row == static_cast<int>(row))
        {
            slots[i].count += 1;
            return;
        }
    }
    for (unsigned i = 0; i < kMisra; ++i)
    {
        if (slots[i].row < 0)
        {
            slots[i].row = static_cast<int>(row);
            slots[i].count = 1;
            return;
        }
    }
    for (unsigned i = 0; i < kMisra; ++i)
    {
        if (slots[i].count > 0)
            slots[i].count -= 1;
        if (slots[i].count == 0)
            slots[i].row = -1;
    }
}

void RegionMap::settleWindows()
{
    for (unsigned bank = 0; bank < kBanks; ++bank)
    {
        if (filled_[bank] < detect_)
            continue;
        unsigned hottest = 0;
        int hotRow = -1;
        for (const Slot& slot : misra_[bank])
        {
            if (slot.count > hottest)
            {
                hottest = slot.count;
                hotRow = slot.row;
            }
        }
        if (scheme_ != "FIX" && scheme_ != "NONE" && scheme_ != "PARA" && hotRow >= 0 && hottest > alarmAt_)
        {
            Region& region = regions_[regionOf(static_cast<unsigned>(hotRow))];
            region.alarmed = true;
            region.quiet = 0;
            if (region.level == 0)
            {
                region.level = 1;
                transitions_.push_back(Transition{commands_, regionOf(static_cast<unsigned>(hotRow)), 0u, 1u});
            }
        }
        misra_[bank].assign(kMisra, Slot());
        filled_[bank] = 0;
    }
}

void RegionMap::setParaProbability(double probability)
{
    if (probability < 0.0 || probability > 1.0)
        throw std::invalid_argument("PARA probability must be in [0, 1]");
    paraP_ = probability;
}

uint64_t RegionMap::nextRandom()
{
    rng_ += 0x9E3779B97F4A7C15ull;
    uint64_t z = rng_;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

unsigned RegionMap::migrationSource(unsigned regionIndex) const
{
    const Region& region = regions_.at(regionIndex);
    unsigned delta = region.keyOld ^ region.keyNew;
    if (delta == 0)
        delta = 1;
    unsigned left = region.nextPair;
    while (left < kRegion && (region.migrated[left] || left > (left ^ delta)))
        left += 1;
    if (left >= kRegion)
        left = 0;
    const unsigned right = left ^ delta;
    const unsigned source = regionIndex * kRegion + (left ^ region.keyOld);
    (void)right;
    return source;
}

void RegionMap::noteCommand(unsigned row, const unsigned* banks, unsigned nBanks,
                            ReadDisturbanceModel* model)
{
    if (row >= kRows)
        throw std::out_of_range("region activation is outside the HBM2 geometry");
    if (scheme_ == "NONE" || nBanks == 0)
        return;
    commands_ += 1;
    for (unsigned i = 0; i < nBanks; ++i)
    {
        const unsigned bank = banks[i];
        if (bank >= kBanks)
            throw std::out_of_range("region activation is outside the HBM2 geometry");
        observe(bank, row);
        filled_[bank] += 1;
        if (filled_[bank] >= detect_)
            settleWindows();
    }
    if (scheme_ == "PARA")
    {
        if (model != nullptr && paraP_ > 0.0)
        {
            const double draw = static_cast<double>(nextRandom() >> 11) * (1.0 / 9007199254740992.0);
            if (draw < paraP_)
            {
                paraCommands_ += 1;
                for (unsigned i = 0; i < nBanks; ++i)
                {
                    const unsigned bank = banks[i];
                    if (row > 0)
                        model->noteRefresh(bank, row - 1);
                    if (row + 1 < kRows)
                        model->noteRefresh(bank, row + 1);
                }
            }
        }
        return;
    }
    Region& region = regions_[regionOf(row)];
    const unsigned every = period(regionOf(row));
    if (every == 0)
        return;
    region.since += 1;
    if (defer_)
        return;
    while (region.since >= period(regionOf(row)))
    {
        swapOnce(regionOf(row), model);
        region.since -= period(regionOf(row));
    }
}

void RegionMap::flush(ReadDisturbanceModel* model)
{
    if (scheme_ == "NONE" || scheme_ == "PARA")
        return;
    for (unsigned region = 0; region < kRegions; ++region)
    {
        while (regions_[region].since >= period(region))
        {
            swapOnce(region, model);
            regions_[region].since -= period(region);
        }
    }
}

void RegionMap::noteActivate(unsigned bank, unsigned row, ReadDisturbanceModel* model)
{
    const unsigned banks[1] = {bank};
    noteCommand(row, banks, 1, model);
}

void RegionMap::noteRefreshWindow()
{
    for (unsigned index = 0; index < regions_.size(); ++index)
    {
        Region& region = regions_[index];
        if (region.alarmed)
        {
            region.quiet = 0;
            region.alarmed = false;
            continue;
        }
        region.quiet += 1;
        if (region.level == 1 && region.quiet >= 2)
        {
            region.level = 0;
            transitions_.push_back(Transition{commands_, index, 1u, 0u});
        }
    }
}

void RegionMap::forceSwap(ReadDisturbanceModel* model)
{
    swapOnce(0, model);
}

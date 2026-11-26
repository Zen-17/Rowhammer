#include "RegionMap.h"

#include "ReadDisturbanceModel.h"

#include <cmath>
#include <stdexcept>

namespace
{
const double kHcMin = 14531.0;
const unsigned kDetect = 1024;
const double kFth = 1.0 / 16.0;

unsigned scaledPeriod(double value)
{
    unsigned period = static_cast<unsigned>(std::floor(value));
    return period < 1u ? 1u : period;
}
}  // namespace

void RegionMap::configure(const std::string& scheme, double g, double c, double scale)
{
    const bool ours = scheme == "OURS" || scheme.compare(0, 5, "OURS-") == 0;
    if (scheme != "FIX" && scheme != "ADA" && !ours && scheme != "PARA" && scheme != "NONE" &&
        scheme != "REACT")
        throw std::invalid_argument("region scheme is not one of FIX, ADA, OURS, PARA, REACT, NONE");
    if (!(scale > 0.0 && scale <= 1.0))
        throw std::invalid_argument("threshold_scale must be in (0, 1]");
    if (g <= 0.0 || c <= 0.0)
        throw std::invalid_argument("g and c must be positive");
    regionRows_ = kRegion;
    misraK_ = 15;
    prngKeys_ = false;
    allowCeFastPeriod_ = false;
    epochCheck_ = (scheme == "FIX" || scheme == "ADA" || (ours && scheme != "OURS-EPOCH"));
    lastG_ = g;
    lastC_ = c;
    lastScale_ = scale;
    scheme_ = ours ? "OURS" : scheme;
    ceFeedback_ = ours && scheme != "OURS-CE";
    scrubOn_ = scheme != "OURS-SCRUB";
    epochCorrupt_ = scheme == "OURS-EPOCH";
    detector_ = scheme == "ADA" || (ours && scheme != "OURS-DET");
    recovery_ = scheme == "REACT" || scheme == "PARA" || ours;
    const double hc = kHcMin * scale;
    const double safe = hc / g;
    const unsigned detect = scaledPeriod(static_cast<double>(kDetect) * scale);
    r1_ = scaledPeriod(safe / (c * static_cast<double>(regionRows_)));
    r0_ = scaledPeriod((safe / c - static_cast<double>(detect)) / (kFth * static_cast<double>(regionRows_)));
    detect_ = detect;
    // Alarm when a single-row count is strictly greater than E_det / 16.
    alarmAt_ = static_cast<unsigned>(std::floor(static_cast<double>(detect) * kFth));
    swaps_ = 0;
    commands_ = 0;
    paraCommands_ = 0;
    rng_ = 0x9E3779B97F4A7C15ull;
    sawL2_ = false;
    swapped_.assign(kRows, 0);
    spareOfLogical_.assign(kRows, -1);
    logicalOfSpare_.assign(kRows, -1);
    for (unsigned bank = 0; bank < kBanks; ++bank)
        spareUsed_[bank] = 0;
    transitions_.clear();
    Region initRegion;
    initRegion.migrated.assign(regionRows_, 0);
    regions_.assign(kRows / regionRows_, initRegion);
    misra_.assign(kBanks, std::vector<Slot>(misraK_));
    for (unsigned bank = 0; bank < kBanks; ++bank)
        filled_[bank] = 0;
}

void RegionMap::setSeed(uint64_t seed)
{
    rng_ = seed ^ 0x9E3779B97F4A7C15ull;
    prngKeys_ = true;
}

void RegionMap::setMarginG(double g, double c, double scale)
{
    if (!(g > 0.0 && c > 0.0 && scale > 0.0 && scale <= 1.0))
        throw std::invalid_argument("g, c must be positive and scale in (0, 1]");
    lastG_ = g;
    lastC_ = c;
    lastScale_ = scale;
    const double hc = kHcMin * scale;
    const double safe = hc / g;
    r1_ = scaledPeriod(safe / (c * static_cast<double>(regionRows_)));
    r0_ = scaledPeriod((safe / c - static_cast<double>(detect_)) /
                       (kFth * static_cast<double>(regionRows_)));
}

void RegionMap::setGeometry(unsigned regionRows, unsigned misraK)
{
    if (regionRows != 16u && regionRows != 32u && regionRows != 64u &&
        regionRows != 128u && regionRows != 256u)
        throw std::invalid_argument("regionRows must be in {16, 32, 64, 128, 256}");
    if (misraK < 1u)
        throw std::invalid_argument("misraK must be >= 1");
    regionRows_ = regionRows;
    misraK_ = misraK;
    const double hc = kHcMin * lastScale_;
    const double safe = hc / lastG_;
    r1_ = scaledPeriod(safe / (lastC_ * static_cast<double>(regionRows_)));
    r0_ = scaledPeriod((safe / lastC_ - static_cast<double>(detect_)) /
                       (kFth * static_cast<double>(regionRows_)));
    Region initRegion;
    initRegion.migrated.assign(regionRows_, 0);
    regions_.assign(kRows / regionRows_, initRegion);
    misra_.assign(kBanks, std::vector<Slot>(misraK_));
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
    if (!spareOfLogical_.empty() && spareOfLogical_[logicalRow] >= 0)
        return static_cast<unsigned>(spareOfLogical_[logicalRow]);
    const unsigned region = regionOf(logicalRow);
    return region * regionRows_ + keyed(regions_[region], offsetOf(logicalRow));
}

unsigned RegionMap::logical(unsigned physicalRow) const
{
    if (physicalRow >= kRows)
        throw std::out_of_range("physical row is outside the bank");
    unsigned row = 0;
    if (!logicalOfSpare_.empty() && logicalOfSpare_[physicalRow] >= 0)
        row = static_cast<unsigned>(logicalOfSpare_[physicalRow]);
    else
    {
        const unsigned region = regionOf(physicalRow);
        const unsigned offset = offsetOf(physicalRow);
        bool found = false;
        for (unsigned candidate = 0; candidate < regionRows_; ++candidate)
        {
            if (keyed(regions_[region], candidate) == offset)
            {
                row = region * regionRows_ + candidate;
                found = true;
                break;
            }
        }
        if (!found)
            throw std::logic_error("region map has no inverse");
    }
    if (epochCorrupt_)
        return (row + 1u) % kRows;
    return row;
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
    if (scheme_ == "NONE" || scheme_ == "PARA" || scheme_ == "REACT")
        return 0;
    if (scheme_ == "OURS" && !detector_ && !allowCeFastPeriod_)
        return r0_;
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
    while (left < regionRows_ && (region.migrated[left] || left > (left ^ delta)))
        left += 1;
    if (left >= regionRows_)
    {
        region.keyOld = region.keyNew;
        if (prngKeys_)
            region.keyNew = static_cast<unsigned>(nextRandom() & (regionRows_ - 1u));
        else
            region.keyNew = (region.keyNew + 1u) & (regionRows_ - 1u);
        if (region.keyNew == region.keyOld)
            region.keyNew ^= 1u;
        region.migrated.assign(regionRows_, 0);
        region.nextPair = 0;
        region.epoch += 1;
        delta = region.keyOld ^ region.keyNew;
        if (delta == 0)
            delta = 1;
        left = 0;
    }
    const unsigned right = left ^ delta;
    const unsigned source = regionIndex * regionRows_ + (left ^ region.keyOld);
    const unsigned dest = regionIndex * regionRows_ + (right ^ region.keyOld);
    if ((!logicalOfSpare_.empty() && source < logicalOfSpare_.size() && logicalOfSpare_[source] >= 0) ||
        (!logicalOfSpare_.empty() && dest < logicalOfSpare_.size() && logicalOfSpare_[dest] >= 0))
    {
        region.migrated[left] = 1;
        region.migrated[right] = 1;
        region.nextPair = left + 1;
        return;
    }
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
        if (source < swapped_.size())
            swapped_[source] = 1;
        if (dest < swapped_.size())
            swapped_[dest] = 1;
    }
    region.migrated[left] = 1;
    region.migrated[right] = 1;
    region.nextPair = left + 1;
    swaps_ += 1;
}

void RegionMap::observe(unsigned bank, unsigned row)
{
    std::vector<Slot>& slots = misra_.at(bank);
    for (unsigned i = 0; i < misraK_; ++i)
    {
        if (slots[i].row == static_cast<int>(row))
        {
            slots[i].count += 1;
            return;
        }
    }
    for (unsigned i = 0; i < misraK_; ++i)
    {
        if (slots[i].row < 0)
        {
            slots[i].row = static_cast<int>(row);
            slots[i].count = 1;
            return;
        }
    }
    for (unsigned i = 0; i < misraK_; ++i)
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
        if (detector_ && hotRow >= 0 && hottest > alarmAt_)
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
        misra_[bank].assign(misraK_, Slot());
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
    while (left < regionRows_ && (region.migrated[left] || left > (left ^ delta)))
        left += 1;
    if (left >= regionRows_)
        left = 0;
    const unsigned right = left ^ delta;
    const unsigned source = regionIndex * regionRows_ + (left ^ region.keyOld);
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
    if (scheme_ == "NONE" || scheme_ == "PARA" || scheme_ == "REACT")
        return;
    for (unsigned region = 0; region < regions_.size(); ++region)
    {
        const unsigned every = period(region);
        if (every == 0)
            continue;
        while (regions_[region].since >= every)
        {
            swapOnce(region, model);
            regions_[region].since -= every;
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

void RegionMap::noteCe(unsigned bank, unsigned physicalRow)
{
    if (!ceFeedback_ || bank >= kBanks || physicalRow >= kRows || regions_.empty())
        return;
    unsigned owner = 0;
    bool found = false;
    if (!logicalOfSpare_.empty() && logicalOfSpare_[physicalRow] >= 0)
    {
        owner = static_cast<unsigned>(logicalOfSpare_[physicalRow]);
        found = true;
    }
    else
    {
        const unsigned region = regionOf(physicalRow);
        const unsigned offset = offsetOf(physicalRow);
        for (unsigned candidate = 0; candidate < regionRows_; ++candidate)
        {
            if (keyed(regions_[region], candidate) == offset)
            {
                owner = region * regionRows_ + candidate;
                found = true;
                break;
            }
        }
    }
    if (!found)
        return;
    Region& region = regions_[regionOf(owner)];
    region.alarmed = true;
    region.quiet = 0;
    const bool again = physicalRow < swapped_.size() && swapped_[physicalRow] != 0 && region.level >= 1 &&
                       spareOfLogical_[owner] < 0;
    if (again && spareUsed_[bank] < 16)
    {
        unsigned spare = (kRows - 16u) + spareUsed_[bank];
        if (spare == physicalRow)
            spare -= 16u;
        spareOfLogical_[owner] = static_cast<int>(spare);
        logicalOfSpare_[spare] = static_cast<int>(owner);
        if (region.level != 2)
            transitions_.push_back(Transition{commands_, regionOf(owner), region.level, 2u});
        region.level = 2;
        sawL2_ = true;
        const unsigned nextSpare = spareUsed_[bank] + 1u;
        for (unsigned one = 0; one < kBanks; ++one)
            spareUsed_[one] = nextSpare;
        if (scrub_ != nullptr)
            for (unsigned one = 0; one < kBanks; ++one)
                scrub_(one, physicalRow, spare, scrubContext_);
        return;
    }
    if (region.level == 0)
    {
        region.level = 1;
        transitions_.push_back(Transition{commands_, regionOf(owner), 0u, 1u});
        // The fast period is due now.  A cross-region hammer never increments
        // this region's counter, so the CE itself has to release the first swap.
        const unsigned every = period(regionOf(owner));
        if (every > 0)
            region.since = every;
    }
}

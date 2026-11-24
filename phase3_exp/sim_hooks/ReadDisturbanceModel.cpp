#include "ReadDisturbanceModel.h"

#include <cmath>
#include <stdexcept>

namespace
{
const double kHcMin = 14531.0;
const double kMedian = 1.35;
const double kSigma = 0.25;
// Flip levels 1..10.  Locked k in {1,2,4,8,10}; the other levels are linear.
const double kMultiplier[ReadDisturbanceModel::kLevels] = {
    1.00, 1.19, 1.30, 1.41, 1.4725, 1.535, 1.5975, 1.66, 1.71, 1.76};

uint64_t mix64(uint64_t value)
{
    value += 0x9E3779B97F4A7C15ull;
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
    return value ^ (value >> 31);
}
}  // namespace

ReadDisturbanceModel::ReadDisturbanceModel() : rows_(static_cast<size_t>(kBanks) * kRows) {}

void ReadDisturbanceModel::configure(uint64_t seed, double thresholdScale, const std::string& genMode)
{
    if (!(thresholdScale > 0.0 && thresholdScale <= 1.0))
        throw std::invalid_argument("threshold_scale must be in (0, 1]");
    if (genMode != "HBM2" && genMode != "HBM3_STYLE")
        throw std::invalid_argument("gen_mode must be HBM2 or HBM3_STYLE");
    scale_ = thresholdScale;
    genMode_ = genMode;
    for (int stream = 0; stream < 3; ++stream)
        stream_[stream] = mix64(seed + static_cast<uint64_t>(stream) * 0x100000001B3ull);
    activates_ = 0;
    cumulative_ = 0;
    scrubbed_ = 0;
    firstCrossings_ = 0;
    swapActs_ = 0;
    activateClears_ = 0;
    swapClears_ = 0;
    swapClearsBank0_ = 0;
    refreshClears_ = 0;
    crossings_.clear();
    flips_.clear();
    used_.clear();
    rows_.assign(static_cast<size_t>(kBanks) * kRows, RowState());
}

void ReadDisturbanceModel::setInjector(InjectFn function, void* context)
{
    inject_ = function;
    context_ = context;
}

void ReadDisturbanceModel::forceRow(unsigned bank, unsigned row, double hcFirst, double alpha)
{
    RowState& rowState = state(bank, row);
    rowState.hc = hcFirst;
    rowState.alpha = alpha;
    rowState.ready = true;
    rowState.forced = true;
    rowState.emitted = 0;
    rowState.disturbance = 0;
}

ReadDisturbanceModel::RowState& ReadDisturbanceModel::state(unsigned bank, unsigned row)
{
    return rows_[static_cast<size_t>(bank) * kRows + row];
}

const ReadDisturbanceModel::RowState& ReadDisturbanceModel::state(unsigned bank, unsigned row) const
{
    return rows_[static_cast<size_t>(bank) * kRows + row];
}

double ReadDisturbanceModel::uniform(int stream)
{
    stream_[stream] = mix64(stream_[stream]);
    return (stream_[stream] >> 11) * (1.0 / 9007199254740992.0);
}

void ReadDisturbanceModel::ensure(unsigned bank, unsigned row)
{
    RowState& rowState = state(bank, row);
    if (rowState.ready)
        return;
    const double mu = std::log(kMedian * kHcMin);
    double hc = 0;
    do
    {
        const double u1 = std::max(1e-12, uniform(0));
        const double u2 = uniform(0);
        const double z = std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
        hc = std::exp(mu + kSigma * z);
    } while (hc < kHcMin);
    rowState.hc = hc;
    rowState.alpha = 1.0 + 2.5 * uniform(1);
    rowState.ready = true;
}

double ReadDisturbanceModel::disturbance(unsigned bank, unsigned row) const
{
    return state(bank, row).disturbance;
}

double ReadDisturbanceModel::hcFirst(unsigned bank, unsigned row) const
{
    return state(bank, row).hc * scale_;
}

double ReadDisturbanceModel::firstThreshold(unsigned bank, unsigned row)
{
    ensure(bank, row);
    const RowState& rowState = state(bank, row);
    double threshold = 2.0 * rowState.alpha * rowState.hc * scale_;
    const double floorT = 2.0 * 14531.0 * scale_;
    if (!rowState.forced && threshold < floorT)
        threshold = floorT;
    return threshold;
}

unsigned ReadDisturbanceModel::flipsOn(unsigned bank, unsigned row) const
{
    return static_cast<unsigned>(state(bank, row).emitted);
}

bool ReadDisturbanceModel::drawBit(Flip& flip)
{
    // 64 data bits and 8 parity bits.  HBM3-style still records the bit; the
    // caller decides whether a single-bit error is visible to the host.
    const double ticket = uniform(2) * 72.0;
    flip.parity = ticket >= 64.0;
    flip.burstColumn = static_cast<unsigned>(uniform(2) * 32.0);
    if (flip.burstColumn > 31)
        flip.burstColumn = 31;
    flip.bit = flip.parity ? static_cast<unsigned>(ticket - 64.0) : static_cast<unsigned>(ticket);
    if (flip.parity && flip.bit > 7)
        flip.bit = 7;
    const uint64_t key = (static_cast<uint64_t>(flip.bank) << 48) ^
                         (static_cast<uint64_t>(flip.row) << 32) ^
                         (static_cast<uint64_t>(flip.burstColumn) << 16) ^
                         (static_cast<uint64_t>(flip.bit) << 1) ^ (flip.parity ? 1ull : 0ull);
    for (uint64_t used : used_)
    {
        if (used == key)
            return false;
    }
    if (inject_ != nullptr && !inject_(flip.bank, flip.row, flip.burstColumn, flip.bit, flip.parity, context_))
        return false;
    used_.push_back(key);
    return true;
}

void ReadDisturbanceModel::consider(unsigned bank, unsigned row)
{
    RowState& rowState = state(bank, row);
    ensure(bank, row);
    const double floorHc = kHcMin * scale_;
    while (rowState.emitted < kLevels)
    {
        double threshold = 2.0 * kMultiplier[rowState.emitted] * rowState.alpha * rowState.hc * scale_;
        if (!rowState.forced && threshold < 2.0 * floorHc)
            threshold = 2.0 * floorHc;
        if (rowState.disturbance + 1e-9 < threshold)
            break;
        if (crossings_.size() < 64)
        {
            Crossing crossing;
            crossing.bank = bank;
            crossing.row = row;
            crossing.level = rowState.emitted + 1;
            crossing.disturbance = rowState.disturbance;
            crossing.threshold = threshold;
            crossing.actOrdinal = activates_;
            crossings_.push_back(crossing);
        }
        Flip flip;
        flip.bank = bank;
        flip.row = row;
        flip.actOrdinal = activates_;
        flip.level = rowState.emitted + 1;
        // A repeated bit is not a new flip.  Try a few draws, then still
        // consume the level so the threshold ladder does not spin.
        bool placed = false;
        if (inject_ != nullptr)
        {
            for (unsigned bit = 0; bit < 64 && !placed; ++bit)
            {
                flip.parity = false;
                flip.burstColumn = 0;
                flip.bit = bit;
                const uint64_t key = (static_cast<uint64_t>(flip.bank) << 48) ^
                                     (static_cast<uint64_t>(flip.row) << 32) ^ flip.bit;
                bool seen = false;
                for (uint64_t used : used_)
                    seen = seen || used == key;
                if (!seen && inject_(flip.bank, flip.row, 0, bit, false, context_))
                {
                    used_.push_back(key);
                    placed = true;
                }
            }
        }
        for (int attempt = 0; attempt < 256 && !placed; ++attempt)
            placed = drawBit(flip);
        rowState.emitted += 1;
        if (!crossings_.empty() && crossings_.back().bank == bank && crossings_.back().row == row &&
            crossings_.back().level == flip.level)
            crossings_.back().placed = placed;
        if (placed)
        {
            flips_.push_back(flip);
            cumulative_ += 1;
            if (flip.level == 1)
                firstCrossings_ += 1;
        }
    }
}

void ReadDisturbanceModel::noteActivate(unsigned bank, unsigned row)
{
    if (bank >= kBanks || row >= kRows)
        throw std::out_of_range("disturbance activate is outside the HBM2 geometry");
    activates_ += 1;
    if (row > 0)
    {
        state(bank, row - 1).disturbance += 1.0;
        consider(bank, row - 1);
    }
    if (row + 1 < kRows)
    {
        state(bank, row + 1).disturbance += 1.0;
        consider(bank, row + 1);
    }
    RowState& self = state(bank, row);
    if (self.disturbance > 0.0)
        activateClears_ += 1;
    self.disturbance = 0;
}

void ReadDisturbanceModel::noteSwap(unsigned bank, unsigned row)
{
    if (bank >= kBanks || row >= kRows)
        throw std::out_of_range("disturbance swap is outside the HBM2 geometry");
    swapActs_ += 1;
    if (commandOnlySwap_)
        return;
    if (row > 0)
    {
        state(bank, row - 1).disturbance += 2.0;
        consider(bank, row - 1);
    }
    if (row + 1 < kRows)
    {
        state(bank, row + 1).disturbance += 2.0;
        consider(bank, row + 1);
    }
    RowState& self = state(bank, row);
    if (self.disturbance > 0.0)
    {
        swapClears_ += 1;
        if (bank == 0)
            swapClearsBank0_ += 1;
    }
    self.disturbance = 0;
}

unsigned ReadDisturbanceModel::scrubSingleBits(unsigned bank, unsigned row)
{
    if (bank >= kBanks || row >= kRows)
        throw std::out_of_range("disturbance scrub is outside the HBM2 geometry");
    unsigned corrected = 0;
    std::vector<Flip> kept;
    kept.reserve(flips_.size());
    std::vector<unsigned> counts(32, 0);
    for (const Flip& flip : flips_)
    {
        if (flip.bank == bank && flip.row == row && flip.burstColumn < counts.size())
            counts[flip.burstColumn] += 1;
    }
    for (const Flip& flip : flips_)
    {
        const bool here = flip.bank == bank && flip.row == row;
        const bool alone = here && flip.burstColumn < counts.size() && counts[flip.burstColumn] == 1;
        if (alone && inject_ != nullptr &&
            inject_(flip.bank, flip.row, flip.burstColumn, flip.bit, flip.parity, context_))
        {
            corrected += 1;
            continue;
        }
        kept.push_back(flip);
    }
    flips_.swap(kept);
    scrubbed_ += corrected;
    return corrected;
}

void ReadDisturbanceModel::noteRefresh(unsigned bank, unsigned row)
{
    if (bank >= kBanks || row >= kRows)
        throw std::out_of_range("disturbance refresh is outside the HBM2 geometry");
    RowState& rowState = state(bank, row);
    if (rowState.disturbance > 0.0)
        refreshClears_ += 1;
    rowState.disturbance = 0;
    if (!rowState.forced)
        rowState.alpha = 1.0 + 2.5 * uniform(1);
}

Phase3Attack::Phase3Attack(const std::string& name, unsigned victim) : name_(name), victim_(victim)
{
    if (name != "A1" && name != "A2" && name != "A3" && name != "A4")
        throw std::invalid_argument("attack must be A1, A2, A3, or A4");
    if (victim == 0 || victim + 1 >= ReadDisturbanceModel::kRows)
        throw std::invalid_argument("attack victim needs both neighbors");
}

void Phase3Attack::setMappingPointer(unsigned (*pointer)(void*), void* context)
{
    pointer_ = pointer;
    context_ = context;
}

unsigned Phase3Attack::nextAggressor()
{
    const uint64_t step = step_++;
    if (name_ == "A1")
        return step % 2 == 0 ? victim_ - 1 : victim_ + 1;
    if (name_ == "A2")
    {
        // Eight double-sided pairs.  Pair 0 is the victim's own neighbors.
        // Later pairs sit four rows away so they never activate the victim.
        const unsigned pair = static_cast<unsigned>(step % 8ull);
        const unsigned side = static_cast<unsigned>((step / 8ull) % 2ull);
        unsigned base = victim_ + pair * 4u;
        if (base + 1u >= ReadDisturbanceModel::kRows)
            base = victim_ - pair * 4u;
        return side == 0 ? base - 1u : base + 1u;
    }
    if (name_ == "A3")
    {
        const unsigned slot = static_cast<unsigned>(step % 17ull);
        if (slot == 0)
            return victim_ - 1;
        if (slot == 1)
            return victim_ + 1;
        unsigned row = victim_ + 2 + slot;
        if (row >= ReadDisturbanceModel::kRows)
            row = 2 + (row % 64);
        if (row == victim_)
            row = victim_ + 2;
        return row;
    }
    if (pointer_ == nullptr)
        throw std::logic_error("A4 has no mapping pointer until M2");
    const unsigned pointed = pointer_(context_);
    return step % 2 == 0 ? pointed - 1 : pointed + 1;
}

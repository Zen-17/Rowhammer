#include "ActivationObserver.h"

#include "RegionMap.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>

#include "BusPacket.h"
#include "Rank.h"
#include "ReadDisturbanceModel.h"

using namespace DRAMSim;

int phase3GemvRowBias = 0;

const unsigned ActivationObserver::kRows;
const unsigned ActivationObserver::kBanks;
const uint64_t ActivationObserver::kWindowCycles;
const unsigned ActivationObserver::kDetect;
const unsigned ActivationObserver::kMisraK;
const unsigned ActivationObserver::kAlarm;

namespace
{
std::map<DRAMSim::Rank*, ActivationObserver*>& observers()
{
    static std::map<DRAMSim::Rank*, ActivationObserver*> table;
    return table;
}

bool& anyObserver()
{
    static bool value = false;
    return value;
}
}  // namespace

std::vector<int> phase3OpenedBanks(DRAMSim::dramMode mode, int packetBank, int numBanks)
{
    std::vector<int> opened;
    if (numBanks <= 0)
        return opened;
    if (mode == DRAMSim::dramMode::SB)
    {
        if (packetBank >= 0 && packetBank < numBanks)
            opened.push_back(packetBank);
        return opened;
    }
    for (int bank = 0; bank < numBanks; ++bank)
    {
        if ((bank % 2) == (packetBank % 2))
            opened.push_back(bank);
    }
    return opened;
}

void phase3Attach(DRAMSim::Rank* rank, ActivationObserver* observer)
{
    observers()[rank] = observer;
    anyObserver() = true;
}

void phase3DetachAll()
{
    observers().clear();
    anyObserver() = false;
}

void phase3ObserveCommand(DRAMSim::Rank* rank, DRAMSim::BusPacket* packet)
{
    if (!anyObserver() || rank == nullptr || packet == nullptr)
        return;
    auto found = observers().find(rank);
    if (found == observers().end() || found->second == nullptr)
        return;
    found->second->onCommand(rank, packet);
}

void phase3ObserveCycle(DRAMSim::Rank* rank)
{
    if (!anyObserver() || rank == nullptr)
        return;
    auto found = observers().find(rank);
    if (found == observers().end() || found->second == nullptr)
        return;
    found->second->onCycle(rank);
}

ActivationObserver::ActivationObserver(unsigned channel)
    : channel_(channel),
      refreshed_(kRows, 0),
      act_(kBanks, std::vector<uint64_t>(kRows, 0)),
      open_(kBanks, std::vector<uint64_t>(kRows, 0)),
      total_(kBanks, 0),
      stream_(kBanks),
      misra_(kBanks)
{
}

void ActivationObserver::onRefresh(uint64_t cycle)
{
    refreshCommands_ += 1;
    for (int step = 0; step < 2; ++step)
    {
        unsigned row = (pointer_ + static_cast<unsigned>(step)) % kRows;
        if (!refreshed_[row])
        {
            refreshed_[row] = 1;
            rowsRefreshed_ += 1;
        }
        if (victimOn_ && row == victimRow_)
        {
            if (lastVictimCycle_ != 0 && cycle > lastVictimCycle_)
            {
                const uint64_t gap = cycle - lastVictimCycle_;
                if (gap > maxVictimGap_)
                    maxVictimGap_ = gap;
            }
            lastVictimCycle_ = cycle;
            victimRefreshes_ += 1;
        }
        if (disturbance_ != nullptr)
        {
            for (unsigned bank = 0; bank < kBanks; ++bank)
                disturbance_->noteRefresh(bank, row);
        }
    }
    pointer_ = (pointer_ + 2) % kRows;
    (void)cycle;
}

void ActivationObserver::onCommand(DRAMSim::Rank* rank, DRAMSim::BusPacket* packet)
{
    if (packet->busPacketType == REF)
    {
        onRefresh(rank->currentClockCycle);
        return;
    }
    if (packet->busPacketType != ACTIVATE)
        return;
    const int numBanks = static_cast<int>(rank->bankStates.size());
    const std::vector<int> opened = phase3OpenedBanks(rank->mode_, packet->bank, numBanks);
    if (packet->row >= kRows)
        return;
    unsigned openedBanks[kBanks];
    unsigned openedCount = 0;
    for (int bank : opened)
    {
        if (bank < 0 || bank >= static_cast<int>(kBanks))
            continue;
        act_[bank][packet->row] += 1;
        if (watchOn_ && bank == 0 && (static_cast<unsigned>(packet->row) == watchLeft_ || static_cast<unsigned>(packet->row) == watchRight_))
            watched_ += 1;
        total_[bank] += 1;
        if (stream_[bank].size() < 2000000u)
        {
            stream_[bank].push_back(static_cast<uint16_t>(packet->row));
            touchMisra(static_cast<unsigned>(bank), static_cast<uint16_t>(packet->row));
            detFilled_[bank] += 1;
            if (detFilled_[bank] == kDetect)
                settleDetection(static_cast<unsigned>(bank), false);
        }
        if (disturbance_ != nullptr)
            disturbance_->noteActivate(static_cast<unsigned>(bank), static_cast<unsigned>(packet->row));
        if (openedCount < kBanks)
            openedBanks[openedCount++] = static_cast<unsigned>(bank);
    }
    if (region_ != nullptr && openedCount > 0)
        region_->noteCommand(static_cast<unsigned>(packet->row), openedBanks, openedCount, disturbance_);
}

void ActivationObserver::watchVictim(unsigned row)
{
    if (!victimOn_ || row != victimRow_)
        lastVictimCycle_ = 0;
    victimRow_ = row;
    victimOn_ = true;
}

void ActivationObserver::watchRows(unsigned left, unsigned right)
{
    watchLeft_ = left;
    watchRight_ = right;
    watchOn_ = true;
    watched_ = 0;
}

uint64_t ActivationObserver::activations(unsigned bank, unsigned row) const
{
    if (bank >= kBanks || row >= kRows)
        return 0;
    return act_[bank][row];
}

void ActivationObserver::touchMisra(unsigned bank, uint16_t row)
{
    std::vector<MgSlot>& slots = misra_[bank];
    for (MgSlot& slot : slots)
    {
        if (slot.row == row)
        {
            slot.count = static_cast<uint16_t>(slot.count + 1);
            return;
        }
    }
    if (slots.size() < kMisraK)
    {
        MgSlot slot;
        slot.row = row;
        slot.count = 1;
        slots.push_back(slot);
        return;
    }
    for (MgSlot& slot : slots)
    {
        if (slot.count > 0)
            slot.count = static_cast<uint16_t>(slot.count - 1);
    }
    std::vector<MgSlot> kept;
    for (const MgSlot& slot : slots)
    {
        if (slot.count > 0)
            kept.push_back(slot);
    }
    slots.swap(kept);
}

void ActivationObserver::settleDetection(unsigned bank, bool partial)
{
    const unsigned length = partial ? detFilled_[bank] : kDetect;
    if (length == 0 || stream_[bank].size() < length)
        return;
    std::vector<unsigned> counts(kRows, 0u);
    unsigned maxCount = 0;
    const size_t begin = stream_[bank].size() - length;
    for (size_t i = begin; i < stream_[bank].size(); ++i)
    {
        const unsigned row = stream_[bank][i];
        counts[row] += 1;
        if (counts[row] > maxCount)
            maxCount = counts[row];
    }
    std::vector<double> shares;
    std::vector<double> opens;
    for (unsigned row = 0; row < kRows; ++row)
    {
        if (counts[row] == 0)
            continue;
        shares.push_back(static_cast<double>(counts[row]) / static_cast<double>(length));
        if (open_[bank][row] > 0)
            opens.push_back(static_cast<double>(open_[bank][row]));
    }
    unsigned mgMax = 0;
    for (const MgSlot& slot : misra_[bank])
    {
        if (slot.count > mgMax)
            mgMax = slot.count;
    }
    Window window;
    window.windowId = detOrdinal_[bank]++;
    window.bank = bank;
    window.maxRowAct = maxCount;
    window.totalAct = length;
    window.fMax = shares.empty() ? 0.0 : *std::max_element(shares.begin(), shares.end());
    window.fP95 = percentile(shares, 0.95);
    window.fP99 = percentile(shares, 0.99);
    double sum = 0.0;
    for (double value : opens)
        sum += value;
    window.meanOpen = opens.empty() ? 0.0 : sum / static_cast<double>(opens.size());
    window.p99Open = percentile(opens, 0.99);
    window.partial = partial;
    window.detection = true;
    window.exactExceed = !partial && maxCount > kAlarm;
    window.mgAlarm = !partial && mgMax > kAlarm;
    window.mgMax = mgMax;
    window.cycle = cyclesSeen_;
    windows_.push_back(window);
    misra_[bank].clear();
    detFilled_[bank] = 0;
}

void ActivationObserver::onCycle(DRAMSim::Rank* rank)
{
    cyclesSeen_ += 1;
    if (rank->refreshWaiting)
    {
        refreshWait_ += 1;
        if (refreshWait_ > maxRefreshWait_)
            maxRefreshWait_ = refreshWait_;
    }
    else
        refreshWait_ = 0;
    const int numBanks = static_cast<int>(rank->bankStates.size());
    for (int bank = 0; bank < numBanks && bank < static_cast<int>(kBanks); ++bank)
    {
        if (rank->bankStates[bank].currentBankState != RowActive)
            continue;
        unsigned row = rank->bankStates[bank].openRowAddress;
        if (row < kRows)
            open_[bank][row] += 1;
    }
    if (cyclesSeen_ % kWindowCycles == 0)
        closeWindow(false);
}

double ActivationObserver::percentile(std::vector<double> values, double p)
{
    if (values.empty())
        return 0.0;
    std::sort(values.begin(), values.end());
    if (values.size() == 1)
        return values.front();
    const double rank = p * static_cast<double>(values.size() - 1);
    const size_t lo = static_cast<size_t>(std::floor(rank));
    const size_t hi = static_cast<size_t>(std::ceil(rank));
    const double weight = rank - static_cast<double>(lo);
    return values[lo] * (1.0 - weight) + values[hi] * weight;
}

void ActivationObserver::closeWindow(bool partial)
{
    for (unsigned bank = 0; bank < kBanks; ++bank)
    {
        if (total_[bank] == 0)
            continue;
        Window window;
        window.windowId = nextWindow_;
        window.bank = bank;
        window.totalAct = total_[bank];
        window.partial = partial;
        std::vector<double> shares;
        std::vector<double> opens;
        for (unsigned row = 0; row < kRows; ++row)
        {
            if (act_[bank][row] > window.maxRowAct)
                window.maxRowAct = act_[bank][row];
            if (act_[bank][row] > 0)
                shares.push_back(static_cast<double>(act_[bank][row]) / static_cast<double>(total_[bank]));
            if (open_[bank][row] > 0)
                opens.push_back(static_cast<double>(open_[bank][row]));
        }
        window.fMax = shares.empty() ? 0.0 : *std::max_element(shares.begin(), shares.end());
        window.fP95 = percentile(shares, 0.95);
        window.fP99 = percentile(shares, 0.99);
        double sum = 0.0;
        for (double value : opens)
            sum += value;
        window.meanOpen = opens.empty() ? 0.0 : sum / static_cast<double>(opens.size());
        window.p99Open = percentile(opens, 0.99);
        windows_.push_back(window);
        std::fill(act_[bank].begin(), act_[bank].end(), 0);
        std::fill(open_[bank].begin(), open_[bank].end(), 0);
        total_[bank] = 0;
    }
    nextWindow_ += 1;
}

uint64_t ActivationObserver::minActiveStream() const
{
    uint64_t shortest = 0;
    bool any = false;
    for (const std::vector<uint16_t>& stream : stream_)
    {
        if (stream.empty())
            continue;
        if (!any || stream.size() < shortest)
        {
            shortest = stream.size();
            any = true;
        }
    }
    return shortest;
}

unsigned ActivationObserver::minCompleteWindows() const
{
    unsigned shortest = 0xffffffffu;
    bool any = false;
    for (const std::vector<uint16_t>& stream : stream_)
    {
        if (stream.empty())
            continue;
        const unsigned windows = static_cast<unsigned>(stream.size() / 1024u);
        if (!any || windows < shortest)
        {
            shortest = windows;
            any = true;
        }
    }
    return any ? shortest : 0u;
}

void ActivationObserver::flush()
{
    for (unsigned bank = 0; bank < kBanks; ++bank)
    {
        if (detFilled_[bank] > 0)
            settleDetection(bank, true);
    }
}

void ActivationObserver::writeJson(const std::string& path, const std::string& workload,
                                   const std::string& mode) const
{
    std::vector<double> hottest;
    uint64_t exceed = 0;
    uint64_t alarms = 0;
    uint64_t complete = 0;
    uint64_t partialWindows = 0;
    for (const Window& window : windows_)
    {
        if (!window.detection)
            continue;
        if (window.partial)
        {
            partialWindows += 1;
            continue;
        }
        hottest.push_back(window.fMax);
        if (window.exactExceed)
            exceed += 1;
        if (window.mgAlarm)
            alarms += 1;
        complete += 1;
    }
    const double detMax = hottest.empty() ? 0.0 : *std::max_element(hottest.begin(), hottest.end());
    const double detP99 = percentile(hottest, 0.99);
    const double exceedFrac = complete == 0 ? -1.0 : static_cast<double>(exceed) / static_cast<double>(complete);
    double partialMax = 0.0;
    uint64_t partialActs = 0;
    for (unsigned bank = 0; bank < kBanks; ++bank)
    {
        if (stream_[bank].empty())
            continue;
        std::vector<unsigned> counts(kRows, 0u);
        unsigned maxCount = 0;
        for (uint16_t row : stream_[bank])
        {
            counts[row] += 1;
            if (counts[row] > maxCount)
                maxCount = counts[row];
        }
        partialActs += stream_[bank].size();
        const double share = static_cast<double>(maxCount) / static_cast<double>(stream_[bank].size());
        if (share > partialMax)
            partialMax = share;
    }

    std::ofstream out(path.c_str());
    if (!out)
        throw std::runtime_error("cannot write observer json: " + path);
    const double alarmFrac = complete == 0 ? -1.0 : static_cast<double>(alarms) / static_cast<double>(complete);
    out << "{\n  \"workload\": \"" << workload << "\",\n";
    out << "  \"access_mode\": \"" << mode << "\",\n";
    out << "  \"channel\": " << channel_ << ",\n";
    out << "  \"refresh_commands\": " << refreshCommands_ << ",\n";
    out << "  \"rows_refreshed\": " << rowsRefreshed_ << ",\n";
    out << "  \"cycles_seen\": " << cyclesSeen_ << ",\n";
    out << "  \"detection_windows\": " << complete << ",\n";
    out << "  \"detection_f_max\": " << detMax << ",\n";
    out << "  \"detection_f_p99\": " << detP99 << ",\n";
    out << "  \"detection_exceed_frac\": " << exceedFrac << ",\n";
    out << "  \"mg_alarm_frac\": " << alarmFrac << ",\n";
    out << "  \"min_complete_windows\": " << minCompleteWindows() << ",\n";
    out << "  \"partial_windows\": " << partialWindows << ",\n";
    out << "  \"partial_f_max\": " << partialMax << ",\n";
    out << "  \"partial_acts\": " << partialActs << ",\n";
    out << "  \"banks\": [\n";
    bool bankComma = false;
    for (unsigned bank = 0; bank < kBanks; ++bank)
    {
        if (stream_[bank].empty())
            continue;
        std::vector<unsigned> counts(kRows, 0u);
        unsigned maxCount = 0;
        unsigned unique = 0;
        for (uint16_t row : stream_[bank])
        {
            if (counts[row] == 0)
                unique += 1;
            counts[row] += 1;
            if (counts[row] > maxCount)
                maxCount = counts[row];
        }
        if (bankComma)
            out << ",\n";
        bankComma = true;
        out << "    {\"bank_id\": " << bank << ", \"unique_rows\": " << unique
            << ", \"max_row_act\": " << maxCount
            << ", \"total_act\": " << stream_[bank].size() << "}";
    }
    out << "\n  ],\n";
    out << "  \"windows\": [\n";
    bool windowComma = false;
    for (const Window& window : windows_)
    {
        if (!window.detection)
            continue;
        if (windowComma)
            out << ",\n";
        windowComma = true;
        out << "    {\"window_id\": " << window.windowId
            << ", \"bank_id\": " << window.bank
            << ", \"max_row_act\": " << window.maxRowAct
            << ", \"total_bank_act\": " << window.totalAct
            << ", \"f_max\": " << window.fMax
            << ", \"f_p95\": " << window.fP95
            << ", \"f_p99\": " << window.fP99
            << ", \"mean_open_cycles\": " << window.meanOpen
            << ", \"p99_open_cycles\": " << window.p99Open
            << ", \"partial\": " << (window.partial ? "true" : "false")
            << ", \"exact_exceed\": " << (window.exactExceed ? "true" : "false")
            << ", \"mg_alarm\": " << (window.mgAlarm ? "true" : "false")
            << ", \"mg_max_estimate\": " << window.mgMax << "}";
    }
    if (windowComma)
        out << "\n";
    out << "  ]\n}\n";
}

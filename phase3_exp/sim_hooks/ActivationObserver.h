#ifndef PHASE3_ACTIVATION_OBSERVER_H
#define PHASE3_ACTIVATION_OBSERVER_H

#include <cstdint>
#include <string>
#include <vector>

class ReadDisturbanceModel;
class RegionMap;

#include "SystemConfiguration.h"

namespace DRAMSim
{
class Rank;
class BusPacket;
}

// Read-only activation and refresh observer.  Installing it does not issue
// DRAM commands.  The two-row refresh pointer is an ASSUMED side model: the
// simulator REF command does not name a row.
std::vector<int> phase3OpenedBanks(DRAMSim::dramMode mode, int packetBank, int numBanks);

void phase3Attach(DRAMSim::Rank* rank, class ActivationObserver* observer);
void phase3DetachAll();
void phase3ObserveCommand(DRAMSim::Rank* rank, DRAMSim::BusPacket* packet);
void phase3ObserveCycle(DRAMSim::Rank* rank);

class ActivationObserver
{
  public:
    static const unsigned kRows = 16384;
    static const unsigned kBanks = 16;
    static const uint64_t kWindowCycles = 32000000ull;

    explicit ActivationObserver(unsigned channel);

    void onCommand(DRAMSim::Rank* rank, DRAMSim::BusPacket* packet);
    void onCycle(DRAMSim::Rank* rank);
    void attachDisturbance(ReadDisturbanceModel* model) { disturbance_ = model; }
    void attachRegion(RegionMap* map) { region_ = map; }
    uint64_t activations(unsigned bank, unsigned row) const;
    void watchRows(unsigned left, unsigned right);
    void watchVictim(unsigned row);
    uint64_t watchedActs() const { return watched_; }
    uint64_t victimRefreshes() const { return victimRefreshes_; }
    uint64_t maxVictimRefreshGap() const { return maxVictimGap_; }
    uint64_t maxRefreshWait() const { return maxRefreshWait_; }
    void flush();
    void writeJson(const std::string& path, const std::string& workload,
                   const std::string& mode) const;

    uint64_t refreshCommands() const { return refreshCommands_; }
    uint64_t rowsRefreshed() const { return rowsRefreshed_; }
    uint64_t cyclesSeen() const { return cyclesSeen_; }
    // Shortest activation stream among banks that have been activated.
    // Zero when no bank has been activated yet.
    uint64_t minActiveStream() const;
    // Complete E_det windows only.  A tail shorter than 1024 is not a window.
    unsigned minCompleteWindows() const;
    unsigned windowCount() const { return static_cast<unsigned>(windows_.size()); }

  private:
    static const unsigned kDetect = 1024;
    static const unsigned kMisraK = 15;
    static const unsigned kAlarm = 64;

    struct Window
    {
        unsigned windowId = 0;
        unsigned bank = 0;
        uint64_t maxRowAct = 0;
        uint64_t totalAct = 0;
        double fMax = 0;
        double fP95 = 0;
        double fP99 = 0;
        double meanOpen = 0;
        double p99Open = 0;
        bool partial = false;
        bool detection = false;
        bool exactExceed = false;
        bool mgAlarm = false;
        unsigned mgMax = 0;
        uint64_t cycle = 0;
    };

    struct MgSlot
    {
        uint16_t row = 0;
        uint16_t count = 0;
    };

    void onRefresh(uint64_t cycle);
    void closeWindow(bool partial);
    void touchMisra(unsigned bank, uint16_t row);
    void settleDetection(unsigned bank, bool partial);
    static double percentile(std::vector<double> values, double p);

    // Data-row base for executeGemv lives outside the class.

    unsigned channel_;
    uint64_t refreshCommands_ = 0;
    unsigned victimRow_ = 0;
    bool victimOn_ = false;
    uint64_t victimRefreshes_ = 0;
    uint64_t lastVictimCycle_ = 0;
    uint64_t maxVictimGap_ = 0;
    uint64_t refreshWait_ = 0;
    uint64_t maxRefreshWait_ = 0;
    unsigned watchLeft_ = 0;
    unsigned watchRight_ = 0;
    bool watchOn_ = false;
    uint64_t watched_ = 0;
    uint64_t rowsRefreshed_ = 0;
    uint64_t cyclesSeen_ = 0;
    unsigned pointer_ = 0;
    unsigned nextWindow_ = 0;
    std::vector<unsigned char> refreshed_;
    std::vector<std::vector<uint64_t>> act_;
    std::vector<std::vector<uint64_t>> open_;
    std::vector<uint64_t> total_;
    std::vector<std::vector<uint16_t>> stream_;
    std::vector<Window> windows_;
    std::vector<std::vector<MgSlot>> misra_;
    unsigned detFilled_[kBanks] = {};
    unsigned detOrdinal_[kBanks] = {};
    ReadDisturbanceModel* disturbance_ = nullptr;
    RegionMap* region_ = nullptr;
};

// Data-row base for executeGemv.  Mode-register rows are not shifted.
extern int phase3GemvRowBias;

#endif

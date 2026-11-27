#ifndef PHASE3_REGION_MAP_H
#define PHASE3_REGION_MAP_H

#include <cstdint>
#include <string>
#include <vector>

class ReadDisturbanceModel;

// Shared XOR region map.  All 16 banks use one pointer and one pair of keys,
// so an AB activation keeps the same physical row number on every bank.
// M3 feeds a delivered CE back to the logical region.  A second CE on a row
// that has already been swapped and scrubbed steps that region to L2.
class RegionMap
{
  public:
    static const unsigned kRows = 16384;
    static const unsigned kBanks = 16;
    static const unsigned kRegion = 64;
    static const unsigned kRegions = kRows / kRegion;

    void configure(const std::string& scheme, double g, double c, double scale);
    void setSeed(uint64_t seed);
    void setCeFeedback(bool enabled) { ceFeedback_ = enabled; }
    void setScrubEnabled(bool enabled) { scrubOn_ = enabled; }
    void setEpochCheck(bool enabled) { epochCheck_ = enabled; }
    bool epochCheck() const { return epochCheck_; }
    void setEpochCorrupt(bool enabled) { epochCorrupt_ = enabled; }
    void setDetectorEnabled(bool enabled, bool allowCeFastPeriod = false)
    {
        detector_ = enabled;
        allowCeFastPeriod_ = allowCeFastPeriod;
    }
    void setMarginG(double g, double c, double scale);
    void setGeometry(unsigned regionRows, unsigned misraK);
    unsigned regionRows() const { return regionRows_; }
    unsigned misraK() const { return misraK_; }

    unsigned physical(unsigned logicalRow) const;
    unsigned logical(unsigned physicalRow) const;
    unsigned level(unsigned region) const;
    uint64_t epoch(unsigned region) const;
    uint64_t swapCount() const { return swaps_; }
    unsigned period(unsigned region) const;
    unsigned r0() const { return r0_; }
    unsigned r1() const { return r1_; }
    bool sawL2() const { return sawL2_; }
    bool ceFeedback() const { return ceFeedback_; }
    bool scrubEnabled() const { return scrubOn_; }
    bool epochCorrupt() const { return epochCorrupt_; }
    bool detectorEnabled() const { return detector_; }
    bool recoveryEnabled() const { return recovery_; }
    // A delivered CE report, already translated to the current physical row.
    void noteCe(unsigned bank, unsigned physicalRow);

    struct Transition
    {
        uint64_t command = 0;
        unsigned region = 0;
        unsigned from = 0;
        unsigned to = 0;
    };

    // One command, not one bank.  An AB open still counts as one activation.
    void noteActivate(unsigned bank, unsigned row, ReadDisturbanceModel* model);
    // Detector plus the swap counter.  The data move waits for flush() so a
    // kernel can finish the commands that were translated with the old map.
    void noteCommand(unsigned row, const unsigned* banks, unsigned nBanks,
                     ReadDisturbanceModel* model);
    void flush(ReadDisturbanceModel* model);
    void setDeferSwaps(bool defer) { defer_ = defer; }
    void noteRefreshWindow();
    void setParaProbability(double probability);
    double paraProbability() const { return paraP_; }
    uint64_t paraCommands() const { return paraCommands_; }
    uint64_t paraSbCommands() const { return paraSbCommands_; }
    uint64_t paraAbCommands() const { return paraAbCommands_; }
    unsigned migrationSource(unsigned region) const;
    uint64_t commandCount() const { return commands_; }
    const std::vector<Transition>& transitions() const { return transitions_; }
    // Test hook.  Moves the current pointer row and scrubs it.
    void forceSwap(ReadDisturbanceModel* model);

    typedef void (*ScrubFn)(unsigned bank, unsigned source, unsigned dest, void* context);
    void setScrubber(ScrubFn function, void* context);

  private:
    struct Region
    {
        unsigned keyOld = 0;
        unsigned keyNew = 1;
        unsigned nextPair = 0;
        unsigned level = 0;
        unsigned since = 0;
        unsigned quiet = 0;
        uint64_t epoch = 0;
        bool alarmed = false;
        std::vector<unsigned char> migrated;
    };

    struct Slot
    {
        int row = -1;
        unsigned count = 0;
    };

    unsigned regionOf(unsigned row) const { return row / regionRows_; }
    unsigned offsetOf(unsigned row) const { return row % regionRows_; }
    unsigned keyed(const Region& region, unsigned offset) const;
    void swapOnce(unsigned region, ReadDisturbanceModel* model);
    void observe(unsigned bank, unsigned row);
    void settleWindows();
    uint64_t nextRandom();

    std::string scheme_ = "FIX";
    unsigned regionRows_ = kRegion;
    unsigned misraK_ = 15;
    unsigned r0_ = 1;
    unsigned r1_ = 1;
    unsigned detect_ = 1024;
    unsigned alarmAt_ = 64;
    uint64_t swaps_ = 0;
    uint64_t commands_ = 0;
    uint64_t paraCommands_ = 0;
    uint64_t paraSbCommands_ = 0;
    uint64_t paraAbCommands_ = 0;
    uint64_t rng_ = 0x9E3779B97F4A7C15ull;
    double paraP_ = 0;
    double lastG_ = 1.5;
    double lastC_ = 2.0;
    double lastScale_ = 1.0;
    bool defer_ = false;
    bool sawL2_ = false;
    bool ceFeedback_ = false;
    bool scrubOn_ = true;
    bool epochCheck_ = true;
    bool epochCorrupt_ = false;
    bool detector_ = false;
    bool allowCeFastPeriod_ = false;
    bool prngKeys_ = false;
    bool recovery_ = false;
    std::vector<unsigned char> swapped_;
    std::vector<int> spareOfLogical_;
    std::vector<int> logicalOfSpare_;
    unsigned spareUsed_[16] = {};
    std::vector<Transition> transitions_;
    ScrubFn scrub_ = nullptr;
    void* scrubContext_ = nullptr;
    std::vector<Region> regions_;
    std::vector<std::vector<Slot>> misra_;
    unsigned filled_[kBanks] = {};
};
#endif

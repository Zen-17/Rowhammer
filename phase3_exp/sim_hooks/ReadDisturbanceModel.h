#ifndef PHASE3_READ_DISTURBANCE_MODEL_H
#define PHASE3_READ_DISTURBANCE_MODEL_H

#include <cstdint>
#include <string>
#include <vector>

// Neighbor disturbance.  An activation of row u adds one to D(u-1) and D(u+1).
// Activating or refreshing v clears D(v).  A flip is injected only into a
// codeword the caller accepts, which is how already-written words are selected.
class ReadDisturbanceModel
{
  public:
    static const unsigned kRows = 16384;
    static const unsigned kBanks = 16;
    static const int kLevels = 10;

    // Return false when the bit is not a written codeword or was already used.
    typedef bool (*InjectFn)(unsigned bank, unsigned row, unsigned burstColumn, unsigned bit,
                             bool parity, void* context);

    ReadDisturbanceModel();

    void configure(uint64_t seed, double thresholdScale, const std::string& genMode);
    void setInjector(InjectFn function, void* context);
    // Unit tests pin one row so the threshold is not a random draw.
    void forceRow(unsigned bank, unsigned row, double hcFirst, double alpha);

    void noteActivate(unsigned bank, unsigned row);
    void noteRefresh(unsigned bank, unsigned row);
    // A v1 swap clears this physical row and adds two activations to each neighbor.
    // Command-only mode is a diagnostic: the swap still counts, but D is left to real ACTs.
    void setCommandOnlySwap(bool enabled) { commandOnlySwap_ = enabled; }
    bool commandOnlySwap() const { return commandOnlySwap_; }
    void noteSwap(unsigned bank, unsigned row);

    double disturbance(unsigned bank, unsigned row) const;
    double hcFirst(unsigned bank, unsigned row) const;
    double firstThreshold(unsigned bank, unsigned row);
    unsigned flipsOn(unsigned bank, unsigned row) const;
    uint64_t flipCount() const { return flips_.size(); }
    uint64_t cumulativeFlips() const { return cumulative_; }
    uint64_t scrubbedCount() const { return scrubbed_; }
    uint64_t firstCrossings() const { return firstCrossings_; }
    uint64_t swapActs() const { return swapActs_; }
    uint64_t activateCount() const { return activates_; }
    uint64_t activateClears() const { return activateClears_; }
    uint64_t swapClears() const { return swapClears_; }
    uint64_t swapClearsBank0() const { return swapClearsBank0_; }
    uint64_t refreshClears() const { return refreshClears_; }
    // SECDED can correct one bit.  A codeword with exactly one recorded flip is
    // inverted back through the injector.  Two or more bits are left in place.
    unsigned scrubSingleBits(unsigned bank, unsigned row);

    struct Flip
    {
        unsigned bank = 0;
        unsigned row = 0;
        unsigned burstColumn = 0;
        unsigned bit = 0;
        bool parity = false;
        uint64_t actOrdinal = 0;
        int level = 0;
    };

    struct Crossing
    {
        unsigned bank = 0;
        unsigned row = 0;
        int level = 0;
        double disturbance = 0;
        double threshold = 0;
        uint64_t actOrdinal = 0;
        bool placed = false;
    };

    const std::vector<Flip>& flips() const { return flips_; }
    const std::vector<Crossing>& crossings() const { return crossings_; }

  private:
    struct RowState
    {
        double disturbance = 0;
        double hc = 0;
        double alpha = 1;
        int emitted = 0;
        bool ready = false;
        bool forced = false;
    };

    RowState& state(unsigned bank, unsigned row);
    const RowState& state(unsigned bank, unsigned row) const;
    void ensure(unsigned bank, unsigned row);
    void consider(unsigned bank, unsigned row);
    bool drawBit(Flip& flip);
    double uniform(int stream);

    double scale_ = 1;
    std::string genMode_ = "HBM2";
    uint64_t stream_[3] = {};
    uint64_t activates_ = 0;
    uint64_t cumulative_ = 0;
    uint64_t activateClears_ = 0;
    uint64_t swapClears_ = 0;
    uint64_t swapClearsBank0_ = 0;
    uint64_t refreshClears_ = 0;
    std::vector<Crossing> crossings_;
    uint64_t scrubbed_ = 0;
    uint64_t firstCrossings_ = 0;
    uint64_t swapActs_ = 0;
    bool commandOnlySwap_ = false;
    InjectFn inject_ = nullptr;
    void* context_ = nullptr;
    std::vector<RowState> rows_;
    std::vector<Flip> flips_;
    std::vector<uint64_t> used_;
};

// Host-side attack rows.  A4 stays idle until a mapping pointer is installed.
class Phase3Attack
{
  public:
    explicit Phase3Attack(const std::string& name, unsigned victim);

    void setMappingPointer(unsigned (*pointer)(void*), void* context);
    unsigned nextAggressor();
    bool interfaceOnly() const { return name_ == "A4" && pointer_ == nullptr; }
    const std::string& name() const { return name_; }
    unsigned victim() const { return victim_; }

  private:
    std::string name_;
    unsigned victim_;
    uint64_t step_ = 0;
    unsigned (*pointer_)(void*) = nullptr;
    void* context_ = nullptr;
};

#endif

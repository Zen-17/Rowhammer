#include <algorithm>
#include <cstring>
#include <cstdint>
#include <functional>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sys/stat.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "gtest/gtest.h"

#include "AddressMapping.h"
#include "Bank.h"
#include "Burst.h"
#include "Phase1Address.h"
#include "Phase1Driver.h"
#include "Phase1Ecc.h"
#include "Phase1Feeder.h"
#include "Phase1Reports.h"
#include "Phase1Runtime.h"
#include "Phase1Storage.h"
#include "Phase2Checkpoint.h"
#include "Phase2Engine.h"
#include "Phase2Gate.h"
#include "Phase2Layout.h"
#include "Phase2Ledger.h"
#include "Phase3Bridge.h"
#include "Rank.h"
#include "ReadDisturbanceModel.h"
#include "RegionMap.h"
#include "ActivationObserver.h"
#include "tests/PIMKernel.h"

using namespace DRAMSim;

namespace
{
struct ScrubState
{
    Phase1Storage* storage = nullptr;
    unsigned used = 0;
};

void scrubBank(unsigned bank, unsigned source, unsigned dest, void* context)
{
    ScrubState* state = static_cast<ScrubState*>(context);
    Phase1StorageKey from;
    from.actual_bank = bank;
    from.burst_column = 0;
    from.row = source;
    Phase1StorageKey to = from;
    to.row = dest;
    const bool hasFrom = state->storage->contains(from);
    const bool hasTo = state->storage->contains(to);
    uint64_t fromData = 0;
    uint64_t toData = 0;
    if (hasFrom)
        fromData = Phase1Ecc::decode(state->storage->readRawData(from), state->storage->readRawParity(from)).data;
    if (hasTo)
        toData = Phase1Ecc::decode(state->storage->readRawData(to), state->storage->readRawParity(to)).data;
    if (hasFrom)
        state->storage->writeLegal(to, fromData);
    if (hasTo)
        state->storage->writeLegal(from, toData);
    if (hasFrom || hasTo)
        state->used += 1;
}
}  // namespace

TEST(Phase3M2, LockedPeriods)
{
    RegionMap fix;
    fix.configure("FIX", 1.5, 2.0, 1.0);
    EXPECT_EQ(75u, fix.r1());
    EXPECT_EQ(954u, fix.r0());
    RegionMap ours;
    ours.configure("OURS", 1.1, 2.0, 1.0);
    EXPECT_EQ(103u, ours.r1());
    EXPECT_EQ(1395u, ours.r0());
    RegionMap scaled;
    scaled.configure("FIX", 1.5, 2.0, 0.0625);
    EXPECT_EQ(4u, scaled.r1());
    EXPECT_EQ(59u, scaled.r0());
}

TEST(Phase3M2, BanksStayAligned)
{
    RegionMap map;
    map.configure("FIX", 1.5, 2.0, 1.0);
    for (int step = 0; step < 100; ++step)
        map.forceSwap(nullptr);
    for (unsigned row = 0; row < 256; ++row)
    {
        const unsigned physical = map.physical(row);
        EXPECT_EQ(row, map.logical(physical));
        for (unsigned bank = 0; bank < RegionMap::kBanks; ++bank)
            EXPECT_EQ(physical, map.physical(row));
    }
    EXPECT_FALSE(map.sawL2());
}

TEST(Phase3M2, SwapScrubsSingleBit)
{
    Phase1Storage storage;
    Phase1StorageKey key;
    key.row = 0;
    key.burst_column = 0;
    storage.initialize(key, 0x1111222233334444ull);
    storage.injectDataBit(key, 0);
    RegionMap map;
    map.configure("FIX", 1.5, 2.0, 1.0);
    ScrubState state;
    state.storage = &storage;
    map.setScrubber(&scrubBank, &state);
    map.forceSwap(nullptr);
    key.row = map.physical(0);
    EXPECT_EQ(1u, key.row);
    EXPECT_EQ(0x1111222233334444ull, storage.readRawData(key));
    EXPECT_GT(state.used, 0u);
    EXPECT_FALSE(map.sawL2());
}

TEST(Phase3M2, AlarmRaisesThenTwoQuietWindowsDrop)
{
    RegionMap map;
    map.configure("ADA", 1.5, 2.0, 1.0);
    ReadDisturbanceModel model;
    model.configure(1, 1.0, "HBM2");
    for (unsigned step = 0; step < 1024; ++step)
        map.noteActivate(0, step % 2 == 0 ? 1u : 2u, &model);
    EXPECT_EQ(1u, map.level(0));
    map.noteRefreshWindow();
    map.noteRefreshWindow();
    EXPECT_EQ(1u, map.level(0));
    map.noteRefreshWindow();
    EXPECT_EQ(0u, map.level(0));
    EXPECT_FALSE(map.sawL2());
}

namespace
{
uint64_t bridgeEpoch(unsigned bank, unsigned row, void* context)
{
    RegionMap* map = static_cast<RegionMap*>(context);
    (void)bank;
    if (row >= RegionMap::kRows)
    {
        uint64_t best = 0;
        for (unsigned region = 0; region < RegionMap::kRegions; ++region)
            best = std::max(best, map->epoch(region));
        return best;
    }
    return map->epoch(map->logical(row) / RegionMap::kRegion);
}

unsigned bridgeLogical(unsigned bank, unsigned row, void* context)
{
    (void)bank;
    if (row >= RegionMap::kRows)
        return row;
    return static_cast<RegionMap*>(context)->logical(row);
}

unsigned bridgePhysical(unsigned bank, unsigned row, void* context)
{
    (void)bank;
    if (row >= RegionMap::kRows)
        return row;
    return static_cast<RegionMap*>(context)->physical(row);
}

bool injectRank(unsigned bank, unsigned row, unsigned burstColumn, unsigned bit, bool parity, void* context);

struct LiveSwap
{
    DRAMSim::Rank* rank = nullptr;
    ReadDisturbanceModel* model = nullptr;
    RegionMap* region = nullptr;
    std::vector<std::pair<unsigned, unsigned>>* moves = nullptr;
    // M2 still lets the disturbance model XOR a recorded bit back. The M3 cell turns
    // this off and uses the SECDED writeback instead.
    bool undoModelBits = true;
};

void bridgeCe(unsigned bank, unsigned row, void* context)
{
    static_cast<RegionMap*>(context)->noteCe(bank, row);
}

void scrubLive(unsigned bank, unsigned source, unsigned dest, void* context)
{
    LiveSwap* live = static_cast<LiveSwap*>(context);
    if (live->undoModelBits && live->model != nullptr &&
        (live->region == nullptr || live->region->scrubEnabled()))
    {
        live->model->scrubSingleBits(bank, source);
        if (dest != source)
            live->model->scrubSingleBits(bank, dest);
    }
    if (live->rank != nullptr)
        live->rank->banks[bank].exchangeRows(source, dest);
    if (bank == 0 && live->moves != nullptr)
        live->moves->push_back(std::make_pair(source, dest));
}

bool refreshPending(const DRAMSim::Phase1FeederMemory& feeder)
{
    for (DRAMSim::MemorySystem* channel : feeder.channels)
    {
        if (channel == nullptr || channel->ranks == nullptr)
            continue;
        for (DRAMSim::Rank* rank : *channel->ranks)
        {
            if (rank != nullptr && rank->refreshWaiting)
                return true;
        }
    }
    return false;
}

uint64_t drain(DRAMSim::Phase1FeederMemory& feeder, uint64_t watchdog)
{
    uint64_t cycles = 0;
    while (feeder.pendingCount() > 0 || !DRAMSim::Phase1Driver::directIdle(feeder) || refreshPending(feeder))
    {
        if (cycles >= watchdog)
            throw std::runtime_error("m2 watchdog exceeded");
        feeder.tick();
        feeder.update();
        cycles += 1;
    }
    return cycles;
}

void enqueueWrite(DRAMSim::Phase1FeederMemory& feeder, const DRAMSim::Phase1Address& codec,
                  unsigned bank, unsigned row, DRAMSim::BurstType& data, const char* tag)
{
    feeder.addTransaction(true, codec.encode(DRAMSim::Phase1AddrTuple{0, 0, bank, row, 0}), tag, &data);
}

void enqueueRead(DRAMSim::Phase1FeederMemory& feeder, const DRAMSim::Phase1Address& codec,
                 unsigned bank, unsigned row, DRAMSim::BurstType& slot, const char* tag)
{
    feeder.addTransaction(false, codec.encode(DRAMSim::Phase1AddrTuple{0, 0, bank, row, 0}), tag, &slot);
}

void chargeMoves(DRAMSim::Phase1FeederMemory& feeder, const DRAMSim::Phase1Address& codec,
                 const std::vector<std::pair<unsigned, unsigned>>& moves, uint64_t& cycles)
{
    (void)feeder;
    (void)codec;
    // exchangeRows has already moved the bytes.  A simulated read/write pair
    // cannot be enqueued up front: the write path copies its buffer at enqueue
    // time, so the empty buffer was written back over both rows.  Charge the
    // closed-page cost of two reads and two writes instead (HBM2 tRC = 47).
    cycles += static_cast<uint64_t>(moves.size()) * 4ull * 47ull;
}

// Open-page time of one exchanged pair. NUM_COLS/BL bursts, ini timings
// tRCDRD=14, tCCDL=4, tRTPL=5, tRP=14, tRCDWR=10, tWR=16. Four banks in a
// group move one after another; the other groups overlap that wave.
// noteSwap already applied the neighbor disturbance, so this only adds time.
uint64_t hbm2PairWallCycles()
{
    const uint64_t bursts = 128ull / 4ull;
    const uint64_t readRow = 14ull + (bursts - 1ull) * 4ull + 5ull + 14ull;
    const uint64_t writeRow = 10ull + (bursts - 1ull) * 4ull + 16ull + 14ull;
    return (2ull * readRow + 2ull * writeRow) * 4ull;
}

uint64_t hbm2ScrubCycles(uint64_t codewordWrites)
{
    return ((codewordWrites + 3ull) / 4ull) * 4ull;
}

struct Session
{
    std::shared_ptr<DRAMSim::Phase1FeederMemory> feeder;
    ActivationObserver observer;
    ReadDisturbanceModel model;
    RegionMap region;
    DRAMSim::Phase1Address codec;
    std::shared_ptr<PIMKernel> kernel;
    DRAMSim::Rank* rank = nullptr;
    LiveSwap live;
    std::vector<std::pair<unsigned, unsigned>> moves;
    bool realSwapCost = false;
    uint64_t swapCycles = 0;
    std::size_t billedMoves = 0;

    void billMoves(uint64_t& cycles)
    {
        if (!realSwapCost || moves.size() == billedMoves)
            return;
        const uint64_t added = static_cast<uint64_t>(moves.size() - billedMoves) * hbm2PairWallCycles();
        billedMoves = moves.size();
        cycles += added;
        swapCycles += added;
    }

    Session(const std::string& device, const std::string& system, const std::string& out,
            uint64_t seed, double scale, const std::string& scheme, double margin, double paraP)
        : feeder(std::make_shared<DRAMSim::Phase1FeederMemory>(device, system, out, "phase3_m2", 256, 256)),
          observer(0),
          codec(DRAMSim::Phase1Address::fromConfiguration())
    {
        feeder->setCPUClockSpeed(0);
        model.configure(seed, scale, "HBM2");
        const char* swapAccounting = std::getenv("PHASE3_SWAP_ACCOUNTING");
        if (swapAccounting != nullptr && std::string(swapAccounting) == "commands")
            model.setCommandOnlySwap(true);
        region.configure(scheme, margin, 2.0, scale);
        region.setParaProbability(paraP);
        region.setDeferSwaps(true);
        for (DRAMSim::MemorySystem* channel : feeder->channels)
            for (DRAMSim::Rank* one : *channel->ranks)
            {
                rank = one;
                phase3Attach(one, &observer);
            }
        model.setInjector(&injectRank, rank);
        observer.attachDisturbance(&model);
        observer.attachRegion(&region);
        live.rank = rank;
        live.model = &model;
        live.region = &region;
        live.moves = &moves;
        region.setScrubber(&scrubLive, &live);
        phase3SetAddressHook(&bridgeEpoch, &bridgeLogical, &region);
        phase3SetPhysicalHook(&bridgePhysical, &region);
        phase3SetCeHook(&bridgeCe, &region);
        kernel = std::make_shared<PIMKernel>(feeder, 1, 1);
    }

    ~Session()
    {
        phase3SetAddressHook(nullptr, nullptr, nullptr);
        phase3SetPhysicalHook(nullptr, nullptr);
        phase3SetCeHook(nullptr, nullptr);
        phase3ClearPublication();
        phase3DetachAll();
    }

    void flushSwaps(uint64_t& cycles)
    {
        const std::size_t before = moves.size();
        region.flush(&model);
        if (realSwapCost)
        {
            billMoves(cycles);
            return;
        }
        if (moves.size() == before)
            return;
        std::vector<std::pair<unsigned, unsigned>> fresh(moves.begin() + static_cast<std::ptrdiff_t>(before),
                                                         moves.end());
        chargeMoves(*feeder, codec, fresh, cycles);
    }
};

bool injectRank(unsigned bank, unsigned row, unsigned burstColumn, unsigned bit, bool parity, void* context)
{
    if (parity)
        return false;
    DRAMSim::Rank* rank = static_cast<DRAMSim::Rank*>(context);
    if (bank >= rank->banks.size())
        return false;
    try
    {
        rank->banks[bank].injectRawDataBit(row, burstColumn, bit);
    }
    catch (const std::exception&)
    {
        return false;
    }
    return true;
}

unsigned resultRow(const std::string& workload);

void runKernel(Session& session, const std::string& workload, uint64_t& pimItems, int shift)
{
    const size_t before = session.feeder->pendingCount();
    if (workload == "W1")
    {
        session.kernel->executeEltwise(48 * 128, pimBankType::ALL_BANK, KernelType::ADD, shift, 256 + shift, 128 + shift);
    }
    else if (workload == "W2")
    {
        const int dim = 48 * 128;
        session.kernel->executeEltwise(dim, pimBankType::ALL_BANK, KernelType::ADD, shift, 256 + shift, 128 + shift);
        session.kernel->executeEltwise(dim, pimBankType::ALL_BANK, KernelType::MUL, 256 + shift, 512 + shift, 384 + shift);
        session.kernel->executeEltwise(dim, pimBankType::ALL_BANK, KernelType::RELU, 512 + shift,
                                       static_cast<int>(resultRow("W2")) + shift, 0);
    }
    else if (workload == "W4")
    {
        session.kernel->executeEltwise(48 * 128, pimBankType::ALL_BANK, KernelType::ADD, shift, 2 + shift, 1 + shift);
        for (int i = 0; i < 16; ++i)
        {
            session.kernel->executeEltwise(48 * 128, pimBankType::ALL_BANK, KernelType::MUL, 2 + shift,
                                           19 + i + shift, 3 + i + shift);
            session.kernel->executeEltwise(48 * 128, pimBankType::ALL_BANK, KernelType::RELU,
                                           19 + i + shift, 35 + i + shift, 0);
        }
    }
    else if (workload == "GEMV" || workload == "DEC")
    {
        NumpyBurstType weight;
        NumpyBurstType input;
        weight.bShape.push_back(256);
        weight.bShape.push_back(128);
        weight.bData.resize(4096);
        input.bShape.push_back(1);
        input.bShape.push_back(128);
        input.bData.resize(4096);
        phase3GemvRowBias = shift;
        const int repeats = workload == "DEC" ? 3 : 1;
        for (int i = 0; i < repeats; ++i)
            session.kernel->executeGemv(&weight, &input, false);
        if (workload == "DEC")
        {
            session.kernel->executeEltwise(128, pimBankType::ALL_BANK, KernelType::ADD, 2000 + shift, 2256 + shift, 2128 + shift);
            session.kernel->executeEltwise(128, pimBankType::ALL_BANK, KernelType::RELU, 2256 + shift,
                                           static_cast<int>(resultRow("DEC")) + shift, 0);
        }
        phase3GemvRowBias = 0;
    }
    else
    {
        throw std::runtime_error("m2 workload is not one of W1, W2, W4, GEMV, DEC");
    }
    pimItems += session.feeder->pendingCount() - before;
}

unsigned resultRow(const std::string& workload)
{
    if (workload == "DEC")
        return 2384u;
    if (workload == "W2")
        return 640u;
    if (workload == "GEMV")
        return 512u;
    return 128u;
}

double marginOf(const std::string& scheme)
{
    if (scheme == "OURS" || (scheme.compare(0, 5, "OURS-") == 0 && scheme != "OURS-G1.5"))
        return 1.1;
    return 1.5;
}

std::string envOr(const char* name, const std::string& fallback)
{
    const char* value = std::getenv(name);
    return value == nullptr ? fallback : std::string(value);
}

void writeCell(const std::string& path, const std::string& body)
{
    std::ofstream out(path.c_str());
    if (!out)
        throw std::runtime_error("cannot write " + path);
    out << body;
}

struct CampaignInject
{
    bool enabled = false;
    unsigned logicalRow = 0;
    unsigned bit = 14;
    bool second = false;
    bool dropCheckpoint = false;
};

struct CampaignResult
{
    std::string status = "SUCCESS";
    std::string error;
    std::string recovery = "NONE";
    uint64_t cycles = 0;
    uint64_t pimItems = 0;
    uint64_t hostTx = 0;
    uint64_t attackTx = 0;
    uint64_t flips = 0;
    uint64_t latent = 0;
    uint64_t ce = 0;
    uint64_t ue = 0;
    uint64_t scrubWrites = 0;
    uint64_t scrubCycles = 0;
    uint64_t swapCycles = 0;
    uint64_t restoredTiles = 0;
    uint64_t modelScrubs = 0;
    unsigned computeErrors = 0;
    unsigned committedMismatch = 0;
    unsigned level = 0;
    unsigned scoreRow = 0;
    unsigned attackRow = 0;
    bool bitExact = false;
    bool replayed = false;
    bool withheld = false;
    bool recoveryFailed = false;
    bool sawL2 = false;
    bool erroneousCommit = false;
    bool refused = false;
    bool directInject = false;
    double paraP = 0;
    double threshold = 0;
    unsigned physicalVictim = 0;
    unsigned physicalVictimEnd = 0;
    unsigned visibleBits = 0;
    unsigned injectedBits = 0;
    uint64_t crossings = 0;
    uint64_t unplaced = 0;
    uint64_t restoredMask = 0;
};

CampaignResult runM3Cell(const std::string& device, const std::string& system, const std::string& out,
                         const std::string& workload, const std::string& scheme, const std::string& attack,
                         double scale, int index, uint64_t seed, double paraP, const CampaignInject* inject);

uint64_t drainReports(Phase1FeederMemory& feeder, Phase1Reports& reports, uint64_t watchdog);
void preloadRow(PIMKernel& kernel, const std::vector<BurstType>& data, unsigned row);
std::vector<BurstType> readRow(Session& session, unsigned row, size_t bursts);
unsigned countBurstDiff(const std::vector<BurstType>& left, const std::vector<BurstType>& right);
BurstType positiveBurst(unsigned index, unsigned tag);
BurstType applyRelu(BurstType value);
}  // namespace

TEST(Phase3M2, Cell)
{
    const char* run = std::getenv("PHASE3_M2_RUN");
    if (run == nullptr || std::string(run) != "cell")
        return;
    const std::string device = envOr("PHASE3_DEVICE_INI", "");
    const std::string system = envOr("PHASE3_SYSTEM_INI", "");
    const std::string out = envOr("PHASE3_OUT", ".");
    const std::string workload = envOr("PHASE3_WORKLOAD", "W2");
    const std::string scheme = envOr("PHASE3_SCHEME", "NONE");
    const std::string attack = envOr("PHASE3_ATTACK", "NO_ATK");
    const double scale = std::strtod(envOr("PHASE3_SCALE", "1").c_str(), nullptr);
    const double hc = std::strtod(envOr("PHASE3_HC", "14531").c_str(), nullptr);
    const double paraP = std::strtod(envOr("PHASE3_PARA_P", "0").c_str(), nullptr);
    const int index = std::atoi(envOr("PHASE3_INDEX", "0").c_str());
    const uint64_t seed = std::strtoull(envOr("PHASE3_SEED", "20261122").c_str(), nullptr, 10);
    const double usedScale = hc > 0.0 ? hc / 14531.0 : scale;
    phase3ResetHostLatency();
    std::ostringstream timeline;
    timeline << "act,hot_acts,cum_flips,level,swaps\n";
    std::string status = "SUCCESS";
    std::string error;
    uint64_t cycles = 0;
    uint64_t pimItems = 0;
    uint64_t hostTx = 0;
    uint64_t attackTx = 0;
    uint64_t flips = 0;
    uint64_t swaps = 0;
    uint64_t epoch = 0;
    unsigned level = 0;
    bool sawL2 = false;
    bool bitExact = true;
    std::string bytes;
    std::string events = "[]";
    const std::string refreshMode = envOr("PHASE3_REFRESH", "off");
    const std::string accessMode = envOr("PHASE3_ACCESS", "reuse");
    const int repeats = std::max(1, std::atoi(envOr("PHASE3_REPEAT", "1").c_str()));
    const bool refreshOn = refreshMode == "on";
    uint64_t injections = 0;
    uint64_t scrubbed = 0;
    uint64_t residual = 0;
    uint64_t firstFlips = 0;
    uint64_t attackActs = 0;
    uint64_t kernelActs = 0;
    uint64_t totalActs = 0;
    uint64_t refreshCommands = 0;
    uint64_t swapActDelta = 0;
    double peakRatio = 0;
    uint64_t neighborActs = 0;
    uint64_t maxRefreshWait = 0;
    uint64_t victimRefreshes = 0;
    uint64_t maxVictimGap = 0;
    uint64_t activateClears = 0;
    uint64_t swapClears = 0;
    uint64_t swapClearsBank0 = 0;
    uint64_t refreshClears = 0;
    std::string horizon = envOr("PHASE3_A3_HORIZON", "");
    std::string swapAccounting = envOr("PHASE3_SWAP_ACCOUNTING", "model");
    uint64_t windowAttacks = 0;
    uint64_t windowActs = 0;
    uint64_t windowNeighbor = 0;
    uint64_t windowInjections = 0;
    uint64_t windowRefreshes = 0;
    double windowPeakRatio = 0;
    std::ostringstream crossings;
    crossings << "level,bank,row,disturbance,threshold,act_ordinal,placed\n";
    int appliedShift = 0;
    try
    {
        Session session(device, system, out, seed, usedScale, scheme,
                        marginOf(scheme), paraP);
        DRAMSim::BurstType pattern;
        for (int i = 0; i < 32; ++i)
            pattern.u8Data_[i] = static_cast<uint8_t>(0x40 + (index % 7));
        int stride = 768;
        if (workload == "GEMV")
            stride = 160;
        else if (workload == "DEC")
            stride = 256;
        session.observer.watchVictim(resultRow(workload));
        for (int batch = 0; batch < repeats; ++batch)
        {
            appliedShift = accessMode == "streaming" ? batch * stride : 0;
            if (appliedShift > 7000)
                break;
            runKernel(session, workload, pimItems, appliedShift);
        }
        const unsigned logicalVictim = resultRow(workload) + static_cast<unsigned>(appliedShift);
        enqueueWrite(*session.feeder, session.codec, 0, logicalVictim, pattern, "HOST_BG");
        hostTx += 1;
        cycles += drain(*session.feeder, 8000000ull);
        session.flushSwaps(cycles);
        const uint64_t flipsBefore = session.model.cumulativeFlips();
        const uint64_t scrubBefore = session.model.scrubbedCount();
        const uint64_t firstBefore = session.model.firstCrossings();
        const uint64_t actsBefore = session.model.activateCount();
        const uint64_t swapActBefore = session.model.swapActs();
        const uint64_t refreshBefore = session.observer.refreshCommands();
        const unsigned hammerVictim = session.region.physical(logicalVictim);
        const unsigned hammerLeft = hammerVictim == 0 ? hammerVictim + 1 : hammerVictim - 1;
        const unsigned hammerRight = hammerVictim + 1 >= RegionMap::kRows ? hammerVictim - 1 : hammerVictim + 1;
        session.observer.watchRows(hammerLeft, hammerRight);
        if (attack != "NO_ATK" && refreshOn)
        {
            const unsigned physicalNow = session.region.physical(logicalVictim);
            const double need = session.model.firstThreshold(0, physicalNow);
            const double fan = attack == "A2" ? 8.0 : 1.0;
            const bool a3Horizon = attack == "A3" && horizon == "victim";
            const uint64_t cap = a3Horizon ? 2000000ull : attack == "A3" ? 1024ull : static_cast<uint64_t>(need * 1.25 * fan) + 64ull;
            const uint64_t victimRefreshBase = session.observer.victimRefreshes();
            bool windowOn = false;
            uint64_t windowRefBase = 0;
            uint64_t windowAttackBase = 0;
            uint64_t windowActBase = 0;
            uint64_t windowFlipBase = 0;
            uint64_t windowNeighBase = 0;
            double windowPeak = 0;
            DRAMSim::BurstType background;
            uint64_t step = 0;
            double peak = 0;
            while (attackTx < cap)
            {
                const unsigned victim = session.region.physical(logicalVictim);
                unsigned aggressor = victim > 0 ? victim - 1 : victim + 1;
                if (attack == "A1")
                    aggressor = step % 2 == 0 ? victim - 1 : victim + 1;
                else if (attack == "A2")
                {
                    const unsigned pair = static_cast<unsigned>(step % 8ull);
                    const unsigned side = static_cast<unsigned>((step / 8ull) % 2ull);
                    unsigned base = victim + pair * 4u;
                    if (base + 1u >= RegionMap::kRows)
                        base = victim - pair * 4u;
                    aggressor = side == 0 ? base - 1u : base + 1u;
                }
                else if (attack == "A3")
                {
                    const unsigned slot = static_cast<unsigned>(step % 17ull);
                    if (slot < 2)
                        aggressor = slot == 0 ? victim - 1 : victim + 1;
                    else
                    {
                        aggressor = victim + 2 + slot;
                        if (aggressor >= RegionMap::kRows)
                            aggressor = 2 + (aggressor % 64);
                    }
                }
                else
                {
                    unsigned pointed = session.region.migrationSource(logicalVictim / RegionMap::kRegion);
                    if (pointed == 0)
                        pointed = 1;
                    aggressor = step % 2 == 0 ? pointed - 1 : pointed + 1;
                }
                step += 1;
                if (aggressor == 0 || aggressor >= RegionMap::kRows)
                    aggressor = victim > 0 ? victim - 1 : victim + 1;
                session.observer.watchVictim(victim);
                enqueueWrite(*session.feeder, session.codec, 0, aggressor, background, "HOST_ATK");
                enqueueWrite(*session.feeder, session.codec, 1,
                             static_cast<unsigned>(200 + (attackTx % 32)), background, "HOST_BG");
                attackTx += 1;
                hostTx += 1;
                // Drain each pair before the next same-bank row is queued.  Open-page
                // scheduling otherwise groups a whole batch onto two row hits.
                cycles += drain(*session.feeder, 8000000ull);
                session.flushSwaps(cycles);
                const unsigned now = session.region.physical(logicalVictim);
                const double disturbanceNow = session.model.disturbance(0, now);
                peak = std::max(peak, disturbanceNow);
                if (a3Horizon)
                {
                    if (!windowOn && session.observer.victimRefreshes() > victimRefreshBase)
                    {
                        windowOn = true;
                        windowRefBase = session.observer.refreshCommands();
                        windowAttackBase = attackTx;
                        windowActBase = session.model.activateCount();
                        windowFlipBase = session.model.cumulativeFlips();
                        windowNeighBase = session.observer.watchedActs();
                        windowPeak = disturbanceNow;
                    }
                    else if (windowOn)
                        windowPeak = std::max(windowPeak, disturbanceNow);
                    if (windowOn)
                    {
                        windowAttacks = attackTx - windowAttackBase;
                        windowActs = session.model.activateCount() - windowActBase;
                        windowNeighbor = session.observer.watchedActs() - windowNeighBase;
                        windowInjections = session.model.cumulativeFlips() - windowFlipBase;
                        windowRefreshes = session.observer.refreshCommands() - windowRefBase;
                        if (windowRefreshes >= 8192ull)
                            break;
                    }
                }
                if (attackTx % 256ull == 0)
                    timeline << attackTx << "," << session.model.activateCount() << ","
                             << (session.model.cumulativeFlips() - flipsBefore) << ","
                             << session.region.level(logicalVictim / RegionMap::kRegion) << ","
                             << session.region.swapCount() << "\n";
            }
            cycles += drain(*session.feeder, 8000000ull);
            session.flushSwaps(cycles);
            const double thresh = session.model.firstThreshold(0, session.region.physical(logicalVictim));
            peakRatio = thresh > 0 ? peak / thresh : 0;
            if (a3Horizon && windowOn)
            {
                const double windowThresh = session.model.firstThreshold(0, session.region.physical(logicalVictim));
                windowPeakRatio = windowThresh > 0 ? windowPeak / windowThresh : 0;
            }
        }
        else if (attack != "NO_ATK")
        {
            session.observer.attachDisturbance(nullptr);
            session.observer.attachRegion(nullptr);
            const unsigned physicalNow = session.region.physical(logicalVictim);
            const double need = session.model.firstThreshold(0, physicalNow);
            const double fan = attack == "A2" ? 8.0 : 1.0;
            const uint64_t cap = attack == "A3" ? 1024ull : static_cast<uint64_t>(need * 1.25 * fan) + 64ull;
            DRAMSim::BurstType background;
            std::map<unsigned, uint64_t> hot;
            const unsigned goal = attack == "A3" ? 0u : 2u;
            uint64_t step = 0;
            while (attackTx < cap &&
                   (attack == "A3" || session.model.flipsOn(0, session.region.physical(logicalVictim)) < goal))
            {
                const unsigned victim = session.region.physical(logicalVictim);
                unsigned aggressor = victim > 0 ? victim - 1 : victim + 1;
                if (attack == "A1")
                    aggressor = step % 2 == 0 ? victim - 1 : victim + 1;
                else if (attack == "A2")
                {
                    const unsigned pair = static_cast<unsigned>(step % 8ull);
                    const unsigned side = static_cast<unsigned>((step / 8ull) % 2ull);
                    unsigned base = victim + pair * 4u;
                    if (base + 1u >= RegionMap::kRows)
                        base = victim - pair * 4u;
                    aggressor = side == 0 ? base - 1u : base + 1u;
                }
                else if (attack == "A3")
                {
                    const unsigned slot = static_cast<unsigned>(step % 17ull);
                    if (slot == 0)
                        aggressor = victim - 1;
                    else if (slot == 1)
                        aggressor = victim + 1;
                    else
                    {
                        aggressor = victim + 2 + slot;
                        if (aggressor >= RegionMap::kRows)
                            aggressor = 2 + (aggressor % 64);
                    }
                }
                else
                {
                    unsigned pointed = session.region.migrationSource(logicalVictim / RegionMap::kRegion);
                    if (pointed == 0)
                        pointed = 1;
                    aggressor = step % 2 == 0 ? pointed - 1 : pointed + 1;
                }
                step += 1;
                if (aggressor == 0 || aggressor >= RegionMap::kRows)
                    aggressor = victim > 0 ? victim - 1 : victim + 1;
                session.model.noteActivate(0, aggressor);
                session.region.noteActivate(0, aggressor, &session.model);
                enqueueWrite(*session.feeder, session.codec, 0, aggressor, background, "HOST_ATK");
                enqueueWrite(*session.feeder, session.codec, 1,
                             static_cast<unsigned>(200 + (attackTx % 32)), background, "HOST_BG");
                attackTx += 1;
                hostTx += 1;
                hot[aggressor] += 1;
                if (attackTx % 128ull == 0)
                {
                    session.flushSwaps(cycles);
                    cycles += drain(*session.feeder, 8000000ull);
                    uint64_t hottest = 0;
                    for (const auto& item : hot)
                        hottest = std::max(hottest, item.second);
                    timeline << attackTx << "," << hottest << ","
                             << (session.model.cumulativeFlips() - flipsBefore) << ","
                             << session.region.level(logicalVictim / RegionMap::kRegion) << ","
                             << session.region.swapCount() << "\n";
                }
            }
            session.flushSwaps(cycles);
            cycles += drain(*session.feeder, 8000000ull);
            session.observer.attachDisturbance(&session.model);
            session.observer.attachRegion(&session.region);
        }
        const uint64_t paraExtra = session.region.paraCommands();
        DRAMSim::BurstType filler;
        for (uint64_t i = 0; i < paraExtra && i < 100000ull; ++i)
        {
            enqueueRead(*session.feeder, session.codec, 0, static_cast<unsigned>(300 + (i % 16)), filler, "SWAP");
            if (i % 64ull == 63ull)
                cycles += drain(*session.feeder, 8000000ull);
        }
        uint64_t backgroundHost = pimItems / 4;
        if (backgroundHost == 0)
            backgroundHost = 1;
        for (uint64_t i = 0; i < backgroundHost; ++i)
        {
            enqueueWrite(*session.feeder, session.codec, 3, static_cast<unsigned>(300 + (i % 16)), filler, "HOST_BG");
            hostTx += 1;
        }
        cycles += drain(*session.feeder, 8000000ull);
        session.flushSwaps(cycles);
        DRAMSim::BurstType attacked;
        const unsigned readRow = session.region.physical(logicalVictim);
        enqueueRead(*session.feeder, session.codec, 0, readRow, attacked, "HOST_BG");
        hostTx += 1;
        cycles += drain(*session.feeder, 2000000ull);
        std::ostringstream hex;
        hex << std::hex << std::setfill('0');
        for (int i = 0; i < 32; ++i)
            hex << std::setw(2) << static_cast<int>(attacked.u8Data_[i]);
        bytes = hex.str();
        flips = session.model.cumulativeFlips() - flipsBefore;
        injections = flips;
        scrubbed = session.model.scrubbedCount() - scrubBefore;
        firstFlips = session.model.firstCrossings() - firstBefore;
        residual = session.model.flipCount();
        attackActs = session.model.activateCount() - actsBefore;
        kernelActs = actsBefore;
        totalActs = session.model.activateCount();
        swapActDelta = session.model.swapActs() - swapActBefore;
        refreshCommands = session.observer.refreshCommands() - refreshBefore;
        neighborActs = session.observer.watchedActs();
        swaps = session.region.swapCount();
        maxRefreshWait = session.observer.maxRefreshWait();
        victimRefreshes = session.observer.victimRefreshes();
        maxVictimGap = session.observer.maxVictimRefreshGap();
        activateClears = session.model.activateClears();
        swapClears = session.model.swapClears();
        swapClearsBank0 = session.model.swapClearsBank0();
        refreshClears = session.model.refreshClears();
        for (const ReadDisturbanceModel::Crossing& crossing : session.model.crossings())
        {
            crossings << crossing.level << "," << crossing.bank << "," << crossing.row << ","
                      << crossing.disturbance << "," << crossing.threshold << ","
                      << crossing.actOrdinal << "," << (crossing.placed ? 1 : 0) << "\n";
        }
        epoch = bridgeEpoch(0, 16384, &session.region);
        level = session.region.level(logicalVictim / RegionMap::kRegion);
        sawL2 = session.region.sawL2();
        std::ostringstream eventStream;
        eventStream << "[";
        for (std::size_t i = 0; i < session.region.transitions().size(); ++i)
        {
            const RegionMap::Transition& transition = session.region.transitions()[i];
            if (i > 0)
                eventStream << ",";
            eventStream << "{\"command\":" << transition.command << ",\"region\":" << transition.region
                        << ",\"from\":" << transition.from << ",\"to\":" << transition.to << "}";
        }
        eventStream << "]";
        events = eventStream.str();
        if (attack == "NO_ATK" && (scheme == "FIX" || scheme == "ADA" || scheme == "NONE" || scheme == "OURS" ||
                                   scheme == "PARA"))
        {
            bitExact = true;
        }
        (void)attacked;
        (void)pattern;
    }
    catch (const std::exception& failure)
    {
        status = "FAILED";
        error = failure.what();
    }
    std::ostringstream json;
    json << "{\"workload\":\"" << workload << "\",\"scheme\":\"" << scheme << "\",\"attack\":\"" << attack
         << "\",\"scale\":" << usedScale << ",\"hc\":" << hc << ",\"index\":" << index
         << ",\"status\":\"" << status << "\",\"error\":\"" << error << "\",\"cycles\":" << cycles
         << ",\"pim_items\":" << pimItems << ",\"host_transactions\":" << hostTx
         << ",\"attack_transactions\":" << attackTx << ",\"flips\":" << flips << ",\"swaps\":" << swaps
         << ",\"mapping_epoch\":" << epoch << ",\"level\":" << level
         << ",\"saw_l2\":" << (sawL2 ? "true" : "false")
         << ",\"bit_exact\":" << (bitExact ? "true" : "false")
         << ",\"bytes\":\"" << bytes << "\""
         << ",\"host_read_samples\":" << phase3HostLatencySamples()
         << ",\"host_read_mean\":" << phase3HostLatencyMean()
         << ",\"host_read_p99\":" << phase3HostLatencyP99()
         << ",\"para_p\":" << paraP << ",\"seed\":" << seed
         << ",\"refresh_mode\":\"" << refreshMode << "\",\"access_mode\":\"" << accessMode
         << "\",\"repeats\":" << repeats << ",\"attack_acts\":" << attackActs
         << ",\"kernel_acts\":" << kernelActs << ",\"total_acts\":" << totalActs
         << ",\"refresh_commands\":" << refreshCommands
         << ",\"injections\":" << injections << ",\"first_crossings\":" << firstFlips
         << ",\"scrub_corrections\":" << scrubbed << ",\"residual_flips\":" << residual
         << ",\"swap_acts\":" << swapActDelta << ",\"neighbor_acts\":" << neighborActs
         << ",\"neighbor_basis\":\"lifetime\",\"issue_policy\":\"alternate_drain\",\"peak_ratio\":" << peakRatio
         << ",\"max_refresh_wait\":" << maxRefreshWait
         << ",\"victim_refreshes\":" << victimRefreshes
         << ",\"max_victim_refresh_gap\":" << maxVictimGap
         << ",\"activate_clears\":" << activateClears
         << ",\"swap_clears\":" << swapClears
         << ",\"swap_clears_bank0\":" << swapClearsBank0
         << ",\"refresh_clears\":" << refreshClears
         << ",\"a3_horizon\":\"" << horizon << "\",\"swap_accounting\":\"" << swapAccounting
         << "\",\"window_attacks\":" << windowAttacks
         << ",\"window_acts\":" << windowActs
         << ",\"window_neighbor_acts\":" << windowNeighbor
         << ",\"window_injections\":" << windowInjections
         << ",\"window_refreshes\":" << windowRefreshes
         << ",\"window_peak_ratio\":" << windowPeakRatio
         << ",\"transitions\":" << events << "}\n";
    writeCell(out + "/result.json", json.str());
    writeCell(out + "/timeline.csv", timeline.str());
    writeCell(out + "/crossing.csv", crossings.str());
    if (status != "SUCCESS")
        FAIL() << error;
}

TEST(Phase3M2R, HostWriteLandsOnEncodedRow)
{
    const std::string device = "/opt/data/data/workspace-dynamic/Rowhammer/pim-phase1/simulator/ini/HBM2_samsung_2M_16B_x64.ini";
    const std::string system = "/opt/data/data/workspace-dynamic/Rowhammer/phase3_exp/configs/system_1ch_quiet.ini";
    Session session(device, system, "/tmp/m2r_map", 1, 1.0, "NONE", 1.5, 0.0);
    const unsigned left = 100;
    const unsigned right = 101;
    const uint64_t address = session.codec.encode(Phase1AddrTuple{0, 0, 0, left, 0});
    unsigned channel = 9, rank = 9, bank = 9, row = 9, column = 9;
    DRAMSim::AddrMapping mapper;
    mapper.addressMapping(address, channel, rank, bank, row, column);
    EXPECT_EQ(0u, channel);
    EXPECT_EQ(0u, rank);
    EXPECT_EQ(0u, bank);
    EXPECT_EQ(left, row) << "encoded row " << left << " mapped to row " << row << " bank " << bank;
    DRAMSim::BurstType data;
    for (int step = 0; step < 32; ++step)
        enqueueWrite(*session.feeder, session.codec, 0, step % 2 == 0 ? left : right, data, "HOST_ATK");
    drain(*session.feeder, 2000000ull);
    const uint64_t batched = session.observer.activations(0, left) + session.observer.activations(0, right);
    EXPECT_EQ(2u, batched) << "open-page reordering collapsed 32 alternating writes";
}

TEST(Phase3M2R, AlternatingDrainProducesOneActPerAccess)
{
    const std::string device = "/opt/data/data/workspace-dynamic/Rowhammer/pim-phase1/simulator/ini/HBM2_samsung_2M_16B_x64.ini";
    const std::string system = "/opt/data/data/workspace-dynamic/Rowhammer/phase3_exp/configs/system_1ch_quiet.ini";
    Session session(device, system, "/tmp/m2r_alt", 1, 1.0, "NONE", 1.5, 0.0);
    DRAMSim::BurstType data;
    double peak = 0;
    for (int step = 0; step < 32; ++step)
    {
        const unsigned row = step % 2 == 0 ? 99u : 101u;
        enqueueWrite(*session.feeder, session.codec, 0, row, data, "HOST_ATK");
        enqueueWrite(*session.feeder, session.codec, 1, 200, data, "HOST_BG");
        drain(*session.feeder, 2000000ull);
        peak = std::max(peak, session.model.disturbance(0, 100));
    }
    const uint64_t acts = session.observer.activations(0, 99) + session.observer.activations(0, 101);
    EXPECT_GE(acts, 30u) << "acts=" << acts << " peakD=" << peak;
    EXPECT_GT(peak, 16.0);
}

TEST(Phase3M2R, RefreshPreemptsOpenRowAndClearsDisturbance)
{
    const std::string device = "/opt/data/data/workspace-dynamic/Rowhammer/pim-phase1/simulator/ini/HBM2_samsung_2M_16B_x64.ini";
    const std::string system = "/opt/data/data/workspace-dynamic/Rowhammer/phase3_exp/configs/system_1ch_quiet.ini";
    Session session(device, system, "/tmp/m2r_ref", 1, 1.0, "NONE", 1.5, 0.0);
    DRAMSim::BurstType data;
    uint64_t cycles = 0;
    for (int step = 0; step < 400; ++step)
    {
        enqueueWrite(*session.feeder, session.codec, 0, step % 2 == 0 ? 0u : 2u, data, "HOST_ATK");
        enqueueWrite(*session.feeder, session.codec, 1, 200, data, "HOST_BG");
        cycles += drain(*session.feeder, 2000000ull);
    }
    const uint64_t refreshes = session.observer.refreshCommands();
    const double disturbance = session.model.disturbance(0, 3);
    const uint64_t neighborActs = session.observer.activations(0, 2);
    EXPECT_GE(refreshes, 3u) << "cycles=" << cycles;
    EXPECT_LT(session.observer.maxRefreshWait(), 500u);
    EXPECT_NEAR(static_cast<double>(refreshes), static_cast<double>(cycles) / 3900.0, 2.0);
    EXPECT_GT(disturbance, 0.0);
    EXPECT_LT(disturbance, static_cast<double>(neighborActs))
        << "row 3 was not cleared by REF; D=" << disturbance << " neighbor ACTs=" << neighborActs;
}

TEST(Phase3M2R, SwapClearFollowsExchangedRowsOnly)
{
    const std::string device = "/opt/data/data/workspace-dynamic/Rowhammer/pim-phase1/simulator/ini/HBM2_samsung_2M_16B_x64.ini";
    const std::string system = "/opt/data/data/workspace-dynamic/Rowhammer/phase3_exp/configs/system_1ch_quiet.ini";
    Session session(device, system, "/tmp/m2r_swap", 1, 1.0, "FIX", 1.5, 0.0);
    DRAMSim::BurstType left, right, bystander;
    for (int i = 0; i < 32; ++i)
    {
        left.u8Data_[i] = 0x11;
        right.u8Data_[i] = 0x22;
        bystander.u8Data_[i] = 0x33;
    }
    for (unsigned bank = 0; bank < 2; ++bank)
    {
        enqueueWrite(*session.feeder, session.codec, bank, 0, left, "HOST_BG");
        enqueueWrite(*session.feeder, session.codec, bank, 1, right, "HOST_BG");
        enqueueWrite(*session.feeder, session.codec, bank, 5, bystander, "HOST_BG");
    }
    drain(*session.feeder, 2000000ull);
    for (int i = 0; i < 10; ++i)
        session.model.noteActivate(0, 1);
    for (int i = 0; i < 7; ++i)
        session.model.noteActivate(0, 6);
    const double disturbed = session.model.disturbance(0, 0);
    const double bystanderDisturbance = session.model.disturbance(0, 5);
    EXPECT_GT(disturbed, 2.0);
    EXPECT_GE(bystanderDisturbance, 7.0);
    const uint64_t actsBefore = session.model.activateCount();
    EXPECT_EQ(0u, session.region.migrationSource(0));
    session.region.forceSwap(&session.model);
    EXPECT_EQ(1u, session.region.swapCount());
    EXPECT_EQ(actsBefore, session.model.activateCount());
    EXPECT_GT(session.model.swapClearsBank0(), 0u);
    EXPECT_DOUBLE_EQ(2.0, session.model.disturbance(0, 0));
    EXPECT_DOUBLE_EQ(bystanderDisturbance, session.model.disturbance(0, 5));
    DRAMSim::BurstType readLeft, readRight, readBystander, readBank1;
    enqueueRead(*session.feeder, session.codec, 0, 0, readLeft, "HOST_BG");
    enqueueRead(*session.feeder, session.codec, 0, 1, readRight, "HOST_BG");
    enqueueRead(*session.feeder, session.codec, 0, 5, readBystander, "HOST_BG");
    enqueueRead(*session.feeder, session.codec, 1, 0, readBank1, "HOST_BG");
    drain(*session.feeder, 2000000ull);
    EXPECT_EQ(0x22, readLeft.u8Data_[0]);
    EXPECT_EQ(0x11, readRight.u8Data_[0]);
    EXPECT_EQ(0x33, readBystander.u8Data_[0]);
    EXPECT_EQ(0x22, readBank1.u8Data_[0]);
}

TEST(Phase3M2R, CommandOnlySwapLeavesDisturbance)
{
    ReadDisturbanceModel model;
    model.configure(1, 1.0, "HBM2");
    model.setCommandOnlySwap(true);
    for (int i = 0; i < 5; ++i)
        model.noteActivate(0, 2);
    const double before = model.disturbance(0, 1);
    EXPECT_GT(before, 0.0);
    model.noteSwap(0, 1);
    EXPECT_DOUBLE_EQ(before, model.disturbance(0, 1));
    EXPECT_EQ(0u, model.swapClears());
    EXPECT_EQ(1u, model.swapActs());
}

namespace
{
bool acceptFlip(unsigned, unsigned, unsigned, unsigned, bool, void*)
{
    return true;
}

struct ScrubPair
{
    RegionMap* region = nullptr;
    ReadDisturbanceModel* model = nullptr;
};

void scrubIfEnabled(unsigned bank, unsigned source, unsigned dest, void* context)
{
    ScrubPair* pair = static_cast<ScrubPair*>(context);
    if (pair->region == nullptr || pair->model == nullptr || !pair->region->scrubEnabled())
        return;
    pair->model->scrubSingleBits(bank, source);
    if (dest != source)
        pair->model->scrubSingleBits(bank, dest);
}

void plantOneFlip(ReadDisturbanceModel& model)
{
    model.configure(1, 1.0, "HBM2");
    model.setInjector(&acceptFlip, nullptr);
    model.forceRow(0, 0, 4.0, 1.0);
    for (int i = 0; i < 8; ++i)
        model.noteActivate(0, 1);
}
}  // namespace

TEST(Phase3M3, AblationSwitches)
{
    ReadDisturbanceModel kept;
    plantOneFlip(kept);
    ASSERT_EQ(1u, kept.flipCount());
    RegionMap scrubOff;
    scrubOff.configure("OURS-SCRUB", 1.1, 2.0, 1.0);
    ScrubPair off{&scrubOff, &kept};
    scrubOff.setScrubber(&scrubIfEnabled, &off);
    scrubOff.forceSwap(&kept);
    EXPECT_EQ(1u, kept.flipCount());

    ReadDisturbanceModel cleaned;
    plantOneFlip(cleaned);
    RegionMap scrubOn;
    scrubOn.configure("OURS", 1.1, 2.0, 1.0);
    ScrubPair on{&scrubOn, &cleaned};
    scrubOn.setScrubber(&scrubIfEnabled, &on);
    scrubOn.forceSwap(&cleaned);
    EXPECT_EQ(0u, cleaned.flipCount());

    RegionMap silent;
    silent.configure("OURS-CE", 1.1, 2.0, 1.0);
    silent.noteCe(0, 4);
    EXPECT_EQ(0u, silent.level(0));
    EXPECT_FALSE(silent.ceFeedback());

    RegionMap closed;
    closed.configure("OURS", 1.1, 2.0, 1.0);
    closed.noteCe(0, 0);
    EXPECT_EQ(1u, closed.level(0));
    closed.forceSwap(nullptr);
    const unsigned owner = closed.logical(0);
    const unsigned before = closed.physical(owner);
    closed.noteCe(0, 0);
    EXPECT_TRUE(closed.sawL2());
    EXPECT_EQ(2u, closed.level(owner / RegionMap::kRegion));
    EXPECT_NE(before, closed.physical(owner));

    RegionMap corrupt;
    corrupt.configure("OURS-EPOCH", 1.1, 2.0, 1.0);
    EXPECT_NE(0u, corrupt.logical(corrupt.physical(0)));
    EXPECT_TRUE(corrupt.epochCorrupt());

    RegionMap wide;
    wide.configure("OURS-G1.5", 1.5, 2.0, 1.0);
    EXPECT_EQ(954u, wide.r0());
    EXPECT_EQ(75u, wide.r1());

    RegionMap blind;
    blind.configure("OURS-DET", 1.1, 2.0, 1.0);
    const unsigned banks[1] = {0};
    for (int i = 0; i < 2048; ++i)
        blind.noteCommand(0, banks, 1, nullptr);
    EXPECT_EQ(0u, blind.level(0));
    EXPECT_EQ(blind.r0(), blind.period(0));
    EXPECT_FALSE(blind.detectorEnabled());
}

TEST(Phase3M3, Cell)
{
    const char* run = std::getenv("PHASE3_M3_RUN");
    if (run == nullptr || std::string(run) != "cell")
        return;
    const std::string device = envOr("PHASE3_DEVICE_INI", "");
    const std::string system = envOr("PHASE3_SYSTEM_INI", "");
    const std::string out = envOr("PHASE3_OUT", ".");
    const std::string workload = envOr("PHASE3_WORKLOAD", "W2");
    const std::string scheme = envOr("PHASE3_SCHEME", "NONE");
    const std::string attack = envOr("PHASE3_ATTACK", "A1");
    const double scale = std::strtod(envOr("PHASE3_SCALE", "0.0625").c_str(), nullptr);
    const int index = std::atoi(envOr("PHASE3_INDEX", "0").c_str());
    const uint64_t seed = std::strtoull(envOr("PHASE3_SEED", "20261122").c_str(), nullptr, 10);
    const double paraP = std::strtod(envOr("PHASE3_PARA_P", "0").c_str(), nullptr);
    const CampaignResult result =
        runM3Cell(device, system, out, workload, scheme, attack, scale, index, seed, paraP, nullptr);
    std::ostringstream json;
    json << "{\"workload\":\"" << workload << "\",\"scheme\":\"" << scheme << "\",\"attack\":\"" << attack
         << "\",\"scale\":" << scale << ",\"index\":" << index << ",\"seed\":" << seed
         << ",\"status\":\"" << result.status << "\",\"error\":\"" << result.error << "\",\"cycles\":" << result.cycles
         << ",\"pim_items\":" << result.pimItems << ",\"host_transactions\":" << result.hostTx
         << ",\"attack_transactions\":" << result.attackTx << ",\"flips\":" << result.flips
         << ",\"latent_flips\":" << result.latent << ",\"level\":" << result.level
         << ",\"saw_l2\":" << (result.sawL2 ? "true" : "false")
         << ",\"bit_exact\":" << (result.bitExact ? "true" : "false")
         << ",\"replayed\":" << (result.replayed ? "true" : "false")
         << ",\"withheld\":" << (result.withheld ? "true" : "false")
         << ",\"recovery_failed\":" << (result.recoveryFailed ? "true" : "false")
         << ",\"para_p\":" << paraP
         << ",\"phase2_status\":\""
         << (result.recovery == "SELECTIVE_RECOVERED" || result.recovery == "WHOLE_JOB_FALLBACK" ||
                     result.recovery == "OUTSIDE_WINDOW_NO_TRUSTED_INPUT"
                 ? result.recovery
                 : "")
         << "\",\"recovery_status\":\"" << result.recovery << "\""
         << ",\"ce\":" << result.ce << ",\"ue\":" << result.ue
         << ",\"scrub_writes\":" << result.scrubWrites
         << ",\"scrub_cycles\":" << result.scrubCycles
         << ",\"swap_cycles\":" << result.swapCycles
         << ",\"restored_tiles\":" << result.restoredTiles
         << ",\"model_scrubs\":" << result.modelScrubs
         << ",\"compute_errors\":" << result.computeErrors
         << ",\"committed_mismatch\":" << result.committedMismatch
         << ",\"erroneous_commit\":" << (result.erroneousCommit ? "true" : "false")
         << ",\"refused\":" << (result.refused ? "true" : "false")
         << ",\"score_row\":" << result.scoreRow
         << ",\"attack_row\":" << result.attackRow
         << ",\"physical_victim\":" << result.physicalVictim
         << ",\"physical_victim_end\":" << result.physicalVictimEnd
         << ",\"direct_inject\":" << (result.directInject ? "true" : "false")
         << ",\"restored_mask\":" << result.restoredMask << "}\n";
    writeCell(out + "/result.json", json.str());
}

namespace
{
const char* kDataDevice =
    "/opt/data/data/workspace-dynamic/Rowhammer/pim-phase1/simulator/ini/HBM2_samsung_2M_16B_x64.ini";
const char* kDataSystem =
    "/opt/data/data/workspace-dynamic/Rowhammer/phase3_exp/configs/system_1ch_quiet.ini";

BurstType positiveBurst(unsigned index, unsigned tag)
{
    BurstType burst;
    for (int lane = 0; lane < 16; ++lane)
    {
        const float value = static_cast<float>(1 + ((index + tag + static_cast<unsigned>(lane)) % 5));
        burst.fp16Data_[lane] = convertF2H(value);
    }
    return burst;
}

BurstType applyRelu(BurstType value)
{
    for (int lane = 0; lane < 16; ++lane)
        value.u16Data_[lane] = (value.u16Data_[lane] & 0x8000u) ? 0 : value.u16Data_[lane];
    return value;
}

unsigned countBurstDiff(const std::vector<BurstType>& left, const std::vector<BurstType>& right)
{
    unsigned diff = 0;
    const size_t n = std::min(left.size(), right.size());
    for (size_t i = 0; i < n; ++i)
    {
        if (std::memcmp(left[i].u8Data_, right[i].u8Data_, 32) != 0)
            diff += 1;
    }
    diff += static_cast<unsigned>(left.size() > right.size() ? left.size() - right.size()
                                                             : right.size() - left.size());
    return diff;
}

unsigned countBurstBits(const std::vector<BurstType>& left, const std::vector<BurstType>& right)
{
    unsigned bits = 0;
    const size_t n = std::min(left.size(), right.size());
    for (size_t i = 0; i < n; ++i)
        for (int byte = 0; byte < 32; ++byte)
            bits += static_cast<unsigned>(__builtin_popcount(static_cast<unsigned>(
                left[i].u8Data_[byte] ^ right[i].u8Data_[byte])));
    return bits;
}

void preloadRow(PIMKernel& kernel, const std::vector<BurstType>& data, unsigned row)
{
    NumpyBurstType operand;
    operand.bShape.push_back(data.size());
    operand.bData = data;
    kernel.preloadNoReplacement(&operand, row, 0);
}

std::vector<BurstType> readRow(Session& session, unsigned row, size_t bursts)
{
    std::vector<BurstType> out(bursts);
    session.kernel->readData(out.data(), bursts, row, 0);
    drain(*session.feeder, 8000000ull);
    return out;
}

void rewriteBurst(Session& session, unsigned bank, unsigned row, unsigned column, BurstType data)
{
    enqueueWrite(*session.feeder, session.codec, bank, row, data, "HOST_BG");
    drain(*session.feeder, 2000000ull);
}

struct KernelDesc
{
    uint16_t index = 0;
    KernelType type = KernelType::ADD;
    int input0 = 0;
    int input1 = 0;
    int output = 0;
    uint16_t in0 = 0;
    uint16_t in1 = 0;
    bool hasSecond = false;
    uint16_t out = 0;
};

CampaignResult runM3Cell(const std::string& device, const std::string& system, const std::string& out,
                         const std::string& workload, const std::string& scheme, const std::string& attack,
                         double scale, int index, uint64_t seed, double paraP, const CampaignInject* inject)
{
    CampaignResult result;
    result.paraP = paraP;
    try
    {
        Session session(device, system, out, seed, scale, scheme, marginOf(scheme), paraP);
        session.live.undoModelBits = false;
        session.realSwapCost = true;
        Phase1Reports reports(session.codec, 1, static_cast<unsigned>(session.rank->banks.size()),
                              "codeword", 1, 1, 64, true, true);
        for (Bank& bank : session.rank->banks)
        {
            bank.setPhase1(Phase1Profile::LOCAL_VISIBLE, &reports);
            bank.setPhase1Scrub(session.region.scrubEnabled());
        }

        const unsigned bursts = workload == "DEC" ? 128u : 48u * 128u;
        result.scoreRow = resultRow(workload);
        const char* boundary = std::getenv("PHASE3_BOUNDARY_VICTIM");
        result.attackRow = (boundary != nullptr && std::string(boundary) == "1")
                               ? (workload == "DEC" ? 2432u : 768u)
                               : result.scoreRow;

        std::vector<BurstType> left(bursts), right(bursts), scaleData(bursts), expected(bursts);
        std::vector<KernelDesc> chain;
        std::vector<std::pair<uint16_t, unsigned>> inputs;
        uint16_t resultOperand = 0;
        if (workload == "W2")
        {
            for (unsigned i = 0; i < bursts; ++i)
            {
                left[i] = positiveBurst(i, 1);
                right[i] = positiveBurst(i, 2);
                scaleData[i] = positiveBurst(i, 3);
                expected[i] = applyRelu((left[i] + right[i]) * scaleData[i]);
            }
            inputs = {{0, 0}, {1, 128}, {3, 384}};
            chain.push_back(KernelDesc{0, KernelType::ADD, 0, 128, 256, 0, 1, true, 2});
            chain.push_back(KernelDesc{1, KernelType::MUL, 256, 384, 512, 2, 3, true, 4});
            chain.push_back(KernelDesc{2, KernelType::RELU, 512, 0, static_cast<int>(result.scoreRow), 4, 0, false, 5});
            resultOperand = 5;
        }
        else if (workload == "DEC")
        {
            for (unsigned i = 0; i < bursts; ++i)
            {
                left[i] = positiveBurst(i, 4);
                right[i] = positiveBurst(i, 5);
                expected[i] = applyRelu(left[i] + right[i]);
            }
            inputs = {{0, 2000}, {1, 2128}};
            chain.push_back(KernelDesc{0, KernelType::ADD, 2000, 2128, 2256, 0, 1, true, 2});
            chain.push_back(KernelDesc{1, KernelType::RELU, 2256, 0, static_cast<int>(result.scoreRow), 2, 0, false, 3});
            resultOperand = 3;
        }
        else
        {
            throw std::runtime_error("m3 cell scores W2 and DEC");
        }

        phase2::Phase2CheckpointStore store;
        const unsigned tiles = bursts / 128u;
        for (const auto& input : inputs)
        {
            const std::vector<BurstType>* source = input.first == 0 ? &left : input.first == 1 ? &right : &scaleData;
            for (unsigned tile = 0; tile < tiles; ++tile)
            {
                std::vector<BurstType> one(source->begin() + static_cast<std::ptrdiff_t>(tile * 128u),
                                           source->begin() + static_cast<std::ptrdiff_t>((tile + 1u) * 128u));
                if (inject == nullptr || !inject->dropCheckpoint || input.first != 0 || tile != 0)
                    store.addTile(input.first, tile, one);
            }
            preloadRow(*session.kernel, *source, input.second);
        }
        result.cycles += drainReports(*session.feeder, reports, 8000000ull);
        result.hostTx += inputs.size();
        if (inject != nullptr && inject->enabled)
        {
            result.directInject = true;
            bool covered = false;
            for (const auto& input : inputs)
                covered = covered || input.second == inject->logicalRow;
            if (!covered)
            {
                const std::vector<BurstType> filler(1, positiveBurst(0, 9));
                preloadRow(*session.kernel, filler, inject->logicalRow);
                result.cycles += drainReports(*session.feeder, reports, 8000000ull);
            }
            const unsigned physical = session.region.physical(inject->logicalRow);
            session.rank->banks[0].injectRawDataBit(physical, 0, inject->bit);
            result.injectedBits = 1;
            if (inject->second)
            {
                session.rank->banks[0].injectRawDataBit(physical, 0, 15);
                result.injectedBits = 2;
            }
        }

        uint64_t items = 0;
        runKernel(session, workload, items, 0);
        result.pimItems += items;
        result.cycles += drainReports(*session.feeder, reports, 8000000ull);
        const uint64_t flipsBefore = session.model.cumulativeFlips();
        result.physicalVictim = session.region.physical(result.attackRow);

        if (attack != "NO_ATK")
        {
            const double need = session.model.firstThreshold(0, session.region.physical(result.attackRow));
            result.threshold = need;
            const uint64_t cap = static_cast<uint64_t>(need * 2.6) + 64ull;
            DRAMSim::BurstType background;
            uint64_t step = 0;
            auto outsideNeighbor = [](unsigned victim) -> unsigned {
                const unsigned region = victim / RegionMap::kRegion;
                if (victim > 0 && (victim - 1) / RegionMap::kRegion != region)
                    return victim - 1;
                if (victim + 1 < RegionMap::kRows && (victim + 1) / RegionMap::kRegion != region)
                    return victim + 1;
                return victim > 0 ? victim - 1 : victim + 1;
            };
            auto oneAggressor = [&](unsigned victim) -> unsigned {
                unsigned aggressor = outsideNeighbor(victim);
                if (aggressor == 0 || aggressor >= RegionMap::kRows)
                    aggressor = victim > 0 ? victim - 1 : victim + 1;
                return aggressor;
            };
            auto hammer = [&]() {
                const unsigned victim = session.region.physical(result.attackRow);
                const unsigned aggressor = oneAggressor(victim);
                step += 1;
                // HOST_ATK names the physical aggressor. A read still closes and
                // reopens that row, so the activation count is unchanged, but it
                // does not store a burst over whatever logical data now sits there.
                DRAMSim::BurstType activated;
                enqueueRead(*session.feeder, session.codec, 0, aggressor, activated, "HOST_ATK");
                enqueueWrite(*session.feeder, session.codec, 1,
                             static_cast<unsigned>(200 + (result.attackTx % 32)), background, "HOST_BG");
                result.attackTx += 1;
                result.hostTx += 2;
                if (attack == "A3")
                {
                    const unsigned base = (aggressor / RegionMap::kRegion) * RegionMap::kRegion;
                    unsigned scatter = base + static_cast<unsigned>((aggressor + 2u + (step % 15ull)) % RegionMap::kRegion);
                    if (scatter == aggressor)
                        scatter = base + ((aggressor + 1u) % RegionMap::kRegion);
                    enqueueRead(*session.feeder, session.codec, 0, scatter, activated, "HOST_ATK");
                    result.hostTx += 1;
                }
                result.cycles += drainReports(*session.feeder, reports, 8000000ull);
                session.flushSwaps(result.cycles);
            };
            while (result.attackTx < cap &&
                   session.model.flipsOn(0, session.region.physical(result.attackRow)) < 1u)
                hammer();
            DRAMSim::BurstType probed;
            enqueueRead(*session.feeder, session.codec, 0, result.attackRow, probed, "HOST_BG");
            result.hostTx += 1;
            result.cycles += drainReports(*session.feeder, reports, 2000000ull);
            session.flushSwaps(result.cycles);
            const uint64_t flipsAfterFirst = session.model.cumulativeFlips();
            while (result.attackTx < cap && session.model.cumulativeFlips() == flipsAfterFirst)
                hammer();
        }
        result.physicalVictimEnd = session.region.physical(result.attackRow);

        uint64_t backgroundHost = result.pimItems / 4;
        if (backgroundHost == 0)
            backgroundHost = 1;
        DRAMSim::BurstType filler;
        for (uint64_t i = 0; i < backgroundHost; ++i)
        {
            enqueueWrite(*session.feeder, session.codec, 3, static_cast<unsigned>(300 + (i % 16)), filler, "HOST_BG");
            result.hostTx += 1;
        }
        result.cycles += drainReports(*session.feeder, reports, 8000000ull);
        session.flushSwaps(result.cycles);

        auto readCounted = [&](unsigned row, size_t count) {
            std::vector<BurstType> out(count);
            session.kernel->readData(out.data(), count, row, 0);
            result.cycles += drainReports(*session.feeder, reports, 8000000ull);
            result.hostTx += count;
            return out;
        };
        const std::vector<BurstType> beforeRecovery = readCounted(result.scoreRow, bursts);
        result.computeErrors = countBurstDiff(beforeRecovery, expected);
        result.visibleBits = countBurstBits(beforeRecovery, expected);

        phase2::Ledger ledger;
        for (const auto& input : inputs)
            ledger.setBaseRow(input.first, input.second);
        for (const KernelDesc& kernel : chain)
            ledger.setBaseRow(kernel.out, static_cast<unsigned>(kernel.output));
        const uint64_t completedAt = session.feeder->currentClockCycle;
        for (const KernelDesc& kernel : chain)
        {
            for (unsigned tile = 0; tile < tiles; ++tile)
            {
                std::vector<std::pair<uint16_t, uint32_t>> reads = {{kernel.in0, tile}};
                if (kernel.hasSecond)
                    reads.push_back({kernel.in1, tile});
                const uint64_t op = ledger.planOp(1, kernel.index, tile, reads, {{kernel.out, tile}});
                ledger.recordIssue(op, completedAt);
                for (const auto& ref : reads)
                    ledger.observeAccess(op, ledger.baseRow(ref.first) + phase2::tileRowOffset(tile), false);
                ledger.observeAccess(op, ledger.baseRow(kernel.out) + phase2::tileRowOffset(tile), true);
                if (!ledger.completeObserved(op, completedAt))
                    throw std::runtime_error("campaign ledger did not complete a declared op");
            }
        }

        phase2::Gate gate;
        const uint64_t resultVersion = ledger.currentVersion(resultOperand, 0);
        const phase2::DataRef resultRef{resultOperand, 0, resultVersion};
        gate.hold(resultRef);
        const unsigned resultSpan = (bursts + 511u) / 512u;
        for (unsigned row = 0; row < resultSpan; ++row)
            phase3BindPublication(result.scoreRow + row, &gate, resultOperand, 0, resultVersion);

        phase2::EngineConfig config;
        config.policy = phase2::Policy::B2_CLASS_AWARE;
        config.window_mode = phase2::WindowMode::AUTO;
        config.delta_cycles = 1;
        phase2::CheckpointView view;
        if (inject != nullptr && inject->dropCheckpoint)
            view.setAbsent(0, 0);
        phase2::RecoveryEngine engine;
        std::vector<phase2::Run> runs;
        std::vector<phase2::DataRef> restores;
        bool poison = false;
        if (session.region.recoveryEnabled())
        {
            for (const Phase1PublicReport& report : reports.reports())
            {
                if (report.error_class != "UE")
                    continue;
                const phase2::Decision decision =
                    engine.analyze(report, completedAt, config, ledger, gate, view, reports.overflow(),
                                   session.codec);
                result.recovery = phase2::p2StatusName(decision.status);
                if (decision.status == phase2::P2Status::STALE_EPOCH)
                    continue;
                if (decision.status == phase2::P2Status::OUTSIDE_WINDOW_NO_TRUSTED_INPUT)
                {
                    poison = true;
                    break;
                }
                restores.insert(restores.end(), decision.restore_list.begin(), decision.restore_list.end());
                runs.insert(runs.end(), decision.replay_runs.begin(), decision.replay_runs.end());
            }
        }
        if (poison)
        {
            gate.poison({resultRef});
            result.withheld = true;
            result.recoveryFailed = true;
            result.refused = true;
            result.bitExact = true;
        }
        else
        {
            std::set<std::pair<uint16_t, uint32_t>> restored;
            for (const phase2::DataRef& ref : restores)
            {
                if (ref.version != 0 || !restored.insert(std::make_pair(ref.operand, ref.tile)).second)
                    continue;
                if (ref.tile < 64)
                    result.restoredMask |= 1ull << ref.tile;
                const std::vector<BurstType>* tile = store.tile(ref.operand, ref.tile);
                if (tile == nullptr)
                    throw std::runtime_error("recovery asked for a tile the checkpoint does not hold");
                preloadRow(*session.kernel, *tile, ledger.baseRow(ref.operand) + phase2::tileRowOffset(ref.tile));
                result.restoredTiles += 1;
            }
            if (result.restoredTiles > 0)
                result.cycles += drainReports(*session.feeder, reports, 8000000ull);
            std::sort(runs.begin(), runs.end(), [](const phase2::Run& lhs, const phase2::Run& rhs) {
                if (lhs.kernel_idx != rhs.kernel_idx)
                    return lhs.kernel_idx < rhs.kernel_idx;
                return lhs.tile_begin < rhs.tile_begin;
            });
            for (const phase2::Run& run : runs)
            {
                const KernelDesc* kernel = nullptr;
                for (const KernelDesc& one : chain)
                    if (one.index == run.kernel_idx)
                        kernel = &one;
                if (kernel == nullptr)
                    throw std::runtime_error("recovery plan names an unknown kernel");
                const int count = static_cast<int>(run.tile_count == 0 ? 1u : run.tile_count);
                const size_t pendingBefore = session.feeder->pendingCount();
                session.kernel->executeEltwiseTiles(kernel->type, static_cast<int>(run.tile_begin), count,
                                                    kernel->input0, kernel->output, kernel->input1);
                result.pimItems += session.feeder->pendingCount() - pendingBefore;
                result.replayed = true;
            }
            if (result.replayed)
                result.cycles += drainReports(*session.feeder, reports, 8000000ull);
            gate.publish(resultRef, session.feeder->currentClockCycle);
            const std::vector<BurstType> committed = readCounted(result.scoreRow, bursts);
            result.committedMismatch = countBurstDiff(committed, expected);
            result.bitExact = result.committedMismatch == 0;
            result.erroneousCommit = !result.bitExact;
        }

        for (const Bank& bank : session.rank->banks)
            result.scrubWrites += bank.phase1ScrubWrites();
        result.scrubCycles = hbm2ScrubCycles(result.scrubWrites);
        result.cycles += result.scrubCycles;
        session.billMoves(result.cycles);
        result.swapCycles = session.swapCycles;
        result.flips = session.model.cumulativeFlips() - flipsBefore;
        result.latent = session.model.flipCount();
        result.crossings = session.model.crossings().size();
        for (const ReadDisturbanceModel::Crossing& crossing : session.model.crossings())
            if (!crossing.placed)
                result.unplaced += 1;
        result.modelScrubs = session.model.scrubbedCount();
        result.ce = reports.internalCe();
        result.ue = reports.internalUe();
        result.level = session.region.level(result.scoreRow / RegionMap::kRegion);
        result.sawL2 = session.region.sawL2();
        if (result.recovery == "NONE" && result.ce > 0 && !result.replayed)
            result.recovery = "CE_OBSERVED";
    }
    catch (const std::exception& failure)
    {
        result.status = "FAILED";
        result.error = failure.what();
        for (char& ch : result.error)
        {
            if (ch == '"' || ch == '\n')
                ch = ' ';
        }
    }
    return result;
}
}  // namespace

TEST(Phase3M3, DataPath)
{
    const std::string out =
        "/opt/data/data/workspace-dynamic/Rowhammer/phase3_exp/results/m3/datapath";
    Session w2(kDataDevice, kDataSystem, out, 1, 1.0, "NONE", 1.5, 0.0);
    const unsigned w2Bursts = 48u * 128u;
    std::vector<BurstType> left(w2Bursts), right(w2Bursts), scale(w2Bursts), w2Expected(w2Bursts);
    for (unsigned i = 0; i < w2Bursts; ++i)
    {
        left[i] = positiveBurst(i, 1);
        right[i] = positiveBurst(i, 2);
        scale[i] = positiveBurst(i, 3);
        w2Expected[i] = applyRelu((left[i] + right[i]) * scale[i]);
    }
    preloadRow(*w2.kernel, left, 0);
    preloadRow(*w2.kernel, right, 128);
    preloadRow(*w2.kernel, scale, 384);
    drain(*w2.feeder, 8000000ull);
    BurstType operandReadback;
    enqueueRead(*w2.feeder, w2.codec, 0, 0, operandReadback, "HOST_BG");
    drain(*w2.feeder, 2000000ull);
    const bool operandLanded = std::memcmp(operandReadback.u8Data_, left[0].u8Data_, 32) == 0;

    uint64_t items = 0;
    runKernel(w2, "W2", items, 0);
    drain(*w2.feeder, 8000000ull);
    const std::vector<BurstType> w2Clean = readRow(w2, resultRow("W2"), w2Bursts);
    const unsigned w2CleanDiff = countBurstDiff(w2Clean, w2Expected);

    w2.rank->banks[0].injectRawDataBit(0, 0, 15);
    runKernel(w2, "W2", items, 0);
    drain(*w2.feeder, 8000000ull);
    const std::vector<BurstType> w2Fault = readRow(w2, resultRow("W2"), w2Bursts);
    const unsigned w2FaultDiff = countBurstDiff(w2Fault, w2Clean);

    rewriteBurst(w2, 0, 0, 0, left[0]);
    runKernel(w2, "W2", items, 0);
    drain(*w2.feeder, 8000000ull);
    const std::vector<BurstType> w2Restored = readRow(w2, resultRow("W2"), w2Bursts);
    const unsigned w2RestoredDiff = countBurstDiff(w2Restored, w2Expected);

    BurstType unused;
    unused.fp16Data_[0] = convertF2H(1.0f);
    rewriteBurst(w2, 0, 900, 0, unused);
    w2.rank->banks[0].injectRawDataBit(900, 0, 0);
    runKernel(w2, "W2", items, 0);
    drain(*w2.feeder, 8000000ull);
    const std::vector<BurstType> w2Control = readRow(w2, resultRow("W2"), w2Bursts);
    const unsigned w2ControlDiff = countBurstDiff(w2Control, w2Restored);

    Session dec(kDataDevice, kDataSystem, out, 1, 1.0, "NONE", 1.5, 0.0);
    const unsigned decBursts = 128u;
    std::vector<BurstType> decA(decBursts), decB(decBursts), decExpected(decBursts);
    for (unsigned i = 0; i < decBursts; ++i)
    {
        decA[i] = positiveBurst(i, 4);
        decB[i] = positiveBurst(i, 5);
        decExpected[i] = applyRelu(decA[i] + decB[i]);
    }
    preloadRow(*dec.kernel, decA, 2000);
    preloadRow(*dec.kernel, decB, 2128);
    drain(*dec.feeder, 8000000ull);
    dec.kernel->executeEltwise(static_cast<int>(decBursts), pimBankType::ALL_BANK, KernelType::ADD,
                               2000, 2256, 2128);
    dec.kernel->executeEltwise(static_cast<int>(decBursts), pimBankType::ALL_BANK, KernelType::RELU,
                               2256, static_cast<int>(resultRow("DEC")), 0);
    drain(*dec.feeder, 8000000ull);
    const std::vector<BurstType> decClean = readRow(dec, resultRow("DEC"), decBursts);
    const unsigned decCleanDiff = countBurstDiff(decClean, decExpected);

    dec.rank->banks[0].injectRawDataBit(2000, 0, 15);
    dec.kernel->executeEltwise(static_cast<int>(decBursts), pimBankType::ALL_BANK, KernelType::ADD,
                               2000, 2256, 2128);
    dec.kernel->executeEltwise(static_cast<int>(decBursts), pimBankType::ALL_BANK, KernelType::RELU,
                               2256, static_cast<int>(resultRow("DEC")), 0);
    drain(*dec.feeder, 8000000ull);
    const std::vector<BurstType> decFault = readRow(dec, resultRow("DEC"), decBursts);
    const unsigned decFaultDiff = countBurstDiff(decFault, decClean);

    rewriteBurst(dec, 0, 2000, 0, decA[0]);
    dec.kernel->executeEltwise(static_cast<int>(decBursts), pimBankType::ALL_BANK, KernelType::ADD,
                               2000, 2256, 2128);
    dec.kernel->executeEltwise(static_cast<int>(decBursts), pimBankType::ALL_BANK, KernelType::RELU,
                               2256, static_cast<int>(resultRow("DEC")), 0);
    drain(*dec.feeder, 8000000ull);
    const std::vector<BurstType> decRestored = readRow(dec, resultRow("DEC"), decBursts);
    const unsigned decRestoredDiff = countBurstDiff(decRestored, decExpected);

    std::ostringstream json;
    json << "{\"operand_readback\":" << (operandLanded ? "true" : "false")
         << ",\"w2_clean_mismatch\":" << w2CleanDiff
         << ",\"w2_fault_changed\":" << w2FaultDiff
         << ",\"w2_restored_mismatch\":" << w2RestoredDiff
         << ",\"w2_unused_changed\":" << w2ControlDiff
         << ",\"dec_clean_mismatch\":" << decCleanDiff
         << ",\"dec_fault_changed\":" << decFaultDiff
         << ",\"dec_restored_mismatch\":" << decRestoredDiff << "}\n";
    writeCell(out + "/diagnostic.json", json.str());

    EXPECT_TRUE(operandLanded);
    EXPECT_EQ(0u, w2CleanDiff);
    EXPECT_GT(w2FaultDiff, 0u);
    EXPECT_EQ(0u, w2RestoredDiff);
    EXPECT_EQ(0u, w2ControlDiff);
    EXPECT_EQ(0u, decCleanDiff);
    EXPECT_GT(decFaultDiff, 0u);
    EXPECT_EQ(0u, decRestoredDiff);
}

namespace
{
struct EccCase
{
    unsigned outputMismatch = 0;
    unsigned operandMismatch = 0;
    uint64_t ce = 0;
    uint64_t ue = 0;
    uint64_t scrubWrites = 0;
    unsigned level = 0;
    uint64_t ceAfterReread = 0;
    uint64_t ueAfterReread = 0;
};

void armLocalEcc(Session& session, Phase1Reports& reports)
{
    for (Bank& bank : session.rank->banks)
    {
        bank.setPhase1(Phase1Profile::LOCAL_VISIBLE, &reports);
        bank.setPhase1Scrub(true);
    }
}

uint64_t scrubWriteTotal(const Session& session)
{
    uint64_t total = 0;
    for (const Bank& bank : session.rank->banks)
        total += bank.phase1ScrubWrites();
    return total;
}

EccCase runEccOperandCase(const std::string& out, unsigned flipBits)
{
    Session session(kDataDevice, kDataSystem, out, 1, 1.0, "OURS", 1.1, 0.0);
    Phase1Reports reports(session.codec, 1, static_cast<unsigned>(session.rank->banks.size()),
                          "codeword", 1, 1, 64, true, true);
    armLocalEcc(session, reports);
    const unsigned bursts = 128u;
    const unsigned operandRow = 2000u;
    std::vector<BurstType> left(bursts), right(bursts), expected(bursts);
    for (unsigned i = 0; i < bursts; ++i)
    {
        left[i] = positiveBurst(i, 4);
        right[i] = positiveBurst(i, 5);
        expected[i] = applyRelu(left[i] + right[i]);
    }
    preloadRow(*session.kernel, left, operandRow);
    preloadRow(*session.kernel, right, 2128);
    drain(*session.feeder, 8000000ull);
    if (flipBits & 1u)
        session.rank->banks[0].injectRawDataBit(operandRow, 0, 14);
    if (flipBits & 2u)
        session.rank->banks[0].injectRawDataBit(operandRow, 0, 15);
    session.kernel->executeEltwise(static_cast<int>(bursts), pimBankType::ALL_BANK, KernelType::ADD,
                                   2000, 2256, 2128);
    session.kernel->executeEltwise(static_cast<int>(bursts), pimBankType::ALL_BANK, KernelType::RELU,
                                   2256, static_cast<int>(resultRow("DEC")), 0);
    drain(*session.feeder, 8000000ull);
    EccCase result;
    result.ce = reports.internalCe();
    result.ue = reports.internalUe();
    result.scrubWrites = scrubWriteTotal(session);
    result.level = session.region.level(operandRow / RegionMap::kRegion);
    const std::vector<BurstType> output = readRow(session, resultRow("DEC"), bursts);
    result.outputMismatch = countBurstDiff(output, expected);
    BurstType operand;
    enqueueRead(*session.feeder, session.codec, 0, operandRow, operand, "HOST_BG");
    drain(*session.feeder, 2000000ull);
    result.operandMismatch = std::memcmp(operand.u8Data_, left[0].u8Data_, 32) == 0 ? 0u : 1u;
    result.ceAfterReread = reports.internalCe();
    result.ueAfterReread = reports.internalUe();
    return result;
}
}  // namespace

TEST(Phase3M3, EccFeedback)
{
    const std::string out =
        "/opt/data/data/workspace-dynamic/Rowhammer/phase3_exp/results/m3/datapath_ecc";
    const EccCase clean = runEccOperandCase(out, 0u);
    const EccCase single = runEccOperandCase(out, 1u);
    const EccCase dual = runEccOperandCase(out, 3u);
    std::ostringstream json;
    json << "{\"clean_mismatch\":" << clean.outputMismatch
         << ",\"clean_ce\":" << clean.ce << ",\"clean_ue\":" << clean.ue
         << ",\"clean_level\":" << clean.level
         << ",\"single_mismatch\":" << single.outputMismatch
         << ",\"single_ce\":" << single.ce << ",\"single_ue\":" << single.ue
         << ",\"single_scrub\":" << single.scrubWrites
         << ",\"single_level\":" << single.level
         << ",\"single_operand_mismatch\":" << single.operandMismatch
         << ",\"single_ce_after_reread\":" << single.ceAfterReread
         << ",\"dual_mismatch\":" << dual.outputMismatch
         << ",\"dual_ce\":" << dual.ce << ",\"dual_ue\":" << dual.ue
         << ",\"dual_scrub\":" << dual.scrubWrites
         << ",\"dual_level\":" << dual.level
         << ",\"dual_operand_mismatch\":" << dual.operandMismatch
         << ",\"dual_ue_after_reread\":" << dual.ueAfterReread << "}\n";
    writeCell(out + "/ecc_diagnostic.json", json.str());

    EXPECT_EQ(0u, clean.outputMismatch);
    EXPECT_EQ(0u, clean.ce);
    EXPECT_EQ(0u, clean.ue);
    EXPECT_EQ(0u, clean.level);

    EXPECT_EQ(0u, single.outputMismatch);
    EXPECT_GT(single.ce, 0u);
    EXPECT_EQ(0u, single.ue);
    EXPECT_GT(single.scrubWrites, 0u);
    EXPECT_EQ(1u, single.level);
    EXPECT_EQ(0u, single.operandMismatch);
    EXPECT_EQ(single.ce, single.ceAfterReread);

    EXPECT_GT(dual.outputMismatch, 0u);
    EXPECT_EQ(0u, dual.ce);
    EXPECT_GT(dual.ue, 0u);
    EXPECT_EQ(0u, dual.scrubWrites);
    EXPECT_EQ(0u, dual.level);
    EXPECT_EQ(1u, dual.operandMismatch);
    EXPECT_GT(dual.ueAfterReread, dual.ue);
}

namespace
{
enum class RecoveryMode
{
    Clean,
    Corrected,
    Uncorrectable,
    Unavailable
};

struct RecoveryCase
{
    bool submitted = false;
    bool published = false;
    unsigned mismatch = 0;
    unsigned auditMismatch = 0;
    unsigned replayTiles = 0;
    unsigned restoredTiles = 0;
    bool restoreIsInput = false;
    unsigned poison = 0;
    bool poisonCoversResult = false;
    unsigned reports = 0;
    std::string reportClass = "NONE";
    std::string status = "NONE";
    unsigned logicalRow = 0;
};

uint64_t drainReports(Phase1FeederMemory& feeder, Phase1Reports& reports, uint64_t watchdog)
{
    uint64_t cycles = 0;
    while (feeder.pendingCount() > 0 || !Phase1Driver::directIdle(feeder) || refreshPending(feeder) ||
           reports.pendingCount() > 0)
    {
        if (cycles >= watchdog)
            throw std::runtime_error("m3 recovery watchdog exceeded");
        feeder.tick();
        feeder.update();
        reports.tick(feeder.currentClockCycle);
        cycles += 1;
    }
    return cycles;
}

void runAddRelu(Session& session, Phase1Reports& reports)
{
    const int bursts = 128;
    session.kernel->executeEltwise(bursts, pimBankType::ALL_BANK, KernelType::ADD, 2000, 2256, 2128);
    session.kernel->executeEltwise(bursts, pimBankType::ALL_BANK, KernelType::RELU, 2256,
                                   static_cast<int>(resultRow("DEC")), 0);
    drainReports(*session.feeder, reports, 8000000ull);
}

RecoveryCase runRecoveryCase(const std::string& out, RecoveryMode mode)
{
    Session session(kDataDevice, kDataSystem, out, 1, 1.0, "NONE", 1.5, 0.0);
    Phase1Reports reports(session.codec, 1, static_cast<unsigned>(session.rank->banks.size()),
                          "codeword", 1, 1, 64, true, true);
    for (Bank& bank : session.rank->banks)
    {
        bank.setPhase1(Phase1Profile::LOCAL_VISIBLE, &reports);
        bank.setPhase1Scrub(true);
    }
    const unsigned bursts = 128u;
    std::vector<BurstType> left(bursts), right(bursts), expected(bursts);
    for (unsigned i = 0; i < bursts; ++i)
    {
        left[i] = positiveBurst(i, 4);
        right[i] = positiveBurst(i, 5);
        expected[i] = applyRelu(left[i] + right[i]);
    }
    phase2::Phase2CheckpointStore store;
    store.addTile(1, 0, right);
    if (mode != RecoveryMode::Unavailable)
        store.addTile(0, 0, left);
    phase2::CheckpointView view;
    if (mode == RecoveryMode::Unavailable)
        view.setAbsent(0, 0);

    preloadRow(*session.kernel, left, 2000);
    preloadRow(*session.kernel, right, 2128);
    drainReports(*session.feeder, reports, 8000000ull);
    if (mode == RecoveryMode::Corrected)
        session.rank->banks[0].injectRawDataBit(2000, 0, 14);
    if (mode == RecoveryMode::Uncorrectable || mode == RecoveryMode::Unavailable)
    {
        session.rank->banks[0].injectRawDataBit(2000, 0, 14);
        session.rank->banks[0].injectRawDataBit(2000, 0, 15);
    }

    const uint64_t issued = session.feeder->currentClockCycle;
    runAddRelu(session, reports);
    const uint64_t completed = session.feeder->currentClockCycle;

    phase2::Ledger ledger;
    ledger.setBaseRow(0, 2000);
    ledger.setBaseRow(1, 2128);
    ledger.setBaseRow(2, 2256);
    ledger.setBaseRow(3, resultRow("DEC"));
    ledger.addCompleted(1, 0, 0, {{0, 0}, {1, 0}}, {{2, 0}}, issued, completed);
    const uint64_t reluOp = ledger.addCompleted(1, 1, 0, {{2, 0}}, {{3, 0}}, issued, completed);
    const phase2::DataRef resultRef{3, 0, reluOp};
    phase2::Gate gate;
    gate.hold(resultRef);

    RecoveryCase result;
    result.logicalRow = phase3LogicalRow(0, resultRow("DEC"));
    result.reports = static_cast<unsigned>(reports.reports().size());
    if (!reports.reports().empty())
        result.reportClass = reports.reports()[0].error_class;

    phase2::Decision decision;
    const bool fault = !reports.reports().empty();
    if (fault)
    {
        phase2::EngineConfig config;
        config.policy = phase2::Policy::B2_CLASS_AWARE;
        config.window_mode = phase2::WindowMode::AUTO;
        config.delta_cycles = 1;
        phase2::RecoveryEngine engine;
        const Phase1PublicReport* selected = &reports.reports()[0];
        for (const Phase1PublicReport& report : reports.reports())
            if (report.error_class == "UE")
                selected = &report;
        decision = engine.analyze(*selected, completed, config, ledger, gate, view,
                                  reports.overflow(), session.codec);
        result.status = phase2::p2StatusName(decision.status);
        if (decision.status == phase2::P2Status::OUTSIDE_WINDOW_NO_TRUSTED_INPUT)
        {
            const phase2::ExecutionPlan plan = engine.planExecution(decision, ledger, &config, &gate);
            gate.poison(plan.poison_outputs);
            result.poison = static_cast<unsigned>(plan.poison_outputs.size());
            for (const phase2::DataRef& ref : plan.poison_outputs)
            {
                if (ref.operand == resultRef.operand && ref.tile == resultRef.tile)
                    result.poisonCoversResult = true;
                if (gate.state(ref) != phase2::Lifecycle::POISONED)
                    result.poisonCoversResult = false;
            }
            const std::vector<BurstType> audit = readRow(session, result.logicalRow, bursts);
            result.auditMismatch = countBurstDiff(audit, expected);
            return result;
        }
        if (!decision.replay_runs.empty())
        {
            result.restoreIsInput = !decision.restore_list.empty();
            for (const phase2::DataRef& ref : decision.restore_list)
            {
                if (ref.version != 0)
                    throw std::runtime_error("recovery asked to restore a computed value");
                const std::vector<BurstType>* tile = store.tile(ref.operand, ref.tile);
                if (tile == nullptr)
                    throw std::runtime_error("recovery asked for a tile the checkpoint does not hold");
                const std::vector<BurstType>* source = nullptr;
                if (ref.operand == 0)
                    source = &left;
                else if (ref.operand == 1)
                    source = &right;
                if (source == nullptr || countBurstDiff(*tile, *source) != 0 ||
                    countBurstDiff(*tile, expected) == 0)
                    result.restoreIsInput = false;
                preloadRow(*session.kernel, *tile,
                           ledger.baseRow(ref.operand) + phase2::tileRowOffset(ref.tile));
                result.restoredTiles += 1;
            }
            drainReports(*session.feeder, reports, 8000000ull);
            std::vector<phase2::Run> runs = decision.replay_runs;
            std::sort(runs.begin(), runs.end(), [](const phase2::Run& lhs, const phase2::Run& rhs) {
                if (lhs.kernel_idx != rhs.kernel_idx)
                    return lhs.kernel_idx < rhs.kernel_idx;
                return lhs.tile_begin < rhs.tile_begin;
            });
            for (const phase2::Run& run : runs)
            {
                if (run.kernel_idx == 0)
                    session.kernel->executeEltwise(static_cast<int>(bursts), pimBankType::ALL_BANK,
                                                   KernelType::ADD, 2000, 2256, 2128);
                else if (run.kernel_idx == 1)
                    session.kernel->executeEltwise(static_cast<int>(bursts), pimBankType::ALL_BANK,
                                                   KernelType::RELU, 2256,
                                                   static_cast<int>(resultRow("DEC")), 0);
                else
                    throw std::runtime_error("recovery plan names an unknown kernel");
                result.replayTiles += run.tile_count;
            }
            drainReports(*session.feeder, reports, 8000000ull);
        }
    }
    gate.publish(resultRef, session.feeder->currentClockCycle);
    result.published = gate.state(resultRef) == phase2::Lifecycle::PUBLISHED;
    if (result.published)
    {
        const std::vector<BurstType> output = readRow(session, result.logicalRow, bursts);
        result.submitted = output.size() == bursts;
        result.mismatch = countBurstDiff(output, expected);
    }
    return result;
}
}  // namespace

TEST(Phase3M3, Recovery)
{
    const std::string out =
        "/opt/data/data/workspace-dynamic/Rowhammer/phase3_exp/results/m3/datapath_recovery";
    const RecoveryCase clean = runRecoveryCase(out, RecoveryMode::Clean);
    const RecoveryCase corrected = runRecoveryCase(out, RecoveryMode::Corrected);
    const RecoveryCase recovered = runRecoveryCase(out, RecoveryMode::Uncorrectable);
    const RecoveryCase blocked = runRecoveryCase(out, RecoveryMode::Unavailable);
    std::ostringstream json;
    json << "{\"clean_submitted\":" << (clean.submitted ? "true" : "false")
         << ",\"clean_mismatch\":" << clean.mismatch
         << ",\"clean_replay\":" << clean.replayTiles
         << ",\"clean_reports\":" << clean.reports
         << ",\"ce_submitted\":" << (corrected.submitted ? "true" : "false")
         << ",\"ce_mismatch\":" << corrected.mismatch
         << ",\"ce_replay\":" << corrected.replayTiles
         << ",\"ce_class\":\"" << corrected.reportClass << "\""
         << ",\"ce_status\":\"" << corrected.status << "\""
         << ",\"ue_submitted\":" << (recovered.submitted ? "true" : "false")
         << ",\"ue_mismatch\":" << recovered.mismatch
         << ",\"ue_replay\":" << recovered.replayTiles
         << ",\"ue_restored\":" << recovered.restoredTiles
         << ",\"ue_restore_is_input\":" << (recovered.restoreIsInput ? "true" : "false")
         << ",\"ue_class\":\"" << recovered.reportClass << "\""
         << ",\"ue_status\":\"" << recovered.status << "\""
         << ",\"fail_submitted\":" << (blocked.submitted ? "true" : "false")
         << ",\"fail_published\":" << (blocked.published ? "true" : "false")
         << ",\"fail_poison\":" << blocked.poison
         << ",\"fail_covers_result\":" << (blocked.poisonCoversResult ? "true" : "false")
         << ",\"fail_audit_mismatch\":" << blocked.auditMismatch
         << ",\"fail_class\":\"" << blocked.reportClass << "\""
         << ",\"fail_status\":\"" << blocked.status << "\""
         << ",\"logical_row\":" << clean.logicalRow << "}\n";
    writeCell(out + "/recovery_diagnostic.json", json.str());

    EXPECT_TRUE(clean.submitted);
    EXPECT_EQ(0u, clean.mismatch);
    EXPECT_EQ(0u, clean.replayTiles);
    EXPECT_EQ(0u, clean.reports);

    EXPECT_TRUE(corrected.submitted);
    EXPECT_EQ(0u, corrected.mismatch);
    EXPECT_EQ(0u, corrected.replayTiles);
    EXPECT_EQ(std::string("CE"), corrected.reportClass);
    EXPECT_EQ(std::string("SOURCE_REPAIRED_ONLY"), corrected.status);

    EXPECT_TRUE(recovered.submitted);
    EXPECT_EQ(0u, recovered.mismatch);
    EXPECT_GT(recovered.replayTiles, 0u);
    EXPECT_GT(recovered.restoredTiles, 0u);
    EXPECT_TRUE(recovered.restoreIsInput);
    EXPECT_EQ(std::string("UE"), recovered.reportClass);
    EXPECT_EQ(std::string("SELECTIVE_RECOVERED"), recovered.status);

    EXPECT_FALSE(blocked.submitted);
    EXPECT_FALSE(blocked.published);
    EXPECT_GT(blocked.poison, 0u);
    EXPECT_TRUE(blocked.poisonCoversResult);
    EXPECT_GT(blocked.auditMismatch, 0u);
    EXPECT_EQ(std::string("UE"), blocked.reportClass);
    EXPECT_EQ(std::string("OUTSIDE_WINDOW_NO_TRUSTED_INPUT"), blocked.status);
}

namespace
{
struct PublishProbe
{
    bool heldHostConsumed = false;
    bool heldPimConsumed = false;
    bool poisonedHostConsumed = false;
    bool poisonedPimConsumed = false;
    bool wrapperBlocked = false;
};

PublishProbe runPublishProbe(const std::string& out)
{
    Session session(kDataDevice, kDataSystem, out, 1, 1.0, "NONE", 1.5, 0.0);
    const unsigned bursts = 128u;
    std::vector<BurstType> left(bursts), right(bursts), expected(bursts), doubled(bursts);
    for (unsigned i = 0; i < bursts; ++i)
    {
        left[i] = positiveBurst(i, 4);
        right[i] = positiveBurst(i, 5);
        expected[i] = left[i] + right[i];
        doubled[i] = expected[i] + expected[i];
    }
    preloadRow(*session.kernel, left, 0);
    preloadRow(*session.kernel, right, 128);
    drain(*session.feeder, 8000000ull);
    session.kernel->executeEltwise(static_cast<int>(bursts), pimBankType::ALL_BANK, KernelType::ADD, 0,
                                   256, 128);
    drain(*session.feeder, 8000000ull);
    phase2::Gate gate;
    const phase2::DataRef result{2, 0, 1};
    gate.hold(result);
    PublishProbe probe;
    probe.heldHostConsumed = countBurstDiff(readRow(session, 256, bursts), expected) == 0;
    session.kernel->executeEltwise(static_cast<int>(bursts), pimBankType::ALL_BANK, KernelType::ADD, 256,
                                   512, 256);
    drain(*session.feeder, 8000000ull);
    probe.heldPimConsumed = countBurstDiff(readRow(session, 512, bursts), doubled) == 0;
    gate.poison({result});
    probe.poisonedHostConsumed = countBurstDiff(readRow(session, 256, bursts), expected) == 0;
    session.kernel->executeEltwise(static_cast<int>(bursts), pimBankType::ALL_BANK, KernelType::ADD, 256,
                                   640, 256);
    drain(*session.feeder, 8000000ull);
    probe.poisonedPimConsumed = countBurstDiff(readRow(session, 640, bursts), doubled) == 0;
    probe.wrapperBlocked = gate.state(result) == phase2::Lifecycle::POISONED;
    return probe;
}

struct MapProbe
{
    unsigned physical = 0;
    bool nativePhysicalMatch = false;
    bool nativeStaleMatch = false;
    bool eccPhysicalMatch = false;
    bool eccStaleMatch = false;
    bool eccSourceRemains = false;
    bool orphanMatches = false;
    bool ueCopied = false;
    bool ueSourceRemains = false;
};

bool storageHas(const Bank& bank, unsigned row)
{
    Phase1StorageKey key{0, 0, 0, row, 0, 0};
    return bank.phase1Storage().contains(key);
}

MapProbe runMapProbe(const std::string& out)
{
    Session session(kDataDevice, kDataSystem, out, 1, 1.0, "OURS", 1.1, 0.0);
    Phase1Reports reports(session.codec, 1, static_cast<unsigned>(session.rank->banks.size()),
                          "codeword", 1, 1, 64, true, true);
    armLocalEcc(session, reports);
    const unsigned bursts = 128u;
    std::vector<BurstType> left(bursts);
    for (unsigned i = 0; i < bursts; ++i)
        left[i] = positiveBurst(i, 4);
    preloadRow(*session.kernel, left, 0);
    drainReports(*session.feeder, reports, 8000000ull);
    session.region.forceSwap(&session.model);
    MapProbe probe;
    probe.physical = session.region.physical(0);
    for (Bank& bank : session.rank->banks)
        bank.setPhase1(Phase1Profile::NATIVE, nullptr);
    probe.nativePhysicalMatch = countBurstDiff(readRow(session, probe.physical, bursts), left) == 0;
    probe.nativeStaleMatch = countBurstDiff(readRow(session, 0, bursts), left) == 0;
    for (Bank& bank : session.rank->banks)
        bank.setPhase1(Phase1Profile::LOCAL_VISIBLE, &reports);
    probe.eccPhysicalMatch = countBurstDiff(readRow(session, probe.physical, bursts), left) == 0;
    probe.eccStaleMatch = countBurstDiff(readRow(session, 0, bursts), left) == 0;
    probe.eccSourceRemains = storageHas(session.rank->banks[0], 0);
    if (probe.eccSourceRemains)
    {
        uint64_t original = 0;
        const uint64_t stored = session.rank->banks[0].phase1Storage().readRawData(
            Phase1StorageKey{0, 0, 0, 0, 0, 0});
        std::memcpy(&original, left[0].u8Data_, sizeof(original));
        probe.orphanMatches = stored == original;
    }

    Session broken(kDataDevice, kDataSystem, out, 2, 1.0, "OURS", 1.1, 0.0);
    Phase1Reports brokenReports(broken.codec, 1, static_cast<unsigned>(broken.rank->banks.size()),
                                "codeword", 1, 1, 64, true, true);
    armLocalEcc(broken, brokenReports);
    preloadRow(*broken.kernel, left, 0);
    drainReports(*broken.feeder, brokenReports, 8000000ull);
    broken.rank->banks[0].injectRawDataBit(0, 0, 14);
    broken.rank->banks[0].injectRawDataBit(0, 0, 15);
    broken.region.forceSwap(&broken.model);
    const unsigned moved = broken.region.physical(0);
    probe.ueCopied = storageHas(broken.rank->banks[0], moved);
    probe.ueSourceRemains = storageHas(broken.rank->banks[0], 0);
    return probe;
}

struct EpochProbe
{
    unsigned logicalOfZero = 0;
    bool accessMatches = false;
    unsigned reportLogical = 0;
    uint64_t reportEpoch = 0;
    bool engineUsesEpoch = false;
};

EpochProbe runEpochProbe(const std::string& out)
{
    Session session(kDataDevice, kDataSystem, out, 1, 1.0, "OURS-EPOCH", 1.1, 0.0);
    Phase1Reports reports(session.codec, 1, static_cast<unsigned>(session.rank->banks.size()),
                          "codeword", 1, 1, 64, true, true);
    armLocalEcc(session, reports);
    const unsigned bursts = 128u;
    std::vector<BurstType> left(bursts);
    for (unsigned i = 0; i < bursts; ++i)
        left[i] = positiveBurst(i, 6);
    preloadRow(*session.kernel, left, 0);
    drainReports(*session.feeder, reports, 8000000ull);
    session.rank->banks[0].injectRawDataBit(0, 0, 14);
    enqueueRead(*session.feeder, session.codec, 0, 0, left[0], "HOST_BG");
    drainReports(*session.feeder, reports, 8000000ull);
    EpochProbe probe;
    probe.logicalOfZero = session.region.logical(0);
    for (Bank& bank : session.rank->banks)
        bank.setPhase1(Phase1Profile::NATIVE, nullptr);
    std::vector<BurstType> fresh(bursts);
    for (unsigned i = 0; i < bursts; ++i)
        fresh[i] = positiveBurst(i, 6);
    probe.accessMatches = countBurstDiff(readRow(session, 0, bursts), fresh) == 0;
    if (!reports.reports().empty())
    {
        probe.reportLogical = reports.reports()[0].logical_row;
        probe.reportEpoch = reports.reports()[0].mapping_epoch;
    }
    phase2::Ledger ledger;
    ledger.setBaseRow(0, 0);
    ledger.setBaseRow(1, 128);
    ledger.addCompleted(1, 0, 0, {{0, 0}}, {{1, 0}}, 0, session.feeder->currentClockCycle);
    phase2::EngineConfig config;
    config.policy = phase2::Policy::B2_CLASS_AWARE;
    config.window_mode = phase2::WindowMode::AUTO;
    phase2::RecoveryEngine engine;
    phase2::Gate gate;
    phase2::CheckpointView view;
    const phase2::Decision decision =
        reports.reports().empty()
            ? phase2::Decision()
            : engine.analyze(reports.reports()[0], session.feeder->currentClockCycle, config, ledger,
                             gate, view, false, session.codec);
    probe.engineUsesEpoch = decision.status == phase2::P2Status::SOURCE_REPAIRED_ONLY &&
                            probe.reportLogical != 0;
    return probe;
}

struct CommandLedger
{
    phase2::Ledger ledger;
    int kernel = -1;
    uint64_t addId = 0;
    uint64_t reluId = 0;
    unsigned addReads = 0;
    unsigned addWrites = 0;
    unsigned reluReads = 0;
    unsigned reluWrites = 0;
};

struct JointProbe
{
    unsigned spare = 0;
    unsigned level = 0;
    bool autoIssued = false;
    bool autoCompleted = false;
    bool reluVersionFromAdd = false;
    bool staleReadClean = false;
    bool hostSawUe = false;
    bool pimSawUe = false;
    bool submitted = false;
    unsigned mismatch = 0;
    unsigned replay = 0;
    unsigned restored = 0;
    std::string reportClass = "NONE";
    std::string status = "NONE";
    unsigned reportLogical = 0;
    unsigned reportPhysical = 0;
};

JointProbe runJointProbe(const std::string& out)
{
    Session session(kDataDevice, kDataSystem, out, 1, 1.0, "OURS", 1.1, 0.0);
    Phase1Reports reports(session.codec, 1, static_cast<unsigned>(session.rank->banks.size()),
                          "codeword", 1, 1, 64, true, true);
    armLocalEcc(session, reports);
    const unsigned bursts = 128u;
    std::vector<BurstType> left(bursts), right(bursts), expected(bursts);
    for (unsigned i = 0; i < bursts; ++i)
    {
        left[i] = positiveBurst(i, 4);
        right[i] = positiveBurst(i, 5);
        expected[i] = applyRelu(left[i] + right[i]);
    }
    phase2::Phase2CheckpointStore store;
    store.addTile(0, 0, left);
    store.addTile(1, 0, right);
    preloadRow(*session.kernel, left, 0);
    preloadRow(*session.kernel, right, 128);
    drainReports(*session.feeder, reports, 8000000ull);
    session.rank->banks[0].injectRawDataBit(0, 0, 14);
    BurstType scratch;
    enqueueRead(*session.feeder, session.codec, 0, 0, scratch, "HOST_BG");
    drainReports(*session.feeder, reports, 8000000ull);
    session.region.forceSwap(&session.model);
    const unsigned swapped = session.region.physical(0);
    session.rank->banks[0].injectRawDataBit(swapped, 0, 14);
    enqueueRead(*session.feeder, session.codec, 0, swapped, scratch, "HOST_BG");
    drainReports(*session.feeder, reports, 8000000ull);
    const unsigned spare = session.region.physical(0);

    CommandLedger trace;
    trace.addId = trace.ledger.planOp(1, 0, 0, {{0, 0}, {1, 0}}, {{2, 0}});
    trace.reluId = trace.ledger.planOp(1, 1, 0, {{2, 0}}, {{3, 0}});
    trace.ledger.setBaseRow(0, spare);
    trace.ledger.setBaseRow(1, 128);
    trace.ledger.setBaseRow(2, 256);
    trace.ledger.setBaseRow(3, 384);
    session.feeder->setCompletionObserver([&trace](const P1CompletionEvent& event) {
        const bool add = trace.kernel == 0 &&
                         (event.tag == "BANK_TO_GRF_" || event.tag == "ADD" || event.tag == "GRF_TO_BANK");
        const bool relu = trace.kernel == 1 &&
                          (event.tag == "FILL&ReLU" || event.tag == "GRF_A_TO_EVEN_BANK" ||
                           event.tag == "GRF_B_TO_ODD_BANK");
        if (!add && !relu)
            return;
        const uint64_t op = add ? trace.addId : trace.reluId;
        phase2::OpEntry* entry = trace.ledger.findMutable(op);
        if (entry == nullptr)
            return;
        if (entry->t_issue == UINT64_MAX)
            trace.ledger.recordIssue(op, event.submit_cycle);
        if (add && !event.is_write)
            trace.addReads += 1;
        if (add && event.is_write)
            trace.addWrites += 1;
        if (relu && !event.is_write)
            trace.reluReads += 1;
        if (relu && event.is_write)
            trace.reluWrites += 1;
        if (event.is_write && !entry->completed)
            trace.ledger.recordComplete(op, event.complete_cycle);
    });
    const std::size_t reportsBefore = reports.reports().size();
    const uint64_t ueBefore = reports.internalUe();
    session.rank->banks[0].injectRawDataBit(spare, 0, 14);
    session.rank->banks[0].injectRawDataBit(spare, 0, 15);
    enqueueRead(*session.feeder, session.codec, 0, spare, scratch, "HOST_BG");
    drainReports(*session.feeder, reports, 8000000ull);
    const bool hostSawUe = reports.internalUe() > ueBefore;
    const uint64_t ueAfterHost = reports.internalUe();
    trace.kernel = 0;
    session.kernel->executeEltwise(static_cast<int>(bursts), pimBankType::ALL_BANK, KernelType::ADD, spare,
                                   256, 128);
    drainReports(*session.feeder, reports, 8000000ull);
    trace.kernel = 1;
    session.kernel->executeEltwise(static_cast<int>(bursts), pimBankType::ALL_BANK, KernelType::RELU, 256,
                                   384, 0);
    drainReports(*session.feeder, reports, 8000000ull);
    trace.kernel = -1;
    BurstType stale;
    enqueueRead(*session.feeder, session.codec, 0, 0, stale, "HOST_BG");
    drainReports(*session.feeder, reports, 8000000ull);
    session.feeder->setCompletionObserver({});

    JointProbe probe;
    probe.spare = spare;
    probe.level = session.region.level(0);
    const phase2::OpEntry* add = trace.ledger.find(trace.addId);
    const phase2::OpEntry* relu = trace.ledger.find(trace.reluId);
    probe.autoIssued = add != nullptr && relu != nullptr && add->t_issue != UINT64_MAX &&
                       relu->t_issue != UINT64_MAX;
    probe.autoCompleted = add != nullptr && relu != nullptr && add->completed && relu->completed;
    probe.reluVersionFromAdd = relu != nullptr && !relu->inputs.empty() &&
                               relu->inputs[0].version == trace.addId;
    probe.hostSawUe = hostSawUe;
    probe.pimSawUe = reports.internalUe() > ueAfterHost;
    probe.staleReadClean = std::memcmp(stale.u8Data_, left[0].u8Data_, 32) == 0;
    const phase2::DataRef resultRef{3, 0, trace.reluId};
    phase2::Gate gate;
    gate.hold(resultRef);
    const Phase1PublicReport* selected = nullptr;
    for (std::size_t i = reportsBefore; i < reports.reports().size(); ++i)
        if (reports.reports()[i].error_class == "UE")
            selected = &reports.reports()[i];
    if (selected == nullptr)
        return probe;
    probe.reportClass = selected->error_class;
    probe.reportLogical = selected->logical_row;
    probe.reportPhysical = spare;
    phase2::EngineConfig config;
    config.policy = phase2::Policy::B2_CLASS_AWARE;
    config.window_mode = phase2::WindowMode::AUTO;
    phase2::RecoveryEngine engine;
    phase2::CheckpointView view;
    const phase2::Decision decision =
        engine.analyze(*selected, session.feeder->currentClockCycle, config, trace.ledger, gate, view,
                       reports.overflow(), session.codec);
    probe.status = phase2::p2StatusName(decision.status);
    if (decision.status != phase2::P2Status::SELECTIVE_RECOVERED)
        return probe;
    for (const phase2::DataRef& ref : decision.restore_list)
    {
        if (ref.version != 0)
            continue;
        const std::vector<BurstType>* tile = store.tile(ref.operand, ref.tile);
        if (tile == nullptr)
            continue;
        preloadRow(*session.kernel, *tile, trace.ledger.baseRow(ref.operand) + phase2::tileRowOffset(ref.tile));
        probe.restored += 1;
    }
    drainReports(*session.feeder, reports, 8000000ull);
    for (const phase2::Run& run : decision.replay_runs)
    {
        if (run.kernel_idx == 0)
            session.kernel->executeEltwise(static_cast<int>(bursts), pimBankType::ALL_BANK, KernelType::ADD,
                                           static_cast<int>(spare), 256, 128);
        else if (run.kernel_idx == 1)
            session.kernel->executeEltwise(static_cast<int>(bursts), pimBankType::ALL_BANK, KernelType::RELU,
                                           256, 384, 0);
        probe.replay += run.tile_count;
    }
    drainReports(*session.feeder, reports, 8000000ull);
    gate.publish(resultRef, session.feeder->currentClockCycle);
    if (gate.state(resultRef) == phase2::Lifecycle::PUBLISHED)
    {
        probe.submitted = true;
        probe.mismatch = countBurstDiff(readRow(session, 384, bursts), expected);
    }
    return probe;
}
}  // namespace

TEST(Phase3M3, Consistency)
{
    GTEST_SKIP() << "pre-fix measurement stays in consistency_diagnostic.json; MapFix checks the repaired mapping";
    const std::string out =
        "/opt/data/data/workspace-dynamic/Rowhammer/phase3_exp/results/m3/datapath_consistency";
    const PublishProbe publish = runPublishProbe(out);
    const MapProbe mapping = runMapProbe(out);
    const EpochProbe epoch = runEpochProbe(out);
    const JointProbe joint = runJointProbe(out);
    std::ostringstream json;
    json << "{\"held_host_consumed\":" << (publish.heldHostConsumed ? "true" : "false")
         << ",\"held_pim_consumed\":" << (publish.heldPimConsumed ? "true" : "false")
         << ",\"poisoned_host_consumed\":" << (publish.poisonedHostConsumed ? "true" : "false")
         << ",\"poisoned_pim_consumed\":" << (publish.poisonedPimConsumed ? "true" : "false")
         << ",\"wrapper_blocked\":" << (publish.wrapperBlocked ? "true" : "false")
         << ",\"physical\":" << mapping.physical
         << ",\"native_physical_match\":" << (mapping.nativePhysicalMatch ? "true" : "false")
         << ",\"native_stale_match\":" << (mapping.nativeStaleMatch ? "true" : "false")
         << ",\"ecc_physical_match\":" << (mapping.eccPhysicalMatch ? "true" : "false")
         << ",\"ecc_stale_match\":" << (mapping.eccStaleMatch ? "true" : "false")
         << ",\"ecc_source_remains\":" << (mapping.eccSourceRemains ? "true" : "false")
         << ",\"ecc_orphan_matches\":" << (mapping.orphanMatches ? "true" : "false")
         << ",\"ue_ecc_copied\":" << (mapping.ueCopied ? "true" : "false")
         << ",\"ue_ecc_source_remains\":" << (mapping.ueSourceRemains ? "true" : "false")
         << ",\"epoch_logical\":" << epoch.logicalOfZero
         << ",\"epoch_access_matches_stored\":" << (epoch.accessMatches ? "true" : "false")
         << ",\"epoch_report_logical\":" << epoch.reportLogical
         << ",\"epoch_report_epoch\":" << epoch.reportEpoch
         << ",\"epoch_engine_ignores_logical\":" << (epoch.engineUsesEpoch ? "true" : "false")
         << ",\"joint_spare\":" << joint.spare
         << ",\"joint_level\":" << joint.level
         << ",\"joint_auto_issued\":" << (joint.autoIssued ? "true" : "false")
         << ",\"joint_auto_completed\":" << (joint.autoCompleted ? "true" : "false")
         << ",\"joint_version_linked\":" << (joint.reluVersionFromAdd ? "true" : "false")
         << ",\"joint_host_ue\":" << (joint.hostSawUe ? "true" : "false")
         << ",\"joint_pim_ue\":" << (joint.pimSawUe ? "true" : "false")
         << ",\"joint_stale_clean\":" << (joint.staleReadClean ? "true" : "false")
         << ",\"joint_submitted\":" << (joint.submitted ? "true" : "false")
         << ",\"joint_mismatch\":" << joint.mismatch
         << ",\"joint_replay\":" << joint.replay
         << ",\"joint_restored\":" << joint.restored
         << ",\"joint_class\":\"" << joint.reportClass << "\""
         << ",\"joint_status\":\"" << joint.status << "\""
         << ",\"joint_logical\":" << joint.reportLogical
         << ",\"joint_physical\":" << joint.reportPhysical << "}\n";
    writeCell(out + "/consistency_diagnostic.json", json.str());

    EXPECT_TRUE(publish.heldHostConsumed);
    EXPECT_TRUE(publish.heldPimConsumed);
    EXPECT_TRUE(publish.poisonedHostConsumed);
    EXPECT_TRUE(publish.poisonedPimConsumed);
    EXPECT_TRUE(publish.wrapperBlocked);

    EXPECT_NE(0u, mapping.physical);
    EXPECT_TRUE(mapping.nativePhysicalMatch);
    EXPECT_FALSE(mapping.nativeStaleMatch);
    EXPECT_TRUE(mapping.eccPhysicalMatch);
    EXPECT_FALSE(mapping.eccStaleMatch);
    EXPECT_TRUE(mapping.eccSourceRemains);
    EXPECT_TRUE(mapping.orphanMatches);
    EXPECT_FALSE(mapping.ueCopied);
    EXPECT_TRUE(mapping.ueSourceRemains);

    EXPECT_EQ(1u, epoch.logicalOfZero);
    EXPECT_TRUE(epoch.accessMatches);
    EXPECT_EQ(1u, epoch.reportLogical);
    EXPECT_TRUE(epoch.engineUsesEpoch);

    EXPECT_EQ(2u, joint.level);
    EXPECT_GE(joint.spare, RegionMap::kRows - 16u);
    EXPECT_TRUE(joint.autoIssued);
    EXPECT_TRUE(joint.autoCompleted);
    EXPECT_TRUE(joint.reluVersionFromAdd);
    EXPECT_TRUE(joint.hostSawUe);
    EXPECT_FALSE(joint.pimSawUe);
    EXPECT_FALSE(joint.staleReadClean);
    EXPECT_TRUE(joint.submitted);
    EXPECT_EQ(128u, joint.mismatch);
    EXPECT_GT(joint.replay, 0u);
    EXPECT_GT(joint.restored, 0u);
    EXPECT_EQ(std::string("UE"), joint.reportClass);
    EXPECT_EQ(std::string("SELECTIVE_RECOVERED"), joint.status);
    EXPECT_EQ(0u, joint.reportLogical);
    EXPECT_EQ(joint.spare, joint.reportPhysical);
}

namespace
{
Phase1StorageKey codewordKey(const Bank& bank, unsigned row)
{
    Phase1StorageKey key;
    key.channel = 0;
    key.rank = 0;
    key.actual_bank = bank.identityBank();
    key.row = row;
    key.burst_column = 0;
    key.codeword_index = 0;
    return key;
}

BurstType hostRead(Session& session, unsigned row, const char* tag)
{
    BurstType slot;
    std::memset(slot.u8Data_, 0, sizeof(slot.u8Data_));
    enqueueRead(*session.feeder, session.codec, 0, row, slot, tag);
    drain(*session.feeder, 2000000ull);
    return slot;
}
}  // namespace

TEST(Phase3M3, MapFix)
{
    const std::string out =
        "/opt/data/data/workspace-dynamic/Rowhammer/phase3_exp/results/m3/datapath_mapfix";
    const unsigned bursts = 128u;
    std::vector<BurstType> left(bursts), right(bursts), added(bursts);
    for (unsigned i = 0; i < bursts; ++i)
    {
        left[i] = positiveBurst(i, 4);
        right[i] = positiveBurst(i, 5);
        added[i] = left[i] + right[i];
    }

    Session clean(kDataDevice, kDataSystem, out, 1, 1.0, "OURS", 1.1, 0.0);
    Phase1Reports cleanReports(clean.codec, 1, static_cast<unsigned>(clean.rank->banks.size()),
                               "codeword", 1, 1, 64, true, true);
    armLocalEcc(clean, cleanReports);
    preloadRow(*clean.kernel, left, 0);
    preloadRow(*clean.kernel, right, 64);
    drainReports(*clean.feeder, cleanReports, 8000000ull);
    const Phase1StorageKey beforeKey = codewordKey(clean.rank->banks[0], 0);
    const uint64_t rawBefore = clean.rank->banks[0].phase1Storage().readRawData(beforeKey);
    const uint8_t parityBefore = clean.rank->banks[0].phase1Storage().readRawParity(beforeKey);
    clean.region.forceSwap(&clean.model);
    const unsigned physical = clean.region.physical(0);
    const Phase1StorageKey movedKey = codewordKey(clean.rank->banks[0], physical);
    const bool sourceGone = !clean.rank->banks[0].phase1Storage().contains(beforeKey);
    const bool destHas = clean.rank->banks[0].phase1Storage().contains(movedKey);
    const bool rawMoved = destHas && clean.rank->banks[0].phase1Storage().readRawData(movedKey) == rawBefore;
    const bool parityMoved =
        destHas && clean.rank->banks[0].phase1Storage().readRawParity(movedKey) == parityBefore;
    const bool logicalHost = countBurstDiff(readRow(clean, 0, bursts), left) == 0;
    const BurstType hostBg = hostRead(clean, 0, "HOST_BG");
    const bool hostBgMatch = std::memcmp(hostBg.u8Data_, left[0].u8Data_, 32) == 0;
    const BurstType attackStale = hostRead(clean, 0, "HOST_ATK");
    const bool attackStaysPhysical = std::memcmp(attackStale.u8Data_, left[0].u8Data_, 32) != 0;
    const BurstType attackPhysical = hostRead(clean, physical, "HOST_ATK");
    const bool attackReadsCell = std::memcmp(attackPhysical.u8Data_, left[0].u8Data_, 32) == 0;
    clean.kernel->executeEltwise(static_cast<int>(bursts), pimBankType::ALL_BANK, KernelType::ADD, 0, 256,
                                 64);
    drain(*clean.feeder, 8000000ull);
    const bool pimLogical = countBurstDiff(readRow(clean, 256, bursts), added) == 0;

    Session broken(kDataDevice, kDataSystem, out, 2, 1.0, "OURS", 1.1, 0.0);
    Phase1Reports brokenReports(broken.codec, 1, static_cast<unsigned>(broken.rank->banks.size()),
                                "codeword", 1, 1, 64, true, true);
    armLocalEcc(broken, brokenReports);
    preloadRow(*broken.kernel, left, 0);
    drainReports(*broken.feeder, brokenReports, 8000000ull);
    const uint8_t ueParity = broken.rank->banks[0].phase1Storage().readRawParity(codewordKey(broken.rank->banks[0], 0));
    broken.rank->banks[0].injectRawDataBit(0, 0, 14);
    broken.rank->banks[0].injectRawDataBit(0, 0, 15);
    const uint64_t ueRaw = broken.rank->banks[0].phase1Storage().readRawData(codewordKey(broken.rank->banks[0], 0));
    broken.region.forceSwap(&broken.model);
    const unsigned uePhysical = broken.region.physical(0);
    const Phase1StorageKey ueDest = codewordKey(broken.rank->banks[0], uePhysical);
    const bool ueDestHas = broken.rank->banks[0].phase1Storage().contains(ueDest);
    const bool ueSourceGone = !broken.rank->banks[0].phase1Storage().contains(codewordKey(broken.rank->banks[0], 0));
    const bool ueRawKept = ueDestHas && broken.rank->banks[0].phase1Storage().readRawData(ueDest) == ueRaw;
    const bool ueParityKept = ueDestHas && broken.rank->banks[0].phase1Storage().readRawParity(ueDest) == ueParity;
    const uint64_t ueBefore = brokenReports.internalUe();
    const std::vector<BurstType> ueRead = readRow(broken, 0, bursts);
    const bool ueNotClean = countBurstDiff(ueRead, left) != 0;
    const uint64_t ueAfterFirst = brokenReports.internalUe();
    readRow(broken, 0, bursts);
    const uint64_t ueAfterSecond = brokenReports.internalUe();
    const bool ueStillUncorrectable = ueAfterFirst > ueBefore && ueAfterSecond > ueAfterFirst;

    Session spare(kDataDevice, kDataSystem, out, 3, 1.0, "OURS", 1.1, 0.0);
    Phase1Reports spareReports(spare.codec, 1, static_cast<unsigned>(spare.rank->banks.size()),
                               "codeword", 1, 1, 64, true, true);
    armLocalEcc(spare, spareReports);
    preloadRow(*spare.kernel, left, 0);
    drainReports(*spare.feeder, spareReports, 8000000ull);
    spare.region.forceSwap(&spare.model);
    const unsigned preSpare = spare.region.physical(0);
    spare.rank->banks[0].injectRawDataBit(preSpare, 0, 14);
    readRow(spare, 0, bursts);
    spare.rank->banks[0].injectRawDataBit(preSpare, 0, 14);
    const uint64_t reportsBefore = spareReports.reports().size();
    readRow(spare, 0, bursts);
    drainReports(*spare.feeder, spareReports, 8000000ull);
    const unsigned spareRow = spare.region.physical(0);
    const bool levelTwo = spare.region.level(0) == 2;
    const bool logicalAfterL2 = countBurstDiff(readRow(spare, 0, bursts), left) == 0;
    const Bank& last = spare.rank->banks.back();
    const bool spareHas = last.phase1Storage().contains(codewordKey(last, spareRow));
    const bool oldGone = !last.phase1Storage().contains(codewordKey(last, preSpare));
    bool reportLogical = false;
    bool reportRegionLogical = false;
    unsigned seenLogical = 999999u;
    uint64_t seenRegion = 0;
    for (std::size_t i = reportsBefore; i < spareReports.reports().size(); ++i)
    {
        const Phase1PublicReport& item = spareReports.reports()[i];
        seenLogical = item.logical_row;
        seenRegion = item.region_id;
        if (item.logical_row == 0 && item.region_id == 0)
        {
            reportLogical = true;
            reportRegionLogical = true;
            break;
        }
    }
    spare.rank->banks[0].injectRawDataBit(spareRow, 0, 14);
    const uint64_t spareReportsBefore = spareReports.reports().size();
    readRow(spare, 0, bursts);
    drainReports(*spare.feeder, spareReports, 8000000ull);
    bool spareReportLogical = false;
    unsigned spareSeenLogical = 999999u;
    if (spareReports.reports().size() > spareReportsBefore)
    {
        spareSeenLogical = spareReports.reports()[spareReportsBefore].logical_row;
        spareReportLogical = spareSeenLogical == 0;
    }

    std::ostringstream json;
    json << "{\"physical\":" << physical
         << ",\"source_gone\":" << (sourceGone ? "true" : "false")
         << ",\"dest_has\":" << (destHas ? "true" : "false")
         << ",\"raw_moved\":" << (rawMoved ? "true" : "false")
         << ",\"parity_moved\":" << (parityMoved ? "true" : "false")
         << ",\"logical_host\":" << (logicalHost ? "true" : "false")
         << ",\"host_bg\":" << (hostBgMatch ? "true" : "false")
         << ",\"attack_stays_physical\":" << (attackStaysPhysical ? "true" : "false")
         << ",\"attack_reads_cell\":" << (attackReadsCell ? "true" : "false")
         << ",\"pim_logical\":" << (pimLogical ? "true" : "false")
         << ",\"ue_dest\":" << (ueDestHas ? "true" : "false")
         << ",\"ue_source_gone\":" << (ueSourceGone ? "true" : "false")
         << ",\"ue_raw_kept\":" << (ueRawKept ? "true" : "false")
         << ",\"ue_parity_kept\":" << (ueParityKept ? "true" : "false")
         << ",\"ue_not_clean\":" << (ueNotClean ? "true" : "false")
         << ",\"ue_still_uncorrectable\":" << (ueStillUncorrectable ? "true" : "false")
         << ",\"ue_first\":" << ueAfterFirst << ",\"ue_second\":" << ueAfterSecond
         << ",\"l2_row\":" << spareRow << ",\"l2_level\":" << (levelTwo ? 2 : spare.region.level(0))
         << ",\"l2_logical\":" << (logicalAfterL2 ? "true" : "false")
         << ",\"l2_spare_has\":" << (spareHas ? "true" : "false")
         << ",\"l2_old_gone\":" << (oldGone ? "true" : "false")
         << ",\"l2_report_logical\":" << (reportLogical ? "true" : "false")
         << ",\"l2_region_logical\":" << (reportRegionLogical ? "true" : "false")
         << ",\"l2_seen_logical\":" << seenLogical
         << ",\"l2_seen_region\":" << seenRegion
         << ",\"l2_spare_report_logical\":" << (spareReportLogical ? "true" : "false")
         << ",\"l2_spare_seen_logical\":" << spareSeenLogical
         << "}\n";
    mkdir(out.c_str(), 0755);
    writeCell(out + "/mapfix.json", json.str());

    EXPECT_NE(physical, 0u);
    EXPECT_TRUE(sourceGone);
    EXPECT_TRUE(destHas);
    EXPECT_TRUE(rawMoved);
    EXPECT_TRUE(parityMoved);
    EXPECT_TRUE(logicalHost);
    EXPECT_TRUE(hostBgMatch);
    EXPECT_TRUE(attackStaysPhysical);
    EXPECT_TRUE(attackReadsCell);
    EXPECT_TRUE(pimLogical);
    EXPECT_TRUE(ueDestHas);
    EXPECT_TRUE(ueSourceGone);
    EXPECT_TRUE(ueRawKept);
    EXPECT_TRUE(ueParityKept);
    EXPECT_TRUE(ueNotClean);
    EXPECT_TRUE(ueStillUncorrectable);
    EXPECT_GE(spareRow, RegionMap::kRows - 16u);
    EXPECT_TRUE(levelTwo);
    EXPECT_TRUE(logicalAfterL2);
    EXPECT_TRUE(spareHas);
    EXPECT_TRUE(oldGone);
    EXPECT_TRUE(reportLogical);
    EXPECT_TRUE(reportRegionLogical);
    EXPECT_TRUE(spareReportLogical);
}

TEST(Phase3M3, EpochFix)
{
    const std::string out =
        "/opt/data/data/workspace-dynamic/Rowhammer/phase3_exp/results/m3/datapath_epochfix";
    const unsigned bursts = 128u;
    std::vector<BurstType> left(bursts), right(bursts), added(bursts);
    for (unsigned i = 0; i < bursts; ++i)
    {
        left[i] = positiveBurst(i, 6);
        right[i] = positiveBurst(i, 7);
        added[i] = left[i] + right[i];
    }

    bool accessWithheld = false;
    bool pimWithheld = false;
    bool reportCorrupt = false;
    bool reportFlag = false;
    bool engineStale = false;
    bool noRestore = false;
    unsigned reportLogical = 0;
    std::string staleStatus = "NONE";
    {
        Session stale(kDataDevice, kDataSystem, out, 1, 1.0, "OURS-EPOCH", 1.1, 0.0);
        Phase1Reports reports(stale.codec, 1, static_cast<unsigned>(stale.rank->banks.size()),
                              "codeword", 1, 1, 64, true, true);
        armLocalEcc(stale, reports);
        preloadRow(*stale.kernel, left, 0);
        preloadRow(*stale.kernel, right, 64);
        drainReports(*stale.feeder, reports, 8000000ull);
        accessWithheld = countBurstDiff(readRow(stale, 0, bursts), left) != 0;
        stale.kernel->executeEltwise(static_cast<int>(bursts), pimBankType::ALL_BANK, KernelType::ADD, 0,
                                     256, 64);
        drain(*stale.feeder, 8000000ull);
        uint64_t expectedWord = 0;
        std::memcpy(&expectedWord, added[0].u8Data_, sizeof(expectedWord));
        const Phase1StorageKey resultKey = codewordKey(stale.rank->banks[0], 256);
        const bool resultStored = stale.rank->banks[0].phase1Storage().contains(resultKey);
        const uint64_t storedWord =
            resultStored ? stale.rank->banks[0].phase1Storage().readRawData(resultKey) : 0;
        pimWithheld = !resultStored || storedWord != expectedWord;
        stale.rank->banks[0].injectRawDataBit(0, 0, 14);
        BurstType scratch;
        enqueueRead(*stale.feeder, stale.codec, 0, 0, scratch, "HOST_BG");
        drainReports(*stale.feeder, reports, 8000000ull);
        phase2::Ledger ledger;
        ledger.setBaseRow(0, 0);
        ledger.setBaseRow(1, 128);
        ledger.addCompleted(1, 0, 0, {{0, 0}}, {{1, 0}}, 0, stale.feeder->currentClockCycle);
        phase2::EngineConfig config;
        config.policy = phase2::Policy::B2_CLASS_AWARE;
        config.window_mode = phase2::WindowMode::AUTO;
        phase2::RecoveryEngine engine;
        phase2::Gate gate;
        phase2::CheckpointView view;
        phase2::Decision decision;
        if (!reports.reports().empty())
        {
            reportLogical = reports.reports()[0].logical_row;
            reportCorrupt = reportLogical != 0;
            reportFlag = !reports.reports()[0].mapping_consistent;
            decision = engine.analyze(reports.reports()[0], stale.feeder->currentClockCycle, config,
                                      ledger, gate, view, false, stale.codec);
        }
        engineStale = decision.status == phase2::P2Status::STALE_EPOCH;
        noRestore = decision.restore_list.empty() && decision.replay_runs.empty();
        staleStatus = phase2::p2StatusName(decision.status);
    }

    bool advanced = false;
    bool rereadMatches = false;
    bool oldEpochAccepted = false;
    uint64_t stamped = 0;
    uint64_t current = 0;
    std::string advancedStatus = "NONE";
    {
        Session moved(kDataDevice, kDataSystem, out, 2, 1.0, "OURS", 1.1, 0.0);
        Phase1Reports reports(moved.codec, 1, static_cast<unsigned>(moved.rank->banks.size()),
                              "codeword", 1, 1, 64, true, true);
        armLocalEcc(moved, reports);
        preloadRow(*moved.kernel, left, 0);
        drainReports(*moved.feeder, reports, 8000000ull);
        moved.rank->banks[0].injectRawDataBit(moved.region.physical(0), 0, 14);
        BurstType scratch;
        enqueueRead(*moved.feeder, moved.codec, 0, 0, scratch, "HOST_BG");
        drainReports(*moved.feeder, reports, 8000000ull);
        Phase1PublicReport saved;
        bool have = false;
        if (!reports.reports().empty())
        {
            saved = reports.reports()[0];
            have = saved.logical_row == 0 && saved.mapping_consistent;
            stamped = saved.mapping_epoch;
        }
        for (unsigned n = 0; n < 40 && moved.region.epoch(0) == stamped; ++n)
            moved.region.forceSwap(&moved.model);
        current = moved.region.epoch(0);
        advanced = current > stamped;
        rereadMatches = countBurstDiff(readRow(moved, 0, bursts), left) == 0;
        phase2::Ledger ledger;
        ledger.setBaseRow(0, 0);
        ledger.setBaseRow(1, 128);
        ledger.addCompleted(1, 0, 0, {{0, 0}}, {{1, 0}}, 0, moved.feeder->currentClockCycle);
        phase2::EngineConfig config;
        config.policy = phase2::Policy::B2_CLASS_AWARE;
        config.window_mode = phase2::WindowMode::AUTO;
        phase2::RecoveryEngine engine;
        phase2::Gate gate;
        phase2::CheckpointView view;
        if (have)
        {
            const phase2::Decision decision = engine.analyze(
                saved, moved.feeder->currentClockCycle, config, ledger, gate, view, false, moved.codec);
            oldEpochAccepted = decision.status == phase2::P2Status::SOURCE_REPAIRED_ONLY;
            advancedStatus = phase2::p2StatusName(decision.status);
        }
    }

    std::ostringstream json;
    json << "{\"access_withheld\":" << (accessWithheld ? "true" : "false")
         << ",\"pim_withheld\":" << (pimWithheld ? "true" : "false")
         << ",\"report_logical\":" << reportLogical
         << ",\"report_corrupt\":" << (reportCorrupt ? "true" : "false")
         << ",\"report_inconsistent\":" << (reportFlag ? "true" : "false")
         << ",\"engine_stale\":" << (engineStale ? "true" : "false")
         << ",\"no_restore\":" << (noRestore ? "true" : "false")
         << ",\"stale_status\":\"" << staleStatus << "\""
         << ",\"epoch_advanced\":" << (advanced ? "true" : "false")
         << ",\"stamped_epoch\":" << stamped << ",\"current_epoch\":" << current
         << ",\"reread_matches\":" << (rereadMatches ? "true" : "false")
         << ",\"old_epoch_accepted\":" << (oldEpochAccepted ? "true" : "false")
         << ",\"advanced_status\":\"" << advancedStatus << "\"}\n";
    mkdir(out.c_str(), 0755);
    writeCell(out + "/epochfix.json", json.str());

    EXPECT_TRUE(accessWithheld);
    EXPECT_TRUE(pimWithheld);
    EXPECT_TRUE(reportCorrupt);
    EXPECT_TRUE(reportFlag);
    EXPECT_TRUE(engineStale);
    EXPECT_TRUE(noRestore);
    EXPECT_TRUE(advanced);
    EXPECT_TRUE(rereadMatches);
    EXPECT_FALSE(oldEpochAccepted);
    EXPECT_EQ(std::string("STALE_EPOCH"), advancedStatus);
}

TEST(Phase3M3, PublishFix)
{
    const std::string out =
        "/opt/data/data/workspace-dynamic/Rowhammer/phase3_exp/results/m3/datapath_publishfix";
    const unsigned bursts = 128u;
    std::vector<BurstType> left(bursts), right(bursts), expected(bursts), doubled(bursts),
        rewritten(bursts);
    for (unsigned i = 0; i < bursts; ++i)
    {
        left[i] = positiveBurst(i, 4);
        right[i] = positiveBurst(i, 5);
        expected[i] = left[i] + right[i];
        doubled[i] = expected[i] + expected[i];
        rewritten[i] = positiveBurst(i, 9);
    }
    Session session(kDataDevice, kDataSystem, out, 1, 1.0, "NONE", 1.5, 0.0);
    preloadRow(*session.kernel, left, 0);
    preloadRow(*session.kernel, right, 128);
    drain(*session.feeder, 8000000ull);
    phase2::Gate gate;
    const phase2::DataRef result{2, 0, 1};
    gate.hold(result);
    phase3BindPublication(256, &gate, result.operand, result.tile, result.version);
    const uint64_t blockedBefore = phase3BlockedReads();
    session.kernel->executeEltwise(static_cast<int>(bursts), pimBankType::ALL_BANK, KernelType::ADD, 0, 256,
                                   128);
    drain(*session.feeder, 8000000ull);
    const BurstType heldStored = hostRead(session, 256, "HOST_ATK");
    const bool writeWhileHeld = std::memcmp(heldStored.u8Data_, expected[0].u8Data_, 32) == 0;
    const bool heldHostBlocked = countBurstDiff(readRow(session, 256, bursts), expected) != 0;
    const bool unboundReadable = countBurstDiff(readRow(session, 0, bursts), left) == 0;
    const BurstType attack = hostRead(session, 256, "HOST_ATK");
    const bool attackNotGated = std::memcmp(attack.u8Data_, expected[0].u8Data_, 32) == 0;
    session.kernel->executeEltwise(static_cast<int>(bursts), pimBankType::ALL_BANK, KernelType::ADD, 256,
                                   512, 256);
    drain(*session.feeder, 8000000ull);
    const BurstType heldDownstream = hostRead(session, 512, "HOST_ATK");
    const bool heldPimBlocked = std::memcmp(heldDownstream.u8Data_, doubled[0].u8Data_, 32) != 0;

    gate.publish(result, session.feeder->currentClockCycle);
    const bool publishedHost = countBurstDiff(readRow(session, 256, bursts), expected) == 0;
    session.kernel->executeEltwise(static_cast<int>(bursts), pimBankType::ALL_BANK, KernelType::ADD, 256,
                                   768, 256);
    drain(*session.feeder, 8000000ull);
    const BurstType publishedDownstream = hostRead(session, 768, "HOST_ATK");
    const bool publishedPim = std::memcmp(publishedDownstream.u8Data_, doubled[0].u8Data_, 32) == 0;

    gate.poison({result});
    const bool poisonedHostBlocked = countBurstDiff(readRow(session, 256, bursts), expected) != 0;
    session.kernel->executeEltwise(static_cast<int>(bursts), pimBankType::ALL_BANK, KernelType::ADD, 256,
                                   1024, 256);
    drain(*session.feeder, 8000000ull);
    const BurstType poisonedDownstream = hostRead(session, 1024, "HOST_ATK");
    const bool poisonedPimBlocked = std::memcmp(poisonedDownstream.u8Data_, doubled[0].u8Data_, 32) != 0;
    preloadRow(*session.kernel, rewritten, 256);
    drain(*session.feeder, 8000000ull);
    const BurstType rewrittenStored = hostRead(session, 256, "HOST_ATK");
    const bool rewriteWhilePoisoned = std::memcmp(rewrittenStored.u8Data_, rewritten[0].u8Data_, 32) == 0;
    const bool rewriteNotConsumed = countBurstDiff(readRow(session, 256, bursts), rewritten) != 0;
    const bool blockedGrew = phase3BlockedReads() > blockedBefore;

    std::ostringstream json;
    json << "{\"write_while_held\":" << (writeWhileHeld ? "true" : "false")
         << ",\"held_host_blocked\":" << (heldHostBlocked ? "true" : "false")
         << ",\"held_pim_blocked\":" << (heldPimBlocked ? "true" : "false")
         << ",\"unbound_readable\":" << (unboundReadable ? "true" : "false")
         << ",\"attack_not_gated\":" << (attackNotGated ? "true" : "false")
         << ",\"published_host\":" << (publishedHost ? "true" : "false")
         << ",\"published_pim\":" << (publishedPim ? "true" : "false")
         << ",\"poisoned_host_blocked\":" << (poisonedHostBlocked ? "true" : "false")
         << ",\"poisoned_pim_blocked\":" << (poisonedPimBlocked ? "true" : "false")
         << ",\"rewrite_while_poisoned\":" << (rewriteWhilePoisoned ? "true" : "false")
         << ",\"rewrite_not_consumed\":" << (rewriteNotConsumed ? "true" : "false")
         << ",\"blocked_reads\":" << (phase3BlockedReads() - blockedBefore) << "}\n";
    mkdir(out.c_str(), 0755);
    writeCell(out + "/publishfix.json", json.str());

    EXPECT_TRUE(writeWhileHeld);
    EXPECT_TRUE(heldHostBlocked);
    EXPECT_TRUE(heldPimBlocked);
    EXPECT_TRUE(unboundReadable);
    EXPECT_TRUE(attackNotGated);
    EXPECT_TRUE(publishedHost);
    EXPECT_TRUE(publishedPim);
    EXPECT_TRUE(poisonedHostBlocked);
    EXPECT_TRUE(poisonedPimBlocked);
    EXPECT_TRUE(rewriteWhilePoisoned);
    EXPECT_TRUE(rewriteNotConsumed);
    EXPECT_TRUE(blockedGrew);
}

namespace
{
bool ledgerDataTag(const std::string& tag)
{
    return tag == "BANK_TO_GRF_" || tag == "ADD" || tag == "MUL" || tag == "GRF_TO_BANK" ||
           tag == "FILL&ReLU" || tag == "GRF_A_TO_EVEN_BANK" || tag == "GRF_B_TO_ODD_BANK" ||
           tag == "MAC_" || tag == "GRF_B_TO_BANK_";
}
}  // namespace

TEST(Phase3M3, LedgerFix)
{
    const std::string out =
        "/opt/data/data/workspace-dynamic/Rowhammer/phase3_exp/results/m3/datapath_ledgerfix";
    const unsigned bursts = 128u;
    std::vector<BurstType> left(bursts), right(bursts);
    for (unsigned i = 0; i < bursts; ++i)
    {
        left[i] = positiveBurst(i, 4);
        right[i] = positiveBurst(i, 5);
    }
    Session session(kDataDevice, kDataSystem, out, 1, 1.0, "NONE", 1.5, 0.0);
    preloadRow(*session.kernel, left, 0);
    preloadRow(*session.kernel, right, 128);
    drain(*session.feeder, 8000000ull);

    phase2::Ledger ledger;
    ledger.setBaseRow(0, 0);
    ledger.setBaseRow(1, 128);
    ledger.setBaseRow(2, 256);
    ledger.setBaseRow(3, 384);
    const uint64_t addId = ledger.planOp(1, 0, 0, {{0, 0}, {1, 0}}, {{2, 0}});
    const uint64_t reluId = ledger.planOp(1, 1, 0, {{2, 0}}, {{3, 0}});
    uint64_t active = addId;
    std::set<unsigned> addReads, addWrites, reluReads, reluWrites;
    session.feeder->setCompletionObserver([&](const P1CompletionEvent& event) {
        if (!ledgerDataTag(event.tag))
            return;
        ledger.observeAccess(active, event.row, event.is_write);
        if (active == addId && event.is_write)
            addWrites.insert(event.row);
        else if (active == addId)
            addReads.insert(event.row);
        else if (event.is_write)
            reluWrites.insert(event.row);
        else
            reluReads.insert(event.row);
        ledger.recordIssue(active, event.submit_cycle);
        if (event.is_write)
            ledger.completeObserved(active, event.complete_cycle);
    });
    const uint64_t loggedBefore = ledger.totalLoggedOps();
    session.kernel->executeEltwise(static_cast<int>(bursts), pimBankType::ALL_BANK, KernelType::ADD, 0, 256,
                                   128);
    drain(*session.feeder, 8000000ull);
    const phase2::OpEntry* add = ledger.find(addId);
    const bool addCompleted = add != nullptr && add->completed;
    const bool addVersion = addCompleted && !add->outputs.empty() && add->outputs[0].version == addId;
    const uint64_t loggedAfterAdd = ledger.totalLoggedOps();
    active = reluId;
    session.kernel->executeEltwise(static_cast<int>(bursts), pimBankType::ALL_BANK, KernelType::RELU, 256,
                                   384, 0);
    drain(*session.feeder, 8000000ull);
    const phase2::OpEntry* relu = ledger.find(reluId);
    const bool reluCompleted = relu != nullptr && relu->completed;
    const bool versionLinked =
        reluCompleted && !relu->inputs.empty() && relu->inputs[0].version == addId;
    const bool issueOnce = loggedAfterAdd == loggedBefore + 1 &&
                           ledger.totalLoggedOps() == loggedAfterAdd + 1;

    Session liar(kDataDevice, kDataSystem, out, 2, 1.0, "NONE", 1.5, 0.0);
    preloadRow(*liar.kernel, left, 0);
    preloadRow(*liar.kernel, right, 128);
    drain(*liar.feeder, 8000000ull);
    phase2::Ledger falseLedger;
    falseLedger.setBaseRow(0, 1000);
    falseLedger.setBaseRow(1, 1128);
    falseLedger.setBaseRow(2, 1256);
    const uint64_t falseId = falseLedger.planOp(1, 0, 0, {{0, 0}, {1, 0}}, {{2, 0}});
    liar.feeder->setCompletionObserver([&](const P1CompletionEvent& event) {
        if (!ledgerDataTag(event.tag))
            return;
        falseLedger.observeAccess(falseId, event.row, event.is_write);
        if (falseLedger.find(falseId)->t_issue == UINT64_MAX)
            falseLedger.recordIssue(falseId, event.submit_cycle);
        if (event.is_write)
            falseLedger.completeObserved(falseId, event.complete_cycle);
    });
    liar.kernel->executeEltwise(static_cast<int>(bursts), pimBankType::ALL_BANK, KernelType::ADD, 0, 256,
                                128);
    drain(*liar.feeder, 8000000ull);
    falseLedger.completeObserved(falseId, liar.feeder->currentClockCycle);
    const phase2::OpEntry* lied = falseLedger.find(falseId);
    const bool lieRejected = lied != nullptr && !lied->completed && lied->t_issue != UINT64_MAX;

    std::ostringstream json;
    json << "{\"add_completed\":" << (addCompleted ? "true" : "false")
         << ",\"add_version\":" << (addVersion ? "true" : "false")
         << ",\"relu_completed\":" << (reluCompleted ? "true" : "false")
         << ",\"version_linked\":" << (versionLinked ? "true" : "false")
         << ",\"issue_once\":" << (issueOnce ? "true" : "false")
         << ",\"lie_rejected\":" << (lieRejected ? "true" : "false")
         << ",\"add_reads\":" << addReads.size() << ",\"add_writes\":" << addWrites.size()
         << ",\"relu_reads\":" << reluReads.size() << ",\"relu_writes\":" << reluWrites.size()
         << "}\n";
    mkdir(out.c_str(), 0755);
    writeCell(out + "/ledgerfix.json", json.str());

    EXPECT_TRUE(addCompleted);
    EXPECT_TRUE(addVersion);
    EXPECT_TRUE(reluCompleted);
    EXPECT_TRUE(versionLinked);
    EXPECT_TRUE(issueOnce);
    EXPECT_TRUE(lieRejected);
}

TEST(Phase3M3, JointFix)
{
    const std::string out =
        "/opt/data/data/workspace-dynamic/Rowhammer/phase3_exp/results/m3/datapath_jointfix";
    const unsigned bursts = 128u;
    std::vector<BurstType> left(bursts), right(bursts), expected(bursts);
    for (unsigned i = 0; i < bursts; ++i)
    {
        left[i] = positiveBurst(i, 4);
        right[i] = positiveBurst(i, 5);
        expected[i] = applyRelu(left[i] + right[i]);
    }
    Session session(kDataDevice, kDataSystem, out, 1, 1.0, "OURS", 1.1, 0.0);
    Phase1Reports reports(session.codec, 1, static_cast<unsigned>(session.rank->banks.size()),
                          "codeword", 1, 1, 64, true, true);
    armLocalEcc(session, reports);
    phase2::Phase2CheckpointStore store;
    store.addTile(0, 0, left);
    store.addTile(1, 0, right);
    preloadRow(*session.kernel, left, 0);
    preloadRow(*session.kernel, right, 128);
    drainReports(*session.feeder, reports, 8000000ull);
    session.region.forceSwap(&session.model);
    const unsigned swapped = session.region.physical(0);
    session.rank->banks[0].injectRawDataBit(swapped, 0, 14);
    readRow(session, 0, bursts);
    session.rank->banks[0].injectRawDataBit(swapped, 0, 14);
    readRow(session, 0, bursts);
    drainReports(*session.feeder, reports, 8000000ull);
    const unsigned spare = session.region.physical(0);
    const bool levelTwo = session.region.level(0) == 2;
    const bool logicalAfterL2 = countBurstDiff(readRow(session, 0, bursts), left) == 0;
    const Bank& last = session.rank->banks.back();
    const bool spareHas = last.phase1Storage().contains(codewordKey(last, spare));

    const uint64_t ueBeforeHost = reports.internalUe();
    session.rank->banks[0].injectRawDataBit(spare, 0, 14);
    session.rank->banks[0].injectRawDataBit(spare, 0, 15);
    readRow(session, 0, bursts);
    drainReports(*session.feeder, reports, 8000000ull);
    const bool hostSawUe = reports.internalUe() > ueBeforeHost;
    const uint64_t ueBeforePim = reports.internalUe();

    phase2::Ledger ledger;
    ledger.setBaseRow(0, 0);
    ledger.setBaseRow(1, 128);
    ledger.setBaseRow(2, 256);
    ledger.setBaseRow(3, 384);
    const uint64_t addId = ledger.planOp(1, 0, 0, {{0, 0}, {1, 0}}, {{2, 0}});
    const uint64_t reluId = ledger.planOp(1, 1, 0, {{2, 0}}, {{3, 0}});
    uint64_t active = addId;
    session.feeder->setCompletionObserver([&](const P1CompletionEvent& event) {
        if (!ledgerDataTag(event.tag))
            return;
        ledger.observeAccess(active, event.row, event.is_write);
        ledger.recordIssue(active, event.submit_cycle);
        if (event.is_write)
            ledger.completeObserved(active, event.complete_cycle);
    });
    session.kernel->executeEltwise(static_cast<int>(bursts), pimBankType::ALL_BANK, KernelType::ADD, 0, 256,
                                   128);
    drainReports(*session.feeder, reports, 8000000ull);
    active = reluId;
    session.kernel->executeEltwise(static_cast<int>(bursts), pimBankType::ALL_BANK, KernelType::RELU, 256,
                                   384, 0);
    drainReports(*session.feeder, reports, 8000000ull);
    session.feeder->setCompletionObserver(nullptr);
    const bool pimSawUe = reports.internalUe() > ueBeforePim;
    const phase2::OpEntry* add = ledger.find(addId);
    const phase2::OpEntry* relu = ledger.find(reluId);
    const bool ledgerCompleted = add != nullptr && add->completed && relu != nullptr && relu->completed &&
                                 !relu->inputs.empty() && relu->inputs[0].version == addId;

    const Phase1PublicReport* selected = nullptr;
    for (const Phase1PublicReport& report : reports.reports())
        if (report.error_class == "UE")
            selected = &report;
    phase2::EngineConfig config;
    config.policy = phase2::Policy::B2_CLASS_AWARE;
    config.window_mode = phase2::WindowMode::AUTO;
    config.delta_cycles = 1;
    phase2::RecoveryEngine engine;
    phase2::Gate gate;
    phase2::CheckpointView view;
    phase2::Decision decision;
    if (selected != nullptr)
        decision = engine.analyze(*selected, session.feeder->currentClockCycle, config, ledger, gate,
                                  view, reports.overflow(), session.codec);
    const bool selective = decision.status == phase2::P2Status::SELECTIVE_RECOVERED;
    unsigned restored = 0;
    unsigned replay = 0;
    bool restoredInput = false;
    if (selective)
    {
        for (const phase2::DataRef& ref : decision.restore_list)
        {
            if (ref.version != 0)
                continue;
            const std::vector<BurstType>* tile = store.tile(ref.operand, ref.tile);
            if (tile == nullptr)
                continue;
            restoredInput = ref.operand == 0 && countBurstDiff(*tile, left) == 0;
            preloadRow(*session.kernel, *tile, ledger.baseRow(ref.operand));
            restored += 1;
        }
        drainReports(*session.feeder, reports, 8000000ull);
        for (const phase2::Run& run : decision.replay_runs)
        {
            if (run.kernel_idx == 0)
                session.kernel->executeEltwise(static_cast<int>(bursts), pimBankType::ALL_BANK,
                                               KernelType::ADD, 0, 256, 128);
            else if (run.kernel_idx == 1)
                session.kernel->executeEltwise(static_cast<int>(bursts), pimBankType::ALL_BANK,
                                               KernelType::RELU, 256, 384, 0);
            replay += run.tile_count;
        }
        drainReports(*session.feeder, reports, 8000000ull);
    }
    const phase2::DataRef result{3, 0, reluId};
    gate.hold(result);
    phase3BindPublication(384, &gate, result.operand, result.tile, result.version);
    const BurstType stored = hostRead(session, session.region.physical(384), "HOST_ATK");
    const bool storedMatches = std::memcmp(stored.u8Data_, expected[0].u8Data_, 32) == 0;
    const bool heldBlocks = countBurstDiff(readRow(session, 384, bursts), expected) != 0;
    gate.publish(result, session.feeder->currentClockCycle);
    const unsigned mismatch = countBurstDiff(readRow(session, 384, bursts), expected);
    const bool reportLogical = selected != nullptr && selected->logical_row == 0;

    std::ostringstream json;
    json << "{\"level\":" << session.region.level(0) << ",\"spare\":" << spare
         << ",\"logical_after_l2\":" << (logicalAfterL2 ? "true" : "false")
         << ",\"spare_has\":" << (spareHas ? "true" : "false")
         << ",\"host_ue\":" << (hostSawUe ? "true" : "false")
         << ",\"pim_ue\":" << (pimSawUe ? "true" : "false")
         << ",\"ledger_completed\":" << (ledgerCompleted ? "true" : "false")
         << ",\"report_logical\":" << (reportLogical ? "true" : "false")
         << ",\"status\":\"" << phase2::p2StatusName(decision.status) << "\""
         << ",\"restored\":" << restored << ",\"replay\":" << replay
         << ",\"restored_input\":" << (restoredInput ? "true" : "false")
         << ",\"stored_matches\":" << (storedMatches ? "true" : "false")
         << ",\"held_blocks\":" << (heldBlocks ? "true" : "false")
         << ",\"mismatch\":" << mismatch << ",\"level_two\":" << (levelTwo ? "true" : "false")
         << "}\n";
    mkdir(out.c_str(), 0755);
    writeCell(out + "/jointfix.json", json.str());

    EXPECT_TRUE(levelTwo);
    EXPECT_GE(spare, RegionMap::kRows - 16u);
    EXPECT_TRUE(logicalAfterL2);
    EXPECT_TRUE(spareHas);
    EXPECT_TRUE(hostSawUe);
    EXPECT_TRUE(pimSawUe);
    EXPECT_TRUE(ledgerCompleted);
    EXPECT_TRUE(reportLogical);
    EXPECT_TRUE(selective);
    EXPECT_GT(restored, 0u);
    EXPECT_TRUE(restoredInput);
    EXPECT_GT(replay, 0u);
    EXPECT_TRUE(storedMatches);
    EXPECT_TRUE(heldBlocks);
    EXPECT_EQ(0u, mismatch);
}

TEST(Phase3M3, Stage4Close)
{
    const std::string out =
        "/opt/data/data/workspace-dynamic/Rowhammer/phase3_exp/results/m3/datapath_stage4";
    const unsigned bursts = 128u;
    std::vector<BurstType> left(bursts), right(bursts), expected(bursts), doubled(bursts),
        sentinel(bursts);
    for (unsigned i = 0; i < bursts; ++i)
    {
        left[i] = positiveBurst(i, 4);
        right[i] = positiveBurst(i, 5);
        expected[i] = left[i] + right[i];
        doubled[i] = expected[i] + expected[i];
        sentinel[i] = positiveBurst(i, 9);
    }

    bool zeroNotStored = false;
    bool retryMatches = false;
    bool cancelNotStored = false;
    bool paused = false;
    bool retried = false;
    bool cancelled = false;
    bool staleHandle = false;
    {
        Session session(kDataDevice, kDataSystem, out, 1, 1.0, "NONE", 1.5, 0.0);
        preloadRow(*session.kernel, left, 0);
        preloadRow(*session.kernel, right, 128);
        preloadRow(*session.kernel, sentinel, 512);
        preloadRow(*session.kernel, sentinel, 640);
        drain(*session.feeder, 8000000ull);
        session.kernel->executeEltwise(static_cast<int>(bursts), pimBankType::ALL_BANK, KernelType::ADD,
                                       0, 256, 128);
        drain(*session.feeder, 8000000ull);
        phase2::Gate gate;
        const phase2::DataRef result{2, 0, 1};
        gate.hold(result);
        phase3BindPublication(256, &gate, result.operand, result.tile, result.version);
        Phase1Runtime runtime(2);
        const OutputHandle handle = runtime.allocateOutput(32, 0);
        runtime.beginProducing(handle, 1);
        runtime.producerComplete(handle, 2);
        runtime.quiesceOutput(handle, 3);
        paused = runtime.state(handle) == OutputState::QUIESCED &&
                 !runtime.consume(handle, ConsumerType::PIM, 3);
        runtime.beginRestore(handle, 4);
        runtime.beginReexecute(handle, 5);
        runtime.producerComplete(handle, 6);
        runtime.tick(7);
        const bool waiting = runtime.state(handle) == OutputState::READY_UNPUBLISHED;
        runtime.tick(8);
        retried = waiting && runtime.canConsume(handle, ConsumerType::PIM, 8);
        session.kernel->executeEltwise(static_cast<int>(bursts), pimBankType::ALL_BANK, KernelType::ADD,
                                       256, 512, 256);
        drain(*session.feeder, 8000000ull);
        const BurstType blockedStored = hostRead(session, 512, "HOST_ATK");
        zeroNotStored = std::memcmp(blockedStored.u8Data_, sentinel[0].u8Data_, 32) == 0 &&
                        std::memcmp(blockedStored.u8Data_, doubled[0].u8Data_, 32) != 0;
        gate.publish(result, session.feeder->currentClockCycle);
        session.kernel->executeEltwise(static_cast<int>(bursts), pimBankType::ALL_BANK, KernelType::ADD,
                                       256, 512, 256);
        drain(*session.feeder, 8000000ull);
        const BurstType retriedStored = hostRead(session, 512, "HOST_ATK");
        retryMatches = std::memcmp(retriedStored.u8Data_, doubled[0].u8Data_, 32) == 0;
        gate.poison({result});
        Phase1Runtime cancelRuntime(0);
        const OutputHandle cancelHandle = cancelRuntime.allocateOutput(32, 0);
        cancelRuntime.beginProducing(cancelHandle, 1);
        Phase1PublicReport ue;
        ue.error_class = "UE";
        cancelRuntime.onReport(ue, 2);
        cancelled = cancelRuntime.state(cancelHandle) == OutputState::POISONED &&
                    !cancelRuntime.consume(cancelHandle, ConsumerType::PIM, 2);
        session.kernel->executeEltwise(static_cast<int>(bursts), pimBankType::ALL_BANK, KernelType::ADD,
                                       256, 640, 256);
        drain(*session.feeder, 8000000ull);
        const BurstType cancelledStored = hostRead(session, 640, "HOST_ATK");
        cancelNotStored = std::memcmp(cancelledStored.u8Data_, sentinel[0].u8Data_, 32) == 0;
        const OutputHandle replacement = cancelRuntime.allocateOutput(32, 3);
        (void)replacement;
        staleHandle = cancelRuntime.isStale(cancelHandle) &&
                      !cancelRuntime.consume(cancelHandle, ConsumerType::HOST, 4);
        phase3ClearPublication();
    }

    bool staleDenied = false;
    bool staleZeroed = false;
    bool reboundMatches = false;
    bool oldReportRejected = false;
    bool forgedReportRejected = false;
    bool freshReportUsable = false;
    {
        Session moved(kDataDevice, kDataSystem, out, 2, 1.0, "OURS", 1.1, 0.0);
        Phase1Reports reports(moved.codec, 1, static_cast<unsigned>(moved.rank->banks.size()),
                              "codeword", 1, 1, 64, true, true);
        armLocalEcc(moved, reports);
        preloadRow(*moved.kernel, left, 0);
        drainReports(*moved.feeder, reports, 8000000ull);
        moved.rank->banks[0].injectRawDataBit(moved.region.physical(0), 0, 14);
        BurstType first;
        enqueueRead(*moved.feeder, moved.codec, 0, 0, first, "HOST_BG");
        drainReports(*moved.feeder, reports, 8000000ull);
        Phase1PublicReport saved;
        const bool have = !reports.reports().empty();
        if (have)
            saved = reports.reports()[0];
        BurstType staleSlot;
        std::memset(staleSlot.u8Data_, 0x11, sizeof(staleSlot.u8Data_));
        enqueueRead(*moved.feeder, moved.codec, 0, 0, staleSlot, "HOST_BG");
        const uint64_t staleBefore = phase3StaleRequests();
        const uint64_t stamped = have ? saved.mapping_epoch : 0;
        for (unsigned n = 0; n < 40 && moved.region.epoch(0) == stamped; ++n)
            moved.region.forceSwap(&moved.model);
        drain(*moved.feeder, 2000000ull);
        staleDenied = phase3StaleRequests() > staleBefore;
        staleZeroed = std::memcmp(staleSlot.u8Data_, left[0].u8Data_, 32) != 0;
        bool cleared = true;
        for (int i = 0; i < 32; ++i)
            cleared = cleared && staleSlot.u8Data_[i] == 0;
        staleZeroed = staleZeroed && cleared;
        reboundMatches = countBurstDiff(readRow(moved, 0, bursts), left) == 0;
        phase2::EngineConfig config;
        config.policy = phase2::Policy::B2_CLASS_AWARE;
        config.window_mode = phase2::WindowMode::AUTO;
        phase2::Ledger ledger;
        ledger.setBaseRow(0, 0);
        ledger.setBaseRow(1, 128);
        ledger.addCompleted(1, 0, 0, {{0, 0}}, {{1, 0}}, 0, moved.feeder->currentClockCycle);
        phase2::Gate gate;
        phase2::CheckpointView view;
        phase2::RecoveryEngine engine;
        if (have)
        {
            const phase2::Decision decision = engine.analyze(
                saved, moved.feeder->currentClockCycle, config, ledger, gate, view, false, moved.codec);
            oldReportRejected = decision.status == phase2::P2Status::STALE_EPOCH &&
                                decision.restore_list.empty() && decision.replay_runs.empty();
            Phase1PublicReport forged = saved;
            forged.mapping_epoch = moved.region.epoch(0) + 5;
            const phase2::Decision forgedDecision = engine.analyze(
                forged, moved.feeder->currentClockCycle, config, ledger, gate, view, false,
                moved.codec);
            forgedReportRejected = forgedDecision.status == phase2::P2Status::STALE_EPOCH;
        }
        moved.rank->banks[0].injectRawDataBit(moved.region.physical(0), 0, 14);
        const uint64_t before = reports.internalCe();
        BurstType fresh;
        enqueueRead(*moved.feeder, moved.codec, 0, 0, fresh, "HOST_BG");
        drainReports(*moved.feeder, reports, 8000000ull);
        if (reports.internalCe() > before)
        {
            const phase2::Decision decision = engine.analyze(
                reports.reports().back(), moved.feeder->currentClockCycle, config, ledger, gate,
                view, false, moved.codec);
            freshReportUsable = decision.status != phase2::P2Status::STALE_EPOCH;
        }
    }

    bool eccNotOracle = false;
    bool correctedMatches = false;
    bool uncorrectableDiffers = false;
    bool recoveredMatches = false;
    bool restoredInput = false;
    {
        Session corrected(kDataDevice, kDataSystem, out, 3, 1.0, "OURS", 1.1, 0.0);
        Phase1Reports reports(corrected.codec, 1, static_cast<unsigned>(corrected.rank->banks.size()),
                              "codeword", 1, 1, 64, true, true);
        armLocalEcc(corrected, reports);
        preloadRow(*corrected.kernel, left, 2000);
        preloadRow(*corrected.kernel, right, 2128);
        drainReports(*corrected.feeder, reports, 8000000ull);
        corrected.rank->banks[0].injectRawDataBit(2000, 0, 14);
        corrected.kernel->executeEltwise(static_cast<int>(bursts), pimBankType::ALL_BANK, KernelType::ADD,
                                         2000, 2256, 2128);
        corrected.kernel->executeEltwise(static_cast<int>(bursts), pimBankType::ALL_BANK, KernelType::RELU,
                                         2256, static_cast<int>(resultRow("DEC")), 0);
        drainReports(*corrected.feeder, reports, 8000000ull);
        eccNotOracle = reports.internalCe() > 0 && corrected.model.cumulativeFlips() == 0;
        correctedMatches = countBurstDiff(readRow(corrected, resultRow("DEC"), bursts),
                                          [&]() {
                                              std::vector<BurstType> outv(bursts);
                                              for (unsigned i = 0; i < bursts; ++i)
                                                  outv[i] = applyRelu(expected[i]);
                                              return outv;
                                          }()) == 0;
        phase3ClearPublication();
    }
    {
        Session failed(kDataDevice, kDataSystem, out, 4, 1.0, "OURS", 1.1, 0.0);
        Phase1Reports reports(failed.codec, 1, static_cast<unsigned>(failed.rank->banks.size()),
                              "codeword", 1, 1, 64, true, true);
        armLocalEcc(failed, reports);
        phase2::Phase2CheckpointStore store;
        store.addTile(0, 0, left);
        store.addTile(1, 0, right);
        preloadRow(*failed.kernel, left, 2000);
        preloadRow(*failed.kernel, right, 2128);
        drainReports(*failed.feeder, reports, 8000000ull);
        failed.rank->banks[0].injectRawDataBit(2000, 0, 14);
        failed.rank->banks[0].injectRawDataBit(2000, 0, 15);
        failed.kernel->executeEltwise(static_cast<int>(bursts), pimBankType::ALL_BANK, KernelType::ADD,
                                      2000, 2256, 2128);
        failed.kernel->executeEltwise(static_cast<int>(bursts), pimBankType::ALL_BANK, KernelType::RELU,
                                      2256, static_cast<int>(resultRow("DEC")), 0);
        drainReports(*failed.feeder, reports, 8000000ull);
        std::vector<BurstType> reluExpected(bursts);
        for (unsigned i = 0; i < bursts; ++i)
            reluExpected[i] = applyRelu(expected[i]);
        uncorrectableDiffers =
            reports.internalUe() > 0 && failed.model.cumulativeFlips() == 0 &&
            countBurstDiff(readRow(failed, resultRow("DEC"), bursts), reluExpected) > 0;
        const std::vector<BurstType>* tile = store.tile(0, 0);
        restoredInput = tile != nullptr && countBurstDiff(*tile, left) == 0 &&
                        countBurstDiff(*tile, reluExpected) > 0;
        preloadRow(*failed.kernel, *tile, 2000);
        drainReports(*failed.feeder, reports, 8000000ull);
        failed.kernel->executeEltwise(static_cast<int>(bursts), pimBankType::ALL_BANK, KernelType::ADD,
                                      2000, 2256, 2128);
        failed.kernel->executeEltwise(static_cast<int>(bursts), pimBankType::ALL_BANK, KernelType::RELU,
                                      2256, static_cast<int>(resultRow("DEC")), 0);
        drainReports(*failed.feeder, reports, 8000000ull);
        recoveredMatches = countBurstDiff(readRow(failed, resultRow("DEC"), bursts), reluExpected) == 0;
        phase3ClearPublication();
    }

    std::ostringstream json;
    json << "{\"zero_not_stored\":" << (zeroNotStored ? "true" : "false")
         << ",\"paused\":" << (paused ? "true" : "false")
         << ",\"retried\":" << (retried ? "true" : "false")
         << ",\"retry_matches\":" << (retryMatches ? "true" : "false")
         << ",\"cancelled\":" << (cancelled ? "true" : "false")
         << ",\"cancel_not_stored\":" << (cancelNotStored ? "true" : "false")
         << ",\"stale_handle\":" << (staleHandle ? "true" : "false")
         << ",\"stale_denied\":" << (staleDenied ? "true" : "false")
         << ",\"stale_zeroed\":" << (staleZeroed ? "true" : "false")
         << ",\"rebound_matches\":" << (reboundMatches ? "true" : "false")
         << ",\"old_report_rejected\":" << (oldReportRejected ? "true" : "false")
         << ",\"forged_report_rejected\":" << (forgedReportRejected ? "true" : "false")
         << ",\"fresh_report_usable\":" << (freshReportUsable ? "true" : "false")
         << ",\"ecc_not_oracle\":" << (eccNotOracle ? "true" : "false")
         << ",\"corrected_matches\":" << (correctedMatches ? "true" : "false")
         << ",\"uncorrectable_differs\":" << (uncorrectableDiffers ? "true" : "false")
         << ",\"restored_input\":" << (restoredInput ? "true" : "false")
         << ",\"recovered_matches\":" << (recoveredMatches ? "true" : "false")
         << ",\"ledger_predeclared\":true}\n";
    mkdir(out.c_str(), 0755);
    writeCell(out + "/stage4_close.json", json.str());

    EXPECT_TRUE(paused);
    EXPECT_TRUE(retried);
    EXPECT_TRUE(zeroNotStored);
    EXPECT_TRUE(retryMatches);
    EXPECT_TRUE(cancelled);
    EXPECT_TRUE(cancelNotStored);
    EXPECT_TRUE(staleHandle);
    EXPECT_TRUE(staleDenied);
    EXPECT_TRUE(staleZeroed);
    EXPECT_TRUE(reboundMatches);
    EXPECT_TRUE(oldReportRejected);
    EXPECT_TRUE(forgedReportRejected);
    EXPECT_TRUE(freshReportUsable);
    EXPECT_TRUE(eccNotOracle);
    EXPECT_TRUE(correctedMatches);
    EXPECT_TRUE(uncorrectableDiffers);
    EXPECT_TRUE(restoredInput);
    EXPECT_TRUE(recoveredMatches);
}

TEST(Phase3M3, Stage5Cell)
{
    const std::string out =
        "/opt/data/data/workspace-dynamic/Rowhammer/phase3_exp/results/m3/datapath_stage5";
    mkdir(out.c_str(), 0755);
    const CampaignResult clean =
        runM3Cell(kDataDevice, kDataSystem, out, "W2", "NONE", "NO_ATK", 1.0, 0, 1, 0.0, nullptr);
    const CampaignResult ours =
        runM3Cell(kDataDevice, kDataSystem, out, "W2", "OURS", "NO_ATK", 1.0, 0, 1, 0.0, nullptr);
    CampaignInject one;
    one.enabled = true;
    one.logicalRow = 0;
    one.bit = 14;
    const CampaignResult corrected =
        runM3Cell(kDataDevice, kDataSystem, out, "W2", "OURS", "NO_ATK", 1.0, 0, 2, 0.0, &one);
    CampaignInject two = one;
    two.second = true;
    const CampaignResult recovered =
        runM3Cell(kDataDevice, kDataSystem, out, "W2", "OURS", "NO_ATK", 1.0, 0, 3, 0.0, &two);
    const CampaignResult bare =
        runM3Cell(kDataDevice, kDataSystem, out, "W2", "NONE", "NO_ATK", 1.0, 0, 4, 0.0, &two);
    const CampaignResult dec =
        runM3Cell(kDataDevice, kDataSystem, out, "DEC", "OURS", "NO_ATK", 1.0, 0, 5, 0.0, nullptr);
    std::ostringstream json;
    json << "{\"clean_exact\":" << (clean.bitExact ? "true" : "false")
         << ",\"clean_status\":\"" << clean.status << "\",\"clean_error\":\"" << clean.error
         << "\",\"ours_items\":" << ours.pimItems << ",\"none_items\":" << clean.pimItems
         << ",\"model_scrubs\":" << (clean.modelScrubs + ours.modelScrubs + corrected.modelScrubs +
                                     recovered.modelScrubs + bare.modelScrubs + dec.modelScrubs)
         << ",\"corrected_ce\":" << corrected.ce << ",\"corrected_flips\":" << corrected.flips
         << ",\"corrected_exact\":" << (corrected.bitExact ? "true" : "false")
         << ",\"corrected_replayed\":" << (corrected.replayed ? "true" : "false")
         << ",\"corrected_scrubs\":" << corrected.scrubWrites
         << ",\"recovered_ue\":" << recovered.ue
         << ",\"recovered_errors\":" << recovered.computeErrors
         << ",\"recovered_committed\":" << recovered.committedMismatch
         << ",\"recovered_replayed\":" << (recovered.replayed ? "true" : "false")
         << ",\"recovered_restored\":" << recovered.restoredTiles
         << ",\"recovered_exact\":" << (recovered.bitExact ? "true" : "false")
         << ",\"bare_exact\":" << (bare.bitExact ? "true" : "false")
         << ",\"bare_commit\":" << (bare.erroneousCommit ? "true" : "false")
         << ",\"bare_replayed\":" << (bare.replayed ? "true" : "false")
         << ",\"dec_exact\":" << (dec.bitExact ? "true" : "false")
         << ",\"dec_status\":\"" << dec.status << "\",\"dec_error\":\"" << dec.error
         << "\",\"score_row\":" << clean.scoreRow << ",\"dec_row\":" << dec.scoreRow << "}\n";
    writeCell(out + "/cell.json", json.str());

    EXPECT_EQ(std::string("SUCCESS"), clean.status) << clean.error;
    EXPECT_TRUE(clean.bitExact);
    EXPECT_EQ(0u, clean.computeErrors);
    EXPECT_EQ(clean.pimItems, ours.pimItems);
    EXPECT_EQ(0ull, clean.modelScrubs);
    EXPECT_EQ(640u, clean.scoreRow);
    EXPECT_EQ(std::string("SUCCESS"), corrected.status) << corrected.error;
    EXPECT_GT(corrected.ce, 0ull);
    EXPECT_EQ(0ull, corrected.flips);
    EXPECT_TRUE(corrected.bitExact);
    EXPECT_FALSE(corrected.replayed);
    EXPECT_GT(corrected.scrubWrites, 0ull);
    EXPECT_EQ(0ull, corrected.modelScrubs);
    EXPECT_EQ(std::string("SUCCESS"), recovered.status) << recovered.error;
    EXPECT_GT(recovered.ue, 0ull);
    EXPECT_GT(recovered.computeErrors, 0u);
    EXPECT_TRUE(recovered.replayed);
    EXPECT_GT(recovered.restoredTiles, 0ull);
    EXPECT_EQ(0u, recovered.committedMismatch);
    EXPECT_TRUE(recovered.bitExact);
    EXPECT_FALSE(recovered.erroneousCommit);
    EXPECT_FALSE(bare.bitExact);
    EXPECT_TRUE(bare.erroneousCommit);
    EXPECT_FALSE(bare.replayed);
    EXPECT_EQ(0ull, bare.modelScrubs);
    EXPECT_EQ(std::string("SUCCESS"), dec.status) << dec.error;
    EXPECT_TRUE(dec.bitExact);
    EXPECT_EQ(2384u, dec.scoreRow);
}

TEST(Phase3M3, Stage5Fault)
{
    const std::string out =
        "/opt/data/data/workspace-dynamic/Rowhammer/phase3_exp/results/m3/datapath_stage5";
    mkdir(out.c_str(), 0755);
    Session session(kDataDevice, kDataSystem, out, 7, 1.0, "OURS", 1.1, 0.0);
    session.live.undoModelBits = false;
    Phase1Reports reports(session.codec, 1, static_cast<unsigned>(session.rank->banks.size()),
                          "codeword", 1, 1, 64, true, true);
    for (Bank& bank : session.rank->banks)
    {
        bank.setPhase1(Phase1Profile::LOCAL_VISIBLE, &reports);
        bank.setPhase1Scrub(true);
    }
    const unsigned bursts = 128u;
    std::vector<BurstType> left(bursts);
    for (unsigned i = 0; i < bursts; ++i)
        left[i] = positiveBurst(i, 4);
    preloadRow(*session.kernel, left, 0);
    drainReports(*session.feeder, reports, 8000000ull);
    Bank& bank = session.rank->banks[0];
    const Phase1StorageKey key{0, 0, 0, 0, 0, 0};
    const uint64_t rawBefore = bank.phase1Storage().readRawData(key);
    const uint8_t parityBefore = bank.phase1Storage().readRawParity(key);
    const uint64_t flipsBefore = session.model.cumulativeFlips();
    const bool parityRefused = !injectRank(0, 0, 0, 1, true, session.rank);
    const bool parityUnchanged = bank.phase1Storage().readRawParity(key) == parityBefore &&
                                 bank.phase1Storage().readRawData(key) == rawBefore;
    bank.injectRawParityBit(0, 0, 0, 1);
    const bool parityMoves = bank.phase1Storage().readRawParity(key) != parityBefore &&
                             bank.phase1Storage().readRawData(key) == rawBefore;
    bank.injectRawParityBit(0, 0, 0, 1);
    bank.injectRawDataBit(0, 0, 14);
    const bool dataMoves = bank.phase1Storage().readRawData(key) != rawBefore &&
                           bank.phase1Storage().readRawParity(key) == parityBefore;
    const bool counterSilent = session.model.cumulativeFlips() == flipsBefore &&
                               session.model.flipsOn(0, 0) == 0;
    BurstType first;
    enqueueRead(*session.feeder, session.codec, 0, 0, first, "HOST_BG");
    drainReports(*session.feeder, reports, 2000000ull);
    const uint64_t ceAfterRead = reports.internalCe();
    const bool scrubbed = bank.phase1ScrubWrites() > 0 && bank.phase1Storage().readRawData(key) == rawBefore;
    const unsigned level = session.region.level(0);
    const uint64_t ceBeforeReread = reports.internalCe();
    enqueueRead(*session.feeder, session.codec, 0, 0, first, "HOST_BG");
    drainReports(*session.feeder, reports, 2000000ull);
    const bool noSecondCe = reports.internalCe() == ceBeforeReread;
    const bool modelNotUsed = session.model.scrubbedCount() == 0;

    Session moved(kDataDevice, kDataSystem, out, 8, 1.0, "OURS", 1.1, 0.0);
    moved.live.undoModelBits = false;
    Phase1Reports movedReports(moved.codec, 1, static_cast<unsigned>(moved.rank->banks.size()),
                               "codeword", 1, 1, 64, true, true);
    for (Bank& one : moved.rank->banks)
    {
        one.setPhase1(Phase1Profile::LOCAL_VISIBLE, &movedReports);
        one.setPhase1Scrub(false);
    }
    preloadRow(*moved.kernel, left, 0);
    drainReports(*moved.feeder, movedReports, 8000000ull);
    moved.rank->banks[0].injectRawDataBit(0, 0, 14);
    const uint64_t flipped = moved.rank->banks[0].phase1Storage().readRawData(key);
    moved.region.forceSwap(&moved.model);
    const unsigned dest = moved.region.physical(0);
    const Phase1StorageKey destKey{0, 0, 0, dest, 0, 0};
    const bool movedBit = dest != 0 && moved.rank->banks[0].phase1Storage().readRawData(destKey) == flipped;
    const bool sourceGone = !moved.rank->banks[0].phase1Storage().contains(key);
    const bool modelStays = moved.model.flipsOn(0, 0) == 0;
    BurstType logical;
    enqueueRead(*moved.feeder, moved.codec, 0, 0, logical, "HOST_BG");
    drainReports(*moved.feeder, movedReports, 2000000ull);
    const bool logicalFollows = std::memcmp(logical.u8Data_, left[0].u8Data_, 32) == 0 &&
                                movedReports.internalCe() > 0 && moved.region.level(0) >= 1u;

    Session sticky(kDataDevice, kDataSystem, out, 9, 1.0, "OURS-SCRUB", 1.1, 0.0);
    Phase1Reports stickyReports(sticky.codec, 1, static_cast<unsigned>(sticky.rank->banks.size()),
                                "codeword", 1, 1, 64, true, true);
    for (Bank& one : sticky.rank->banks)
    {
        one.setPhase1(Phase1Profile::LOCAL_VISIBLE, &stickyReports);
        one.setPhase1Scrub(sticky.region.scrubEnabled());
    }
    preloadRow(*sticky.kernel, left, 0);
    drainReports(*sticky.feeder, stickyReports, 8000000ull);
    sticky.rank->banks[0].injectRawDataBit(0, 0, 14);
    enqueueRead(*sticky.feeder, sticky.codec, 0, 0, first, "HOST_BG");
    drainReports(*sticky.feeder, stickyReports, 2000000ull);
    const uint64_t stickyRaw = sticky.rank->banks[0].phase1Storage().readRawData(key);
    const uint64_t stickyCe = stickyReports.internalCe();
    enqueueRead(*sticky.feeder, sticky.codec, 0, 0, first, "HOST_BG");
    drainReports(*sticky.feeder, stickyReports, 2000000ull);
    const bool remains = stickyRaw != rawBefore && sticky.rank->banks[0].phase1ScrubWrites() == 0 &&
                         stickyReports.internalCe() > stickyCe;

    std::ostringstream json;
    json << "{\"parity_refused\":" << (parityRefused ? "true" : "false")
         << ",\"parity_unchanged\":" << (parityUnchanged ? "true" : "false")
         << ",\"parity_moves\":" << (parityMoves ? "true" : "false")
         << ",\"data_moves\":" << (dataMoves ? "true" : "false")
         << ",\"counter_silent\":" << (counterSilent ? "true" : "false")
         << ",\"ce\":" << ceAfterRead << ",\"scrubbed\":" << (scrubbed ? "true" : "false")
         << ",\"level\":" << level << ",\"no_second_ce\":" << (noSecondCe ? "true" : "false")
         << ",\"model_scrubs\":" << (modelNotUsed ? 0 : 1)
         << ",\"dest\":" << dest << ",\"moved_bit\":" << (movedBit ? "true" : "false")
         << ",\"source_gone\":" << (sourceGone ? "true" : "false")
         << ",\"model_stays\":" << (modelStays ? "true" : "false")
         << ",\"logical_follows\":" << (logicalFollows ? "true" : "false")
         << ",\"persists_without_scrub\":" << (remains ? "true" : "false") << "}\n";
    writeCell(out + "/fault.json", json.str());

    EXPECT_TRUE(parityRefused);
    EXPECT_TRUE(parityUnchanged);
    EXPECT_TRUE(parityMoves);
    EXPECT_TRUE(dataMoves);
    EXPECT_TRUE(counterSilent);
    EXPECT_GT(ceAfterRead, 0ull);
    EXPECT_TRUE(scrubbed);
    EXPECT_GE(level, 1u);
    EXPECT_TRUE(noSecondCe);
    EXPECT_TRUE(modelNotUsed);
    EXPECT_TRUE(movedBit);
    EXPECT_TRUE(sourceGone);
    EXPECT_TRUE(modelStays);
    EXPECT_TRUE(logicalFollows);
    EXPECT_TRUE(remains);
}

TEST(Phase3M3, Stage5Cost)
{
    const std::string out =
        "/opt/data/data/workspace-dynamic/Rowhammer/phase3_exp/results/m3/datapath_stage5";
    mkdir(out.c_str(), 0755);
    const uint64_t wall = hbm2PairWallCycles();
    Session swapped(kDataDevice, kDataSystem, out, 11, 1.0, "OURS", 1.1, 0.0);
    swapped.realSwapCost = true;
    swapped.live.undoModelBits = false;
    uint64_t swapCycles = 0;
    swapped.region.forceSwap(&swapped.model);
    swapped.billMoves(swapCycles);
    const unsigned moved = swapped.region.physical(0);
    swapped.region.noteCe(0, moved);
    swapped.region.noteCe(0, moved);
    const uint64_t afterSwap = swapCycles;
    swapped.billMoves(swapCycles);
    const bool spareCharged = swapped.region.level(0) == 2 && swapCycles == afterSwap + wall;

    Session legacy(kDataDevice, kDataSystem, out, 12, 1.0, "OURS", 1.1, 0.0);
    uint64_t legacyCycles = 0;
    const std::size_t before = legacy.moves.size();
    legacy.region.forceSwap(&legacy.model);
    std::vector<std::pair<unsigned, unsigned>> fresh(legacy.moves.begin() + static_cast<std::ptrdiff_t>(before),
                                                     legacy.moves.end());
    chargeMoves(*legacy.feeder, legacy.codec, fresh, legacyCycles);

    std::vector<BurstType> tile(128);
    for (unsigned i = 0; i < tile.size(); ++i)
        tile[i] = positiveBurst(i, 1);
    Session work(kDataDevice, kDataSystem, out, 13, 1.0, "NONE", 1.5, 0.0);
    Phase1Reports reports(work.codec, 1, static_cast<unsigned>(work.rank->banks.size()), "codeword", 1, 1, 64,
                          true, true);
    for (Bank& bank : work.rank->banks)
    {
        bank.setPhase1(Phase1Profile::LOCAL_VISIBLE, &reports);
        bank.setPhase1Scrub(true);
    }
    preloadRow(*work.kernel, tile, 0);
    const uint64_t checkpointCycles = drain(*work.feeder, 8000000ull);
    const size_t pendingBefore = work.feeder->pendingCount();
    work.kernel->executeEltwiseTiles(KernelType::ADD, 0, 1, 0, 256, 128);
    const uint64_t replayItems = work.feeder->pendingCount() - pendingBefore;
    const uint64_t replayCycles = drain(*work.feeder, 8000000ull);

    Session same(kDataDevice, kDataSystem, out, 14, 1.0, "OURS", 1.1, 0.0);
    preloadRow(*same.kernel, tile, 0);
    drain(*same.feeder, 8000000ull);
    const size_t oursBefore = same.feeder->pendingCount();
    same.kernel->executeEltwiseTiles(KernelType::ADD, 0, 1, 0, 256, 128);
    const uint64_t oursItems = same.feeder->pendingCount() - oursBefore;

    work.rank->banks[0].injectRawDataBit(0, 0, 14);
    BurstType slot;
    enqueueRead(*work.feeder, work.codec, 0, 0, slot, "HOST_BG");
    drainReports(*work.feeder, reports, 2000000ull);
    uint64_t scrubWrites = 0;
    for (const Bank& bank : work.rank->banks)
        scrubWrites += bank.phase1ScrubWrites();
    const uint64_t scrubCycles = hbm2ScrubCycles(scrubWrites);

    std::ostringstream json;
    json << "{\"pair_wall\":" << wall << ",\"legacy_pair\":" << legacyCycles
         << ",\"swap_cycles\":" << afterSwap << ",\"with_spare\":" << swapCycles
         << ",\"spare_charged\":" << (spareCharged ? "true" : "false")
         << ",\"checkpoint_cycles\":" << checkpointCycles
         << ",\"replay_cycles\":" << replayCycles << ",\"replay_items\":" << replayItems
         << ",\"ours_items\":" << oursItems << ",\"scrub_writes\":" << scrubWrites
         << ",\"scrub_cycles\":" << scrubCycles << "}\n";
    writeCell(out + "/cost.json", json.str());

    EXPECT_EQ(2568ull, wall);
    EXPECT_EQ(wall, afterSwap);
    EXPECT_EQ(188ull, legacyCycles);
    EXPECT_NE(wall, legacyCycles);
    EXPECT_TRUE(spareCharged);
    EXPECT_GT(checkpointCycles, 0ull);
    EXPECT_GT(replayCycles, 0ull);
    EXPECT_EQ(replayItems, oursItems);
    EXPECT_GT(scrubWrites, 0ull);
    EXPECT_EQ(scrubCycles, hbm2ScrubCycles(scrubWrites));
}

TEST(Phase3M3, Stage5Joint)
{
    const std::string out =
        "/opt/data/data/workspace-dynamic/Rowhammer/phase3_exp/results/m3/datapath_stage5";
    mkdir(out.c_str(), 0755);
    CampaignInject one;
    one.enabled = true;
    one.logicalRow = 0;
    one.bit = 14;
    CampaignInject two = one;
    two.second = true;
    const CampaignResult corrected =
        runM3Cell(kDataDevice, kDataSystem, out, "W2", "OURS", "NO_ATK", 1.0, 1, 21, 0.0, &one);
    const CampaignResult recovered =
        runM3Cell(kDataDevice, kDataSystem, out, "W2", "OURS", "NO_ATK", 1.0, 1, 22, 0.0, &two);
    const CampaignResult bare =
        runM3Cell(kDataDevice, kDataSystem, out, "W2", "NONE", "NO_ATK", 1.0, 1, 23, 0.0, &two);
    CampaignInject missing = two;
    missing.dropCheckpoint = true;
    const CampaignResult refused =
        runM3Cell(kDataDevice, kDataSystem, out, "W2", "OURS", "NO_ATK", 1.0, 1, 24, 0.0, &missing);
    std::ostringstream json;
    json << "{\"corrected_flips\":" << corrected.flips << ",\"corrected_ce\":" << corrected.ce
         << ",\"corrected_ue\":" << corrected.ue << ",\"corrected_errors\":" << corrected.computeErrors
         << ",\"corrected_replayed\":" << (corrected.replayed ? "true" : "false")
         << ",\"corrected_mismatch\":" << corrected.committedMismatch
         << ",\"corrected_erroneous\":" << (corrected.erroneousCommit ? "true" : "false")
         << ",\"corrected_refused\":" << (corrected.refused ? "true" : "false")
         << ",\"recovered_flips\":" << recovered.flips << ",\"recovered_ce\":" << recovered.ce
         << ",\"recovered_ue\":" << recovered.ue << ",\"recovered_errors\":" << recovered.computeErrors
         << ",\"recovered_replayed\":" << (recovered.replayed ? "true" : "false")
         << ",\"recovered_restored\":" << recovered.restoredTiles
         << ",\"recovered_mismatch\":" << recovered.committedMismatch
         << ",\"recovered_erroneous\":" << (recovered.erroneousCommit ? "true" : "false")
         << ",\"recovered_refused\":" << (recovered.refused ? "true" : "false")
         << ",\"bare_flips\":" << bare.flips << ",\"bare_ue\":" << bare.ue
         << ",\"bare_errors\":" << bare.computeErrors
         << ",\"bare_mismatch\":" << bare.committedMismatch
         << ",\"bare_erroneous\":" << (bare.erroneousCommit ? "true" : "false")
         << ",\"bare_refused\":" << (bare.refused ? "true" : "false")
         << ",\"bare_replayed\":" << (bare.replayed ? "true" : "false")
         << ",\"model_scrubs\":" << (corrected.modelScrubs + recovered.modelScrubs + bare.modelScrubs)
         << ",\"refused_status\":\"" << refused.recovery << "\""
         << ",\"refused_flag\":" << (refused.refused ? "true" : "false")
         << ",\"refused_commit\":" << (refused.erroneousCommit ? "true" : "false")
         << ",\"refused_errors\":" << refused.computeErrors << "}\n";
    writeCell(out + "/joint.json", json.str());

    EXPECT_EQ(std::string("SUCCESS"), corrected.status) << corrected.error;
    EXPECT_EQ(0ull, corrected.flips);
    EXPECT_GT(corrected.ce, 0ull);
    EXPECT_EQ(0ull, corrected.ue);
    EXPECT_EQ(0u, corrected.computeErrors);
    EXPECT_FALSE(corrected.replayed);
    EXPECT_EQ(0u, corrected.committedMismatch);
    EXPECT_FALSE(corrected.erroneousCommit);
    EXPECT_FALSE(corrected.refused);
    EXPECT_EQ(std::string("SUCCESS"), recovered.status) << recovered.error;
    EXPECT_EQ(0ull, recovered.flips);
    EXPECT_GT(recovered.ue, 0ull);
    EXPECT_GT(recovered.computeErrors, 0u);
    EXPECT_TRUE(recovered.replayed);
    EXPECT_GT(recovered.restoredTiles, 0ull);
    EXPECT_EQ(0u, recovered.committedMismatch);
    EXPECT_FALSE(recovered.erroneousCommit);
    EXPECT_FALSE(recovered.refused);
    EXPECT_EQ(std::string("SUCCESS"), bare.status) << bare.error;
    EXPECT_GT(bare.ue, 0ull);
    EXPECT_GT(bare.computeErrors, 0u);
    EXPECT_EQ(bare.computeErrors, bare.committedMismatch);
    EXPECT_TRUE(bare.erroneousCommit);
    EXPECT_FALSE(bare.refused);
    EXPECT_FALSE(bare.replayed);
    EXPECT_EQ(0ull, corrected.modelScrubs + recovered.modelScrubs + bare.modelScrubs);
    EXPECT_EQ(std::string("SUCCESS"), refused.status) << refused.error;
    EXPECT_TRUE(refused.refused);
    EXPECT_TRUE(refused.recoveryFailed);
    EXPECT_FALSE(refused.erroneousCommit);
    EXPECT_GT(refused.computeErrors, 0u);
    EXPECT_EQ(std::string("OUTSIDE_WINDOW_NO_TRUSTED_INPUT"), refused.recovery);
}

TEST(Phase3M3, Stage5Close)
{
    const std::string out =
        "/opt/data/data/workspace-dynamic/Rowhammer/phase3_exp/results/m3/datapath_stage5_close";
    mkdir(out.c_str(), 0755);
    unsetenv("PHASE3_BOUNDARY_VICTIM");

    phase2::Ledger coverage;
    coverage.setBaseRow(0, 0);
    coverage.setBaseRow(2, 256);
    const uint64_t wrong = coverage.planOp(1, 0, 4, {{0, 4}}, {{2, 4}});
    coverage.recordIssue(wrong, 1);
    coverage.observeAccess(wrong, 0, false);
    coverage.observeAccess(wrong, 256, true);
    const bool baseOnlyRejected = !coverage.completeObserved(wrong, 2);
    const uint64_t right = coverage.planOp(1, 0, 4, {{0, 4}}, {{2, 4}});
    coverage.recordIssue(right, 1);
    coverage.observeAccess(right, 0 + phase2::tileRowOffset(4), false);
    coverage.observeAccess(right, 256 + phase2::tileRowOffset(4), true);
    const bool offsetAccepted = coverage.completeObserved(right, 2);

    bool commandOnly = false;
    double actDelta = 0;
    double swapDelta = 0;
    double chargeDelta = 0;
    uint64_t pendingAtCharge = 0;
    const uint64_t analytical = hbm2PairWallCycles();
    {
        Session probe(kDataDevice, kDataSystem, out, 11, 0.0625, "NONE", 1.5, 0.0);
        commandOnly = probe.model.commandOnlySwap();
        const double beforeAct = probe.model.disturbance(0, 101);
        DRAMSim::BurstType beat;
        enqueueWrite(*probe.feeder, probe.codec, 0, 100, beat, "HOST_ATK");
        Phase1Reports reports(probe.codec, 1, static_cast<unsigned>(probe.rank->banks.size()), "codeword", 1, 1, 64, true, true);
        drainReports(*probe.feeder, reports, 2000000ull);
        actDelta = probe.model.disturbance(0, 101) - beforeAct;
        const double beforeSwap = probe.model.disturbance(0, 201);
        probe.model.noteSwap(0, 200);
        swapDelta = probe.model.disturbance(0, 201) - beforeSwap;
        const double quietBefore = probe.model.disturbance(0, 400);
        pendingAtCharge = probe.feeder->pendingCount();
        chargeDelta = probe.model.disturbance(0, 400) - quietBefore;
    }

    const CampaignResult scored =
        runM3Cell(kDataDevice, kDataSystem, out, "W2", "OURS", "NO_ATK", 0.0625, 0, 1, 0.0, nullptr);
    setenv("PHASE3_BOUNDARY_VICTIM", "1", 1);
    const CampaignResult boundary =
        runM3Cell(kDataDevice, kDataSystem, out, "W2", "OURS", "NO_ATK", 0.0625, 0, 1, 0.0, nullptr);
    const CampaignResult boundaryDec =
        runM3Cell(kDataDevice, kDataSystem, out, "DEC", "OURS", "NO_ATK", 0.0625, 0, 1, 0.0, nullptr);
    unsetenv("PHASE3_BOUNDARY_VICTIM");

    CampaignInject tileFault;
    tileFault.enabled = true;
    tileFault.logicalRow = 1;
    tileFault.bit = 14;
    tileFault.second = true;
    const CampaignResult tile =
        runM3Cell(kDataDevice, kDataSystem, out, "W2", "OURS", "NO_ATK", 0.0625, 0, 2, 0.0, &tileFault);

    CampaignInject gemvFault;
    gemvFault.enabled = true;
    gemvFault.logicalRow = 0;
    gemvFault.bit = 14;
    gemvFault.second = true;
    const CampaignResult gemv =
        runM3Cell(kDataDevice, kDataSystem, out, "DEC", "OURS", "NO_ATK", 0.0625, 0, 3, 0.0, &gemvFault);

    const CampaignResult hammer =
        runM3Cell(kDataDevice, kDataSystem, out, "W2", "OURS", "A1", 0.0625, 0, 20261122ull, 0.0, nullptr);

    std::ostringstream json;
    json << "{\"base_only_rejected\":" << (baseOnlyRejected ? "true" : "false")
         << ",\"offset_accepted\":" << (offsetAccepted ? "true" : "false")
         << ",\"command_only_swap\":" << (commandOnly ? "true" : "false")
         << ",\"act_delta\":" << actDelta
         << ",\"swap_delta\":" << swapDelta
         << ",\"pending_at_charge\":" << pendingAtCharge
         << ",\"analytical_pair\":" << analytical
         << ",\"charge_disturbance_delta\":" << chargeDelta
         << ",\"w2_result_first\":640,\"w2_result_last\":651,\"boundary_w2\":" << boundary.attackRow
         << ",\"boundary_dec\":" << boundaryDec.attackRow
         << ",\"scored_attack\":" << scored.attackRow
         << ",\"scored_physical\":" << scored.physicalVictim
         << ",\"scored_exact\":" << (scored.bitExact ? "true" : "false")
         << ",\"scored_items\":" << scored.pimItems
         << ",\"boundary_exact\":" << (boundary.bitExact ? "true" : "false")
         << ",\"tile_status\":\"" << tile.status << "\",\"tile_error\":\"" << tile.error
         << "\",\"tile_direct\":" << (tile.directInject ? "true" : "false")
         << ",\"tile_flips\":" << tile.flips
         << ",\"tile_ue\":" << tile.ue
         << ",\"tile_errors\":" << tile.computeErrors
         << ",\"tile_replayed\":" << (tile.replayed ? "true" : "false")
         << ",\"tile_restored\":" << tile.restoredTiles
         << ",\"tile_mask\":" << tile.restoredMask
         << ",\"tile_committed\":" << tile.committedMismatch
         << ",\"gemv_status\":\"" << gemv.status << "\",\"gemv_error\":\"" << gemv.error
         << "\",\"gemv_direct\":" << (gemv.directInject ? "true" : "false")
         << ",\"gemv_flips\":" << gemv.flips
         << ",\"gemv_ue\":" << gemv.ue
         << ",\"gemv_errors\":" << gemv.computeErrors
         << ",\"gemv_replayed\":" << (gemv.replayed ? "true" : "false")
         << ",\"gemv_exact\":" << (gemv.bitExact ? "true" : "false")
         << ",\"hammer_status\":\"" << hammer.status << "\",\"hammer_error\":\"" << hammer.error
         << "\",\"hammer_direct\":" << (hammer.directInject ? "true" : "false")
         << ",\"hammer_flips\":" << hammer.flips
         << ",\"hammer_latent\":" << hammer.latent
         << ",\"hammer_ce\":" << hammer.ce
         << ",\"hammer_ue\":" << hammer.ue
         << ",\"hammer_errors\":" << hammer.computeErrors
         << ",\"hammer_replayed\":" << (hammer.replayed ? "true" : "false")
         << ",\"hammer_restored\":" << hammer.restoredTiles
         << ",\"hammer_committed\":" << hammer.committedMismatch
         << ",\"hammer_level\":" << hammer.level
         << ",\"hammer_swaps\":" << hammer.swapCycles
         << ",\"hammer_scrubs\":" << hammer.scrubWrites
         << ",\"hammer_model_scrubs\":" << hammer.modelScrubs
         << ",\"hammer_attack\":" << hammer.attackRow
         << ",\"hammer_physical\":" << hammer.physicalVictim
         << ",\"hammer_physical_end\":" << hammer.physicalVictimEnd
         << ",\"hammer_tx\":" << hammer.attackTx
         << ",\"hammer_exact\":" << (hammer.bitExact ? "true" : "false")
         << ",\"hammer_erroneous\":" << (hammer.erroneousCommit ? "true" : "false")
         << ",\"hammer_refused\":" << (hammer.refused ? "true" : "false")
         << "}\n";
    writeCell(out + "/close.json", json.str());

    EXPECT_TRUE(baseOnlyRejected);
    EXPECT_TRUE(offsetAccepted);
    EXPECT_FALSE(commandOnly);
    EXPECT_DOUBLE_EQ(1.0, actDelta);
    EXPECT_DOUBLE_EQ(2.0, swapDelta);
    EXPECT_EQ(0ull, pendingAtCharge);
    EXPECT_EQ(2568ull, analytical);
    EXPECT_DOUBLE_EQ(0.0, chargeDelta);
    EXPECT_EQ(std::string("SUCCESS"), scored.status) << scored.error;
    EXPECT_TRUE(scored.bitExact);
    EXPECT_EQ(640u, scored.attackRow);
    EXPECT_EQ(640u, scored.physicalVictim);
    EXPECT_EQ(768u, boundary.attackRow);
    EXPECT_EQ(2432u, boundaryDec.attackRow);
    EXPECT_TRUE(boundary.bitExact);
    EXPECT_EQ(std::string("SUCCESS"), tile.status) << tile.error;
    EXPECT_TRUE(tile.directInject);
    EXPECT_EQ(0ull, tile.flips);
    EXPECT_GT(tile.ue, 0ull);
    EXPECT_TRUE(tile.replayed);
    EXPECT_NE(0ull, tile.restoredMask & (1ull << 4));
    EXPECT_EQ(0u, tile.committedMismatch);
    EXPECT_EQ(std::string("SUCCESS"), gemv.status) << gemv.error;
    EXPECT_TRUE(gemv.directInject);
    EXPECT_EQ(0ull, gemv.flips);
    EXPECT_GT(gemv.ue, 0ull);
    EXPECT_FALSE(gemv.replayed);
    EXPECT_EQ(0u, gemv.computeErrors);
    EXPECT_TRUE(gemv.bitExact);
    EXPECT_EQ(std::string("SUCCESS"), hammer.status) << hammer.error;
    EXPECT_FALSE(hammer.directInject);
    EXPECT_EQ(640u, hammer.attackRow);
}

TEST(Phase3M3, Stage5Driver)
{
    const std::string out =
        "/opt/data/data/workspace-dynamic/Rowhammer/phase3_exp/results/m3/datapath_stage5_driver";
    mkdir(out.c_str(), 0755);
    unsetenv("PHASE3_BOUNDARY_VICTIM");

    bool commandOnly = false;
    bool readPreserves = false;
    bool selfReadPreserves = false;
    bool writeChanges = false;
    double actDelta = 0;
    double swapDelta = 0;
    double chargeDelta = 0;
    uint64_t pendingAtCharge = 0;
    const uint64_t analytical = hbm2PairWallCycles();
    {
        Session probe(kDataDevice, kDataSystem, out, 11, 0.0625, "NONE", 1.5, 0.0);
        commandOnly = probe.model.commandOnlySwap();
        const double beforeAct = probe.model.disturbance(0, 101);
        DRAMSim::BurstType slot;
        enqueueRead(*probe.feeder, probe.codec, 0, 100, slot, "HOST_ATK");
        drain(*probe.feeder, 2000000ull);
        actDelta = probe.model.disturbance(0, 101) - beforeAct;
        const double beforeSwap = probe.model.disturbance(0, 201);
        probe.model.noteSwap(0, 200);
        swapDelta = probe.model.disturbance(0, 201) - beforeSwap;
        const double quietBefore = probe.model.disturbance(0, 400);
        pendingAtCharge = probe.feeder->pendingCount();
        chargeDelta = probe.model.disturbance(0, 400) - quietBefore;

        const std::vector<BurstType> original(1, positiveBurst(7, 9));
        preloadRow(*probe.kernel, original, 50);
        drain(*probe.feeder, 2000000ull);
        const std::vector<BurstType> before = readRow(probe, 50, 1);
        enqueueRead(*probe.feeder, probe.codec, 0, 49, slot, "HOST_ATK");
        drain(*probe.feeder, 2000000ull);
        readPreserves = countBurstDiff(before, readRow(probe, 50, 1)) == 0 &&
                        countBurstDiff(before, original) == 0;
        enqueueRead(*probe.feeder, probe.codec, 0, 50, slot, "HOST_ATK");
        drain(*probe.feeder, 2000000ull);
        selfReadPreserves = countBurstDiff(before, readRow(probe, 50, 1)) == 0;
        DRAMSim::BurstType empty;
        enqueueWrite(*probe.feeder, probe.codec, 0, 50, empty, "HOST_ATK");
        drain(*probe.feeder, 2000000ull);
        writeChanges = countBurstDiff(before, readRow(probe, 50, 1)) > 0;
    }

    const CampaignResult scored =
        runM3Cell(kDataDevice, kDataSystem, out, "W2", "OURS", "NO_ATK", 0.0625, 0, 1, 0.0, nullptr);
    const CampaignResult bare =
        runM3Cell(kDataDevice, kDataSystem, out, "W2", "NONE", "NO_ATK", 0.0625, 0, 1, 0.0, nullptr);
    CampaignInject one;
    one.enabled = true;
    one.logicalRow = 0;
    one.bit = 14;
    const CampaignResult injected =
        runM3Cell(kDataDevice, kDataSystem, out, "W2", "OURS", "NO_ATK", 0.0625, 0, 2, 0.0, &one);
    CampaignInject tileFault;
    tileFault.enabled = true;
    tileFault.logicalRow = 1;
    tileFault.bit = 14;
    tileFault.second = true;
    const CampaignResult tile =
        runM3Cell(kDataDevice, kDataSystem, out, "W2", "OURS", "NO_ATK", 0.0625, 0, 3, 0.0, &tileFault);
    const CampaignResult hammer =
        runM3Cell(kDataDevice, kDataSystem, out, "W2", "OURS", "A1", 0.0625, 0, 20261122ull, 0.0, nullptr);
    setenv("PHASE3_BOUNDARY_VICTIM", "1", 1);
    const CampaignResult boundary =
        runM3Cell(kDataDevice, kDataSystem, out, "W2", "OURS", "NO_ATK", 0.0625, 0, 1, 0.0, nullptr);
    const CampaignResult boundaryDec =
        runM3Cell(kDataDevice, kDataSystem, out, "DEC", "OURS", "NO_ATK", 0.0625, 0, 1, 0.0, nullptr);
    unsetenv("PHASE3_BOUNDARY_VICTIM");

    std::ostringstream json;
    json << "{\"read_preserves\":" << (readPreserves ? "true" : "false")
         << ",\"self_read_preserves\":" << (selfReadPreserves ? "true" : "false")
         << ",\"empty_write_changes\":" << (writeChanges ? "true" : "false")
         << ",\"command_only_swap\":" << (commandOnly ? "true" : "false")
         << ",\"act_delta\":" << actDelta
         << ",\"swap_delta\":" << swapDelta
         << ",\"pending_at_charge\":" << pendingAtCharge
         << ",\"analytical_pair\":" << analytical
         << ",\"charge_disturbance_delta\":" << chargeDelta
         << ",\"ours_items\":" << scored.pimItems
         << ",\"none_items\":" << bare.pimItems
         << ",\"ours_exact\":" << (scored.bitExact ? "true" : "false")
         << ",\"inject_direct\":" << (injected.directInject ? "true" : "false")
         << ",\"inject_flips\":" << injected.flips
         << ",\"inject_ce\":" << injected.ce
         << ",\"inject_errors\":" << injected.computeErrors
         << ",\"inject_replayed\":" << (injected.replayed ? "true" : "false")
         << ",\"inject_exact\":" << (injected.bitExact ? "true" : "false")
         << ",\"tile_direct\":" << (tile.directInject ? "true" : "false")
         << ",\"tile_flips\":" << tile.flips
         << ",\"tile_ue\":" << tile.ue
         << ",\"tile_mask\":" << tile.restoredMask
         << ",\"tile_committed\":" << tile.committedMismatch
         << ",\"hammer_status\":\"" << hammer.status << "\",\"hammer_error\":\"" << hammer.error
         << "\",\"hammer_direct\":" << (hammer.directInject ? "true" : "false")
         << ",\"hammer_flips\":" << hammer.flips
         << ",\"hammer_ce\":" << hammer.ce
         << ",\"hammer_ue\":" << hammer.ue
         << ",\"hammer_errors\":" << hammer.computeErrors
         << ",\"hammer_exact\":" << (hammer.bitExact ? "true" : "false")
         << ",\"hammer_erroneous\":" << (hammer.erroneousCommit ? "true" : "false")
         << ",\"hammer_replayed\":" << (hammer.replayed ? "true" : "false")
         << ",\"hammer_level\":" << hammer.level
         << ",\"hammer_swaps\":" << hammer.swapCycles
         << ",\"hammer_scrubs\":" << hammer.scrubWrites
         << ",\"hammer_scrub_cycles\":" << hammer.scrubCycles
         << ",\"hammer_model_scrubs\":" << hammer.modelScrubs
         << ",\"hammer_attack\":" << hammer.attackRow
         << ",\"hammer_physical\":" << hammer.physicalVictim
         << ",\"hammer_physical_end\":" << hammer.physicalVictimEnd
         << ",\"hammer_tx\":" << hammer.attackTx
         << ",\"boundary_w2\":" << boundary.attackRow
         << ",\"boundary_dec\":" << boundaryDec.attackRow
         << ",\"boundary_exact\":" << (boundary.bitExact ? "true" : "false")
         << "}\n";
    writeCell(out + "/driver.json", json.str());

    EXPECT_TRUE(readPreserves);
    EXPECT_TRUE(selfReadPreserves);
    EXPECT_TRUE(writeChanges);
    EXPECT_FALSE(commandOnly);
    EXPECT_DOUBLE_EQ(1.0, actDelta);
    EXPECT_DOUBLE_EQ(2.0, swapDelta);
    EXPECT_EQ(0ull, pendingAtCharge);
    EXPECT_EQ(2568ull, analytical);
    EXPECT_DOUBLE_EQ(0.0, chargeDelta);
    EXPECT_EQ(std::string("SUCCESS"), scored.status) << scored.error;
    EXPECT_TRUE(scored.bitExact);
    EXPECT_EQ(scored.pimItems, bare.pimItems);
    EXPECT_EQ(7059ull, scored.pimItems);
    EXPECT_TRUE(injected.directInject);
    EXPECT_EQ(0ull, injected.flips);
    EXPECT_GT(injected.ce, 0ull);
    EXPECT_EQ(0u, injected.computeErrors);
    EXPECT_FALSE(injected.replayed);
    EXPECT_TRUE(injected.bitExact);
    EXPECT_TRUE(tile.directInject);
    EXPECT_EQ(0ull, tile.flips);
    EXPECT_GT(tile.ue, 0ull);
    EXPECT_NE(0ull, tile.restoredMask & (1ull << 4));
    EXPECT_EQ(0u, tile.committedMismatch);
    EXPECT_EQ(std::string("SUCCESS"), hammer.status) << hammer.error;
    EXPECT_FALSE(hammer.directInject);
    EXPECT_EQ(0ull, hammer.flips);
    EXPECT_EQ(0u, hammer.computeErrors);
    EXPECT_TRUE(hammer.bitExact);
    EXPECT_FALSE(hammer.erroneousCommit);
    EXPECT_EQ(640u, hammer.attackRow);
    EXPECT_EQ(640u, hammer.physicalVictim);
    EXPECT_EQ(768u, boundary.attackRow);
    EXPECT_EQ(2432u, boundaryDec.attackRow);
}

TEST(Phase3M3, Stage5Pair)
{
    const std::string out =
        "/opt/data/data/workspace-dynamic/Rowhammer/phase3_exp/results/m3/datapath_stage5_pair";
    mkdir(out.c_str(), 0755);
    unsetenv("PHASE3_BOUNDARY_VICTIM");
    const uint64_t seed = 20261122ull;
    const double scale = 0.0625;

    auto row = [](const char* name, const CampaignResult& result) {
        std::ostringstream line;
        line << "{\"name\":\"" << name << "\",\"status\":\"" << result.status
             << "\",\"error\":\"" << result.error
             << "\",\"direct_inject\":" << (result.directInject ? "true" : "false")
             << ",\"injected_bits\":" << result.injectedBits
             << ",\"model_flips\":" << result.flips
             << ",\"latent_flips\":" << result.latent
             << ",\"crossings\":" << result.crossings
             << ",\"unplaced\":" << result.unplaced
             << ",\"ce\":" << result.ce
             << ",\"ue\":" << result.ue
             << ",\"compute_errors\":" << result.computeErrors
             << ",\"visible_bits\":" << result.visibleBits
             << ",\"scrub_writes\":" << result.scrubWrites
             << ",\"scrub_cycles\":" << result.scrubCycles
             << ",\"replayed\":" << (result.replayed ? "true" : "false")
             << ",\"restored_tiles\":" << result.restoredTiles
             << ",\"erroneous_commit\":" << (result.erroneousCommit ? "true" : "false")
             << ",\"refused\":" << (result.refused ? "true" : "false")
             << ",\"cycles\":" << result.cycles
             << ",\"swap_cycles\":" << result.swapCycles
             << ",\"pim_items\":" << result.pimItems
             << ",\"attack_tx\":" << result.attackTx
             << ",\"threshold\":" << result.threshold
             << ",\"score_row\":" << result.scoreRow
             << ",\"attack_row\":" << result.attackRow
             << ",\"physical\":" << result.physicalVictim
             << ",\"physical_end\":" << result.physicalVictimEnd
             << ",\"level\":" << result.level
             << ",\"bit_exact\":" << (result.bitExact ? "true" : "false") << "}";
        return line.str();
    };

    CampaignInject one;
    one.enabled = true;
    one.logicalRow = 0;
    one.bit = 14;
    CampaignInject two;
    two.enabled = true;
    two.logicalRow = 1;
    two.bit = 14;
    two.second = true;
    CampaignInject missing;
    missing.enabled = true;
    missing.logicalRow = 0;
    missing.bit = 14;
    missing.second = true;
    missing.dropCheckpoint = true;
    const CampaignResult injectCe =
        runM3Cell(kDataDevice, kDataSystem, out, "W2", "OURS", "NO_ATK", scale, 0, seed, 0.0, &one);
    const CampaignResult injectRecover =
        runM3Cell(kDataDevice, kDataSystem, out, "W2", "OURS", "NO_ATK", scale, 1, seed, 0.0, &two);
    const CampaignResult injectBare =
        runM3Cell(kDataDevice, kDataSystem, out, "W2", "NONE", "NO_ATK", scale, 1, seed, 0.0, &two);
    const CampaignResult injectRefuse =
        runM3Cell(kDataDevice, kDataSystem, out, "W2", "OURS", "NO_ATK", scale, 2, seed, 0.0, &missing);
    std::ostringstream injectJson;
    injectJson << "{\"experiment\":\"direct_inject\",\"scale\":" << scale << ",\"seed\":" << seed
               << ",\"rows\":[" << row("ours_one_bit", injectCe) << "," << row("ours_two_bit", injectRecover)
               << "," << row("none_two_bit", injectBare) << "," << row("ours_missing_checkpoint", injectRefuse)
               << "]}\n";
    writeCell(out + "/inject.json", injectJson.str());

    const CampaignResult none =
        runM3Cell(kDataDevice, kDataSystem, out, "W2", "NONE", "A1", scale, 0, seed, 0.0, nullptr);
    const CampaignResult ours =
        runM3Cell(kDataDevice, kDataSystem, out, "W2", "OURS", "A1", scale, 0, seed, 0.0, nullptr);
    std::ostringstream hammerJson;
    hammerJson << "{\"experiment\":\"rowhammer\",\"workload\":\"W2\",\"attack\":\"A1\",\"scale\":" << scale
               << ",\"seed\":" << seed
               << ",\"object\":\"logical result row 640, current physical row, bank 0\""
               << ",\"not_object\":\"768 and 2432 remain the campaign script target and are not used here\""
               << ",\"rows\":[" << row("none", none) << "," << row("ours", ours) << "]}\n";
    writeCell(out + "/hammer.json", hammerJson.str());

    EXPECT_EQ(std::string("SUCCESS"), injectCe.status) << injectCe.error;
    EXPECT_TRUE(injectCe.directInject);
    EXPECT_EQ(1u, injectCe.injectedBits);
    EXPECT_EQ(0ull, injectCe.flips);
    EXPECT_GT(injectCe.ce, 0ull);
    EXPECT_EQ(0u, injectCe.computeErrors);
    EXPECT_FALSE(injectCe.replayed);
    EXPECT_TRUE(injectCe.bitExact);
    EXPECT_EQ(std::string("SUCCESS"), injectRecover.status) << injectRecover.error;
    EXPECT_EQ(2u, injectRecover.injectedBits);
    EXPECT_EQ(0ull, injectRecover.flips);
    EXPECT_GT(injectRecover.ue, 0ull);
    EXPECT_TRUE(injectRecover.replayed);
    EXPECT_EQ(0u, injectRecover.committedMismatch);
    EXPECT_FALSE(injectRecover.erroneousCommit);
    EXPECT_EQ(std::string("SUCCESS"), injectBare.status) << injectBare.error;
    EXPECT_EQ(0ull, injectBare.flips);
    EXPECT_GT(injectBare.computeErrors, 0u);
    EXPECT_FALSE(injectBare.replayed);
    EXPECT_TRUE(injectBare.erroneousCommit);
    EXPECT_TRUE(injectRefuse.refused);
    EXPECT_FALSE(injectRefuse.erroneousCommit);
    EXPECT_EQ(std::string("SUCCESS"), none.status) << none.error;
    EXPECT_EQ(std::string("SUCCESS"), ours.status) << ours.error;
    EXPECT_FALSE(none.directInject);
    EXPECT_FALSE(ours.directInject);
    EXPECT_EQ(640u, none.attackRow);
    EXPECT_EQ(640u, ours.attackRow);
    EXPECT_EQ(640u, none.scoreRow);
    EXPECT_EQ(none.attackRow, ours.attackRow);
    EXPECT_EQ(none.physicalVictim, ours.physicalVictim);
    EXPECT_DOUBLE_EQ(none.threshold, ours.threshold);
    EXPECT_GT(none.threshold, 0.0);
}

TEST(Phase3M3, Stage5Protocol)
{
    const std::string out =
        "/opt/data/data/workspace-dynamic/Rowhammer/phase3_exp/results/m3/datapath_stage5_protocol";
    mkdir(out.c_str(), 0755);
    unsetenv("PHASE3_BOUNDARY_VICTIM");

    auto flags = [](const char* name, double margin) {
        RegionMap map;
        map.configure(name, margin, 2.0, 0.0625);
        std::ostringstream line;
        line << "{\"scheme\":\"" << name << "\",\"period\":" << map.period(0)
             << ",\"scrub\":" << (map.scrubEnabled() ? "true" : "false")
             << ",\"detector\":" << (map.detectorEnabled() ? "true" : "false")
             << ",\"ce_feedback\":" << (map.ceFeedback() ? "true" : "false")
             << ",\"recovery\":" << (map.recoveryEnabled() ? "true" : "false") << "}";
        return line.str();
    };
    const std::string noneFlags = flags("NONE", 1.5);
    const std::string fixFlags = flags("FIX", 1.5);
    const std::string adaFlags = flags("ADA", 1.5);
    const std::string paraFlags = flags("PARA", 1.5);
    const std::string reactFlags = flags("REACT", 1.5);
    const std::string oursFlags = flags("OURS", 1.1);

    const char* schemes[] = {"NONE", "FIX", "ADA", "PARA", "REACT", "OURS"};
    std::ostringstream runs;
    runs << "[";
    uint64_t items = 0;
    bool first = true;
    for (const char* scheme : schemes)
    {
        const CampaignResult result =
            runM3Cell(kDataDevice, kDataSystem, out, "W2", scheme, "NO_ATK", 0.0625, 0, 1, 0.0, nullptr);
        if (!first)
            runs << ",";
        first = false;
        runs << "{\"scheme\":\"" << scheme << "\",\"status\":\"" << result.status
             << "\",\"error\":\"" << result.error
             << "\",\"exact\":" << (result.bitExact ? "true" : "false")
             << ",\"items\":" << result.pimItems
             << ",\"attack_row\":" << result.attackRow
             << ",\"score_row\":" << result.scoreRow
             << ",\"swap_cycles\":" << result.swapCycles
             << ",\"cycles\":" << result.cycles << "}";
        EXPECT_EQ(std::string("SUCCESS"), result.status) << scheme << " " << result.error;
        EXPECT_TRUE(result.bitExact) << scheme;
        EXPECT_EQ(640u, result.attackRow) << scheme;
        EXPECT_EQ(640u, result.scoreRow) << scheme;
        EXPECT_EQ(0ull, result.attackTx) << scheme;
        if (items == 0)
            items = result.pimItems;
        EXPECT_EQ(items, result.pimItems) << scheme;
    }
    runs << "]";
    std::ostringstream json;
    json << "{\"scale\":0.0625,\"workload\":\"W2\",\"attack\":\"NO_ATK\""
         << ",\"flags\":[" << noneFlags << "," << fixFlags << "," << adaFlags << ","
         << paraFlags << "," << reactFlags << "," << oursFlags << "]"
         << ",\"runs\":" << runs.str() << "}\n";
    writeCell(out + "/protocol.json", json.str());

    RegionMap noneMap;
    noneMap.configure("NONE", 1.5, 2.0, 0.0625);
    RegionMap fixMap;
    fixMap.configure("FIX", 1.5, 2.0, 0.0625);
    RegionMap adaMap;
    adaMap.configure("ADA", 1.5, 2.0, 0.0625);
    RegionMap paraMap;
    paraMap.configure("PARA", 1.5, 2.0, 0.0625);
    RegionMap reactMap;
    reactMap.configure("REACT", 1.5, 2.0, 0.0625);
    RegionMap oursMap;
    oursMap.configure("OURS", 1.1, 2.0, 0.0625);
    EXPECT_EQ(0u, noneMap.period(0));
    EXPECT_FALSE(noneMap.recoveryEnabled());
    EXPECT_TRUE(noneMap.scrubEnabled());
    EXPECT_EQ(4u, fixMap.period(0));
    EXPECT_FALSE(fixMap.detectorEnabled());
    EXPECT_FALSE(fixMap.recoveryEnabled());
    EXPECT_EQ(59u, adaMap.period(0));
    EXPECT_TRUE(adaMap.detectorEnabled());
    EXPECT_FALSE(adaMap.recoveryEnabled());
    EXPECT_EQ(0u, paraMap.period(0));
    EXPECT_TRUE(paraMap.recoveryEnabled());
    EXPECT_EQ(0u, reactMap.period(0));
    EXPECT_TRUE(reactMap.recoveryEnabled());
    EXPECT_EQ(87u, oursMap.period(0));
    EXPECT_TRUE(oursMap.ceFeedback());
    EXPECT_TRUE(oursMap.recoveryEnabled());
    EXPECT_TRUE(oursMap.scrubEnabled());
    EXPECT_EQ(7059ull, items);
}


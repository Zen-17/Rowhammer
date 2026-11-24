#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <map>
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
#include "Phase1Storage.h"
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

bool injectRank(unsigned bank, unsigned row, unsigned burstColumn, unsigned bit, bool parity, void* context);

struct LiveSwap
{
    DRAMSim::Rank* rank = nullptr;
    ReadDisturbanceModel* model = nullptr;
    std::vector<std::pair<unsigned, unsigned>>* moves = nullptr;
};

void scrubLive(unsigned bank, unsigned source, unsigned dest, void* context)
{
    LiveSwap* live = static_cast<LiveSwap*>(context);
    if (live->model != nullptr)
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
    DRAMSim::BurstType slot;
    for (const auto& move : moves)
    {
        enqueueRead(feeder, codec, 0, move.first, slot, "SWAP");
        enqueueRead(feeder, codec, 0, move.second, slot, "SWAP");
        enqueueWrite(feeder, codec, 0, move.first, slot, "SWAP");
        enqueueWrite(feeder, codec, 0, move.second, slot, "SWAP");
        if (feeder.pendingCount() > 64)
            cycles += drain(feeder, 8000000ull);
    }
    cycles += drain(feeder, 8000000ull);
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
        live.moves = &moves;
        region.setScrubber(&scrubLive, &live);
        phase3SetAddressHook(&bridgeEpoch, &bridgeLogical, &region);
        kernel = std::make_shared<PIMKernel>(feeder, 1, 1);
    }

    ~Session()
    {
        phase3SetAddressHook(nullptr, nullptr, nullptr);
        phase3DetachAll();
    }

    void flushSwaps(uint64_t& cycles)
    {
        const std::size_t before = moves.size();
        region.flush(&model);
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
        session.kernel->executeEltwise(dim, pimBankType::ALL_BANK, KernelType::RELU, 512 + shift, 640 + shift, 0);
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
            session.kernel->executeEltwise(128, pimBankType::ALL_BANK, KernelType::RELU, 2256 + shift, 2384 + shift, 0);
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
    return scheme == "OURS" ? 1.1 : 1.5;
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


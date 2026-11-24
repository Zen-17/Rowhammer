#include <algorithm>
#include <cstdint>
#include <map>
#include <cstdlib>
#include <sys/stat.h>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "gtest/gtest.h"

#include "Burst.h"
#include "Phase1Address.h"
#include "Phase1Driver.h"
#include "Phase1Feeder.h"
#include "Phase1Storage.h"
#include "ReadDisturbanceModel.h"
#include "ActivationObserver.h"
#include "tests/PIMKernel.h"

using namespace DRAMSim;

namespace
{
bool injectStorage(unsigned bank, unsigned row, unsigned burstColumn, unsigned bit, bool parity, void* context)
{
    Phase1Storage* storage = static_cast<Phase1Storage*>(context);
    Phase1StorageKey key;
    key.channel = 0;
    key.rank = 0;
    key.actual_bank = bank;
    key.row = row;
    key.burst_column = burstColumn;
    key.codeword_index = 0;
    if (!storage->contains(key))
        return false;
    if (parity)
        storage->injectParityBit(key, bit);
    else
        storage->injectDataBit(key, bit);
    return true;
}

bool injectRank(unsigned bank, unsigned row, unsigned burstColumn, unsigned bit, bool parity, void* context)
{
    if (parity)
        return false;
    Rank* rank = static_cast<Rank*>(context);
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

uint32_t crc32(const std::string& text)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (unsigned char ch : text)
    {
        crc ^= ch;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

uint64_t gridSeed(const std::string& grid, int index)
{
    return (20261122ull + crc32(grid) + static_cast<unsigned>(index)) % 2147483648ull;
}

struct RunResult
{
    std::string workload;
    std::string scheme;
    std::string attack;
    double scale = 1;
    int index = 0;
    uint64_t seed = 0;
    std::string status = "SUCCESS";
    uint64_t cycles = 0;
    uint64_t pimItems = 0;
    uint64_t hostTransactions = 0;
    uint64_t attackTransactions = 0;
    uint64_t flips = 0;
    uint64_t firstFlipAct = 0;
    bool bitExact = true;
    bool replayed = false;
    bool withheld = false;
    bool recoveryFailed = false;
    uint64_t latent = 0;
};

std::string jsonEscape(const std::string& text)
{
    std::string out;
    for (char ch : text)
    {
        if (ch == '"' || ch == '\\')
            out.push_back('\'');
        else if (ch == '\n')
            out.push_back(' ');
        else
            out.push_back(ch);
    }
    return out;
}

void writeResult(std::ostream& out, const RunResult& row);

void archiveResult(const std::string& out, const RunResult& row)
{
    std::ostringstream id;
    id << row.workload << "_" << row.scheme << "_" << row.attack << "_s" << row.scale << "_" << row.index;
    const std::string dir = out + "/" + id.str();
    mkdir(dir.c_str(), 0755);
    const std::string path = dir + "/result.json";
    std::ifstream existing(path.c_str());
    if (existing.good())
        return;
    std::ofstream file(path.c_str());
    writeResult(file, row);
}

void writeResult(std::ostream& out, const RunResult& row)
{
    out << "{\"workload\":\"" << row.workload << "\",\"scheme\":\"" << row.scheme
        << "\",\"attack\":\"" << row.attack << "\",\"scale\":" << row.scale
        << ",\"index\":" << row.index << ",\"seed\":" << row.seed
        << ",\"status\":\"" << row.status << "\",\"cycles\":" << row.cycles
        << ",\"pim_items\":" << row.pimItems << ",\"host_transactions\":" << row.hostTransactions
        << ",\"attack_transactions\":" << row.attackTransactions << ",\"flips\":" << row.flips
        << ",\"first_flip_act\":" << row.firstFlipAct << ",\"bit_exact\":" << (row.bitExact ? "true" : "false")
        << ",\"replayed\":" << (row.replayed ? "true" : "false")
        << ",\"withheld\":" << (row.withheld ? "true" : "false")
        << ",\"recovery_failed\":" << (row.recoveryFailed ? "true" : "false")
        << ",\"latent_flips\":" << row.latent << "}\n";
}

uint64_t drain(Phase1FeederMemory& feeder, uint64_t watchdog)
{
    uint64_t cycles = 0;
    while (feeder.pendingCount() > 0 || !Phase1Driver::directIdle(feeder))
    {
        if (cycles >= watchdog)
            throw std::runtime_error("m1 watchdog exceeded");
        feeder.tick();
        feeder.update();
        cycles += 1;
    }
    return cycles;
}

void enqueueWrite(Phase1FeederMemory& feeder, const Phase1Address& codec, unsigned bank, unsigned row,
                  BurstType& data, const char* tag)
{
    feeder.addTransaction(true, codec.encode(Phase1AddrTuple{0, 0, bank, row, 0}), tag, &data);
}

void enqueueRead(Phase1FeederMemory& feeder, const Phase1Address& codec, unsigned bank, unsigned row,
                 BurstType& slot, const char* tag)
{
    feeder.addTransaction(false, codec.encode(Phase1AddrTuple{0, 0, bank, row, 0}), tag, &slot);
}

struct Session
{
    std::shared_ptr<Phase1FeederMemory> feeder;
    ActivationObserver observer;
    ReadDisturbanceModel model;
    Phase1Address codec;
    std::shared_ptr<PIMKernel> kernel;
    Rank* rank = nullptr;

    Session(const std::string& device, const std::string& system, const std::string& out, uint64_t seed,
            double scale)
        : feeder(std::make_shared<Phase1FeederMemory>(device, system, out, "phase3_m1", 256, 256)),
          observer(0),
          codec(Phase1Address::fromConfiguration())
    {
        feeder->setCPUClockSpeed(0);
        model.configure(seed, scale, "HBM2");
        for (MemorySystem* channel : feeder->channels)
            for (Rank* one : *channel->ranks)
            {
                rank = one;
                phase3Attach(one, &observer);
            }
        model.setInjector(&injectRank, rank);
        observer.attachDisturbance(&model);
        kernel = std::make_shared<PIMKernel>(feeder, 1, 1);
    }

    ~Session() { phase3DetachAll(); }
};

void runKernel(Session& session, const std::string& workload, uint64_t& pimItems)
{
    const size_t before = session.feeder->pendingCount();
    if (workload == "W2")
    {
        const int dim = 48 * 128;
        session.kernel->executeEltwise(dim, pimBankType::ALL_BANK, KernelType::ADD, 0, 256, 128);
        session.kernel->executeEltwise(dim, pimBankType::ALL_BANK, KernelType::MUL, 256, 512, 384);
        session.kernel->executeEltwise(dim, pimBankType::ALL_BANK, KernelType::RELU, 512, 640, 0);
    }
    else if (workload == "DEC")
    {
        NumpyBurstType weight;
        NumpyBurstType input;
        weight.bShape.push_back(256);
        weight.bShape.push_back(128);
        weight.bData.resize(4096);
        input.bShape.push_back(1);
        input.bShape.push_back(128);
        input.bData.resize(4096);
        phase3GemvRowBias = 0;
        for (int i = 0; i < 3; ++i)
            session.kernel->executeGemv(&weight, &input, false);
        const int dim = 128;
        session.kernel->executeEltwise(dim, pimBankType::ALL_BANK, KernelType::ADD, 2000, 2256, 2128);
        session.kernel->executeEltwise(dim, pimBankType::ALL_BANK, KernelType::RELU, 2256, 2384, 0);
    }
    else
    {
        throw std::runtime_error("m1 workload must be W2 or DEC");
    }
    pimItems += session.feeder->pendingCount() - before;
}

unsigned resultRow(const std::string& workload)
{
    return workload == "W2" ? 640u : 2384u;
}

RunResult oneRun(const std::string& device, const std::string& system, const std::string& out,
                 const std::string& workload, const std::string& scheme, const std::string& attack,
                 double scale, int index)
{
    const std::string grid = workload + "|" + scheme + "|" + attack + "|" + std::to_string(scale);
    RunResult row;
    row.workload = workload;
    row.scheme = scheme;
    row.attack = attack;
    row.scale = scale;
    row.index = index;
    row.seed = gridSeed(grid, index);
    Session session(device, system, out, row.seed, scale);
    BurstType pattern;
    for (int i = 0; i < 32; ++i)
        pattern.u8Data_[i] = static_cast<uint8_t>(0x40 + (index % 7));
    const unsigned victim = resultRow(workload);
    enqueueWrite(*session.feeder, session.codec, 0, victim, pattern, "HOST_BG");
    row.hostTransactions += 1;
    row.cycles += drain(*session.feeder, 2000000ull);
    runKernel(session, workload, row.pimItems);
    const uint64_t flipsBefore = session.model.flipCount();
    const uint64_t actsBefore = session.model.activateCount();
    if (attack != "NO_ATK")
    {
        // Hammer until the written result row takes a neighbor-induced flip.
        // Matching background writes keep the attack at half of host traffic.
        // Drive the disturbance model directly and issue the same host writes.
        // The observer stays detached for this phase so a REF side-pointer
        // cannot clear D before the locked threshold, and ACTs are not counted twice.
        session.observer.attachDisturbance(nullptr);
        const double need = session.model.firstThreshold(0, victim);
        const double fan = attack == "A2" ? 8.0 : 1.0;
        const uint64_t cap = attack == "A3" ? 1024ull : static_cast<uint64_t>(need * 1.25 * fan) + 64ull;
        BurstType background;
        const unsigned goal = attack == "A3" ? 0u : 2u;
        Phase3Attack plan(attack, victim);
        while (row.attackTransactions < cap &&
               (attack == "A3" || session.model.flipsOn(0, victim) < goal))
        {
            const unsigned aggressor = plan.nextAggressor();
            if (attack != "A3")
                session.model.noteActivate(0, aggressor);
            enqueueWrite(*session.feeder, session.codec, 0, aggressor, background, "HOST_ATK");
            enqueueWrite(*session.feeder, session.codec, 1,
                         static_cast<unsigned>(200 + (row.attackTransactions % 32)), background, "HOST_BG");
            row.attackTransactions += 1;
            row.hostTransactions += 1;
            if (row.attackTransactions % 256ull == 0)
                row.cycles += drain(*session.feeder, 8000000ull);
        }
        row.cycles += drain(*session.feeder, 8000000ull);
        session.observer.attachDisturbance(&session.model);
    }
    uint64_t backgroundHost = row.pimItems / 4;
    if (backgroundHost == 0)
        backgroundHost = 1;
    BurstType filler;
    for (uint64_t i = 0; i < backgroundHost; ++i)
    {
        enqueueWrite(*session.feeder, session.codec, 3, static_cast<unsigned>(300 + (i % 16)), filler, "HOST_BG");
        row.hostTransactions += 1;
    }
    row.cycles += drain(*session.feeder, 8000000ull);
    BurstType attacked;
    enqueueRead(*session.feeder, session.codec, 0, resultRow(workload), attacked, "HOST_BG");
    row.hostTransactions += 1;
    row.cycles += drain(*session.feeder, 2000000ull);

    static std::map<std::string, std::vector<uint8_t>> goldenBytes;
    const std::string goldenKey =
        workload + ":" + std::to_string(scale) + ":" + std::to_string(pattern.u8Data_[0]);
    BurstType reference;
    auto cached = goldenBytes.find(goldenKey);
    if (cached == goldenBytes.end())
    {
        Session golden(device, system, out, row.seed, scale);
        BurstType same;
        for (int i = 0; i < 32; ++i)
            same.u8Data_[i] = pattern.u8Data_[i];
        enqueueWrite(*golden.feeder, golden.codec, 0, victim, same, "HOST_BG");
        drain(*golden.feeder, 2000000ull);
        uint64_t ignored = 0;
        runKernel(golden, workload, ignored);
        enqueueRead(*golden.feeder, golden.codec, 0, resultRow(workload), reference, "HOST_BG");
        drain(*golden.feeder, 8000000ull);
        goldenBytes[goldenKey] = std::vector<uint8_t>(reference.u8Data_, reference.u8Data_ + 32);
    }
    else
    {
        for (int i = 0; i < 32; ++i)
            reference.u8Data_[i] = cached->second[static_cast<size_t>(i)];
    }
    row.bitExact = true;
    for (int i = 0; i < 32; ++i)
    {
        if (attacked.u8Data_[i] != reference.u8Data_[i])
            row.bitExact = false;
    }
    row.flips = session.model.flipCount() - flipsBefore;
    uint64_t seen = 0;
    uint64_t curveCount = 0;
    std::ofstream curve;
    const bool recordCurve = workload == "W2" && scheme == "NONE" && attack == "A1" && index == 0;
    if (recordCurve)
        curve.open((out + "/flip_curve.csv").c_str(), std::ios::app);
    for (const ReadDisturbanceModel::Flip& flip : session.model.flips())
    {
        if (seen++ < flipsBefore)
            continue;
        const uint64_t relative = flip.actOrdinal > actsBefore ? flip.actOrdinal - actsBefore : 0;
        if (row.firstFlipAct == 0)
            row.firstFlipAct = relative;
        if (recordCurve)
            curve << row.scale << "," << relative << "," << ++curveCount << "\n";
    }
    row.latent = row.flips;
    if (scheme == "REACT" && attack != "NO_ATK")
    {
        uint64_t replayItems = 0;
        runKernel(session, workload, replayItems);
        row.cycles += drain(*session.feeder, 8000000ull);
        row.replayed = true;
        BurstType after;
        enqueueRead(*session.feeder, session.codec, 0, resultRow(workload), after, "HOST_BG");
        row.cycles += drain(*session.feeder, 2000000ull);
        bool stillBad = false;
        for (int i = 0; i < 32; ++i)
        {
            if (after.u8Data_[i] != reference.u8Data_[i])
                stillBad = true;
        }
        if (stillBad)
        {
            row.withheld = true;
            row.recoveryFailed = true;
            row.bitExact = false;
        }
        else
        {
            row.bitExact = true;
        }
    }
    if (row.flips > 0 && resultRow(workload) != victim)
        row.latent = row.flips;
    return row;
}
}  // namespace

TEST(Phase3M1, DisturbanceThreshold)
{
    Phase1Storage storage;
    Phase1StorageKey key;
    key.row = 10;
    for (unsigned column = 0; column < 32; ++column)
    {
        key.burst_column = column;
        storage.initialize(key, 0x1234ull);
    }
    key.burst_column = 0;
    const uint8_t originalParity = storage.readRawParity(key);
    ReadDisturbanceModel model;
    model.configure(7, 1.0, "HBM2");
    model.setInjector(&injectStorage, &storage);
    model.forceRow(0, 10, 4.0, 1.0);
    for (int i = 0; i < 7; ++i)
        model.noteActivate(0, 11);
    EXPECT_EQ(0u, model.flipCount());
    EXPECT_DOUBLE_EQ(7.0, model.disturbance(0, 10));
    model.noteActivate(0, 11);
    ASSERT_EQ(1u, model.flipCount());
    if (!model.flips()[0].parity)
    {
        key.burst_column = model.flips()[0].burstColumn;
        EXPECT_NE(0x1234ull, storage.readRawData(key));
    }
    else
    {
        key.burst_column = model.flips()[0].burstColumn;
        EXPECT_NE(originalParity, storage.readRawParity(key));
    }
}

TEST(Phase3M1, DisturbanceRefresh)
{
    ReadDisturbanceModel model;
    model.configure(9, 1.0, "HBM2");
    model.forceRow(0, 10, 4.0, 1.0);
    for (int i = 0; i < 4; ++i)
        model.noteActivate(0, 11);
    EXPECT_DOUBLE_EQ(4.0, model.disturbance(0, 10));
    model.noteRefresh(0, 10);
    EXPECT_DOUBLE_EQ(0.0, model.disturbance(0, 10));
    for (int i = 0; i < 7; ++i)
        model.noteActivate(0, 11);
    EXPECT_EQ(0u, model.flipCount());
}

TEST(Phase3M1, DisturbanceSeed)
{
    std::vector<ReadDisturbanceModel::Flip> first;
    std::vector<ReadDisturbanceModel::Flip> second;
    for (int pass = 0; pass < 2; ++pass)
    {
        ReadDisturbanceModel model;
        model.configure(99, 0.0625, "HBM2");
        model.forceRow(1, 20, 14531.0, 1.0);
        for (int i = 0; i < 4000; ++i)
            model.noteActivate(1, 21);
        (pass == 0 ? first : second) = model.flips();
    }
    ASSERT_FALSE(first.empty());
    ASSERT_EQ(first.size(), second.size());
    for (size_t i = 0; i < first.size(); ++i)
    {
        EXPECT_EQ(first[i].bit, second[i].bit);
        EXPECT_EQ(first[i].burstColumn, second[i].burstColumn);
        EXPECT_EQ(first[i].parity, second[i].parity);
        EXPECT_EQ(first[i].actOrdinal, second[i].actOrdinal);
    }
}

TEST(Phase3M1, AttackShapes)
{
    Phase3Attack evasive("A3", 100);
    unsigned counts[ReadDisturbanceModel::kRows] = {};
    for (int i = 0; i < 1024; ++i)
        counts[evasive.nextAggressor()] += 1;
    unsigned hottest = 0;
    for (unsigned count : counts)
        hottest = std::max(hottest, count);
    EXPECT_LT(hottest, 64u);
    Phase3Attack deferred("A4", 100);
    EXPECT_TRUE(deferred.interfaceOnly());
    EXPECT_THROW(deferred.nextAggressor(), std::logic_error);
    unsigned pointed = 40;
    deferred.setMappingPointer([](void* context) { return *static_cast<unsigned*>(context); }, &pointed);
    const unsigned row = deferred.nextAggressor();
    EXPECT_TRUE(row == 39 || row == 41);
}

TEST(Phase3M1, Campaign)
{
    const char* run = std::getenv("PHASE3_M1_RUN");
    if (run == nullptr || (std::string(run) != "campaign" && std::string(run) != "pilot"))
        return;
    const std::string device = std::getenv("PHASE3_DEVICE_INI");
    const std::string system = std::getenv("PHASE3_SYSTEM_INI");
    const std::string out = std::getenv("PHASE3_OUT");
    std::ofstream raw((out + "/m1_runs.jsonl").c_str());
    if (!raw)
        throw std::runtime_error("cannot write m1_runs.jsonl");
    if (std::string(run) == "pilot")
    {
        const std::string attacks[] = {"A1", "A2", "A3"};
        for (const std::string& attack : attacks)
        {
            for (int index = 0; index < 5; ++index)
            {
                RunResult row = oneRun(device, system, out, "W2", "NONE", attack, 0.0625, index);
                writeResult(std::cout, row);
                if (attack == "A3")
                {
                    EXPECT_EQ(1024u, row.attackTransactions);
                }
                else
                {
                    EXPECT_GE(row.flips, 2u);
                    EXPECT_FALSE(row.bitExact);
                }
            }
        }
        for (const char* scheme : {"NONE", "REACT"})
        {
            RunResult clean = oneRun(device, system, out, "W2", scheme, "NO_ATK", 0.0625, 0);
            writeResult(std::cout, clean);
            EXPECT_TRUE(clean.bitExact);
            EXPECT_EQ(0u, clean.flips);
        }
        return;
    }
    {
        std::ofstream curve((out + "/flip_curve.csv").c_str());
        curve << "scale,act_ordinal,cumulative_flips\n";
    }
    const std::string workloads[] = {"W2", "DEC"};
    const std::string schemes[] = {"NO_ATK", "NONE", "REACT"};
    for (const std::string& workload : workloads)
    {
        for (const std::string& scheme : schemes)
        {
            const std::string attack = scheme == "NO_ATK" ? "NO_ATK" : "A1";
            const int trials = scheme == "NO_ATK" ? 30 : 30;
            for (int index = 0; index < trials; ++index)
            {
                RunResult row = oneRun(device, system, out, workload, scheme, attack, 0.0625, index);
                writeResult(raw, row);
                archiveResult(out, row);
                raw.flush();
            }
            for (int index = 0; index < 3; ++index)
            {
                RunResult row = oneRun(device, system, out, workload, scheme, attack, 1.0, index);
                writeResult(raw, row);
                archiveResult(out, row);
                raw.flush();
            }
        }
    }
}

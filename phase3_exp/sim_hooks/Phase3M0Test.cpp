#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "gtest/gtest.h"

#include "ActivationObserver.h"
#include "Burst.h"
#include "Phase1Address.h"
#include "Phase1Driver.h"
#include "Phase1Feeder.h"
#include "SystemConfiguration.h"
#include "tests/KernelAddrGen.h"
#include "tests/PIMKernel.h"

using namespace DRAMSim;

namespace
{
std::string trim(const std::string& input)
{
    const std::string ws = " \t\r\n";
    const size_t first = input.find_first_not_of(ws);
    if (first == std::string::npos)
        return "";
    const size_t last = input.find_last_not_of(ws);
    return input.substr(first, last - first + 1);
}

std::string requireEnv(const char* name)
{
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0')
        throw std::runtime_error(std::string("missing environment variable ") + name);
    return value;
}

struct KernelSpec
{
    std::string op;
    int in0 = 0;
    int result = 0;
    int in1 = 0;
};

struct Job
{
    std::string name;
    std::string kind;
    int nTiles = 1;
    double hostShare = 0.2;
    int gemvCount = 1;
    int weight0 = 64;
    int weight1 = 16;
    int input0 = 1;
    int input1 = 16;
    std::vector<KernelSpec> kernels;
};

std::vector<Job> readJobs(const std::string& path)
{
    std::ifstream input(path.c_str());
    if (!input)
        throw std::runtime_error("cannot open jobs file: " + path);
    std::vector<Job> jobs;
    Job current;
    bool open = false;
    std::string line;
    while (std::getline(input, line))
    {
        line = trim(line);
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream row(line);
        std::string tag;
        row >> tag;
        if (tag == "begin")
        {
            current = Job();
            row >> current.name >> current.kind;
            open = true;
        }
        else if (tag == "end")
        {
            if (!open)
                throw std::runtime_error("end without begin");
            jobs.push_back(current);
            open = false;
        }
        else if (tag == "n_tiles")
            row >> current.nTiles;
        else if (tag == "host_bw_share")
            row >> current.hostShare;
        else if (tag == "gemv_count")
            row >> current.gemvCount;
        else if (tag == "weight")
            row >> current.weight0 >> current.weight1;
        else if (tag == "input")
            row >> current.input0 >> current.input1;
        else if (tag == "kernel")
        {
            KernelSpec kernel;
            row >> kernel.op >> kernel.in0 >> kernel.result >> kernel.in1;
            current.kernels.push_back(kernel);
        }
        else
        {
            throw std::runtime_error("unknown jobs tag: " + tag);
        }
    }
    if (open)
        throw std::runtime_error("unclosed job");
    return jobs;
}

KernelType kernelType(const std::string& name)
{
    if (name == "ADD")
        return KernelType::ADD;
    if (name == "MUL")
        return KernelType::MUL;
    if (name == "RELU")
        return KernelType::RELU;
    throw std::runtime_error("unsupported kernel " + name);
}

struct IdleRun
{
    uint64_t cycles = 0;
    uint64_t refresh = 0;
    uint64_t observedRefresh = 0;
    uint64_t rowsRefreshed = 0;
};

IdleRun runIdle(const std::string& device, const std::string& system, const std::string& pwd,
                bool observe)
{
    phase3DetachAll();
    auto feeder = std::make_shared<Phase1FeederMemory>(device, system, pwd, "phase3_refresh", 256, 8);
    feeder->setCPUClockSpeed(0);
    std::unique_ptr<ActivationObserver> observer;
    if (observe)
    {
        observer.reset(new ActivationObserver(0));
        for (MemorySystem* channel : feeder->channels)
            for (Rank* rank : *channel->ranks)
                phase3Attach(rank, observer.get());
    }
    const uint64_t steps = ActivationObserver::kWindowCycles;
    for (uint64_t i = 0; i < steps; ++i)
        feeder->update();
    IdleRun result;
    result.cycles = feeder->currentClockCycle;
    for (MemorySystem* channel : feeder->channels)
        for (Rank* rank : *channel->ranks)
            result.refresh += rank->refreshCount;
    if (observer)
    {
        result.observedRefresh = observer->refreshCommands();
        result.rowsRefreshed = observer->rowsRefreshed();
    }
    phase3DetachAll();
    return result;
}

void enqueueHost(Phase1FeederMemory& feeder, const Phase1Address& codec, std::vector<BurstType>& reads,
                 uint64_t count, uint64_t& cursor, unsigned avoidLo, unsigned avoidHi)
{
    if (reads.capacity() < reads.size() + count)
        throw std::logic_error("host read buffer would reallocate");
    BurstType writeData;
    for (uint64_t i = 0; i < count; ++i)
    {
        unsigned row = static_cast<unsigned>((cursor * 1103515245ull + 12345ull) % 8192ull);
        if (avoidHi > avoidLo && row >= avoidLo && row < avoidHi)
            row = (avoidHi + static_cast<unsigned>(cursor % 64ull)) % 8192u;
        const unsigned bank = static_cast<unsigned>((cursor * 17ull) % 16ull);
        const bool isWrite = (cursor % 2ull) == 0;
        cursor += 1;
        const uint64_t address = codec.encode(Phase1AddrTuple{0, 0, bank, row, 0});
        if (isWrite)
            feeder.addTransaction(true, address, std::string("HOST_BG"), &writeData);
        else
        {
            reads.emplace_back();
            feeder.addTransaction(false, address, std::string("HOST_BG"), &reads.back());
        }
    }
}

uint64_t drain(Phase1FeederMemory& feeder, uint64_t watchdog)
{
    uint64_t cycles = 0;
    while (feeder.pendingCount() > 0 || !Phase1Driver::directIdle(feeder))
    {
        if (cycles >= watchdog)
            throw std::runtime_error("workload watchdog exceeded");
        feeder.tick();
        feeder.update();
        cycles += 1;
    }
    return cycles;
}

void writeMeta(const std::string& path, const std::string& workload, const std::string& status,
               uint64_t cycles, uint64_t host, uint64_t pim, const std::vector<std::string>& phases,
               const std::string& error)
{
    std::ofstream out(path.c_str());
    if (!out)
        throw std::runtime_error("cannot write " + path);
    out << "{\n  \"workload\": \"" << workload << "\",\n  \"status\": \"" << status
        << "\",\n  \"cycles\": " << cycles << ",\n  \"host_transactions\": " << host
        << ",\n  \"pim_items\": " << pim << ",\n  \"phases\": [";
    for (size_t i = 0; i < phases.size(); ++i)
    {
        if (i)
            out << ", ";
        out << "\"" << phases[i] << "\"";
    }
    std::string safe = error;
    for (size_t i = 0; i < safe.size(); ++i)
    {
        if (safe[i] == '"' || safe[i] == '\\')
            safe[i] = '\'';
        if (safe[i] == '\n')
            safe[i] = ' ';
    }
    out << "],\n  \"error\": \"" << safe << "\"\n}\n";
}

void runJob(const Job& job, const std::string& device, const std::string& system,
            const std::string& outDir, const std::string& mode)
{
    phase3DetachAll();
    auto feeder = std::make_shared<Phase1FeederMemory>(device, system, outDir,
                                                       "phase3_" + job.name, 256, 256);
    feeder->setCPUClockSpeed(0);
    ActivationObserver observer(0);
    for (MemorySystem* channel : feeder->channels)
        for (Rank* rank : *channel->ranks)
            phase3Attach(rank, &observer);
    const Phase1Geometry geometry = Phase1Address::fromConfiguration();
    Phase1Address codec(geometry);
    auto kernel = std::make_shared<PIMKernel>(feeder, 1, 1);
    std::vector<BurstType> reads;
    reads.reserve(1 << 20);
    uint64_t cursor = 1;
    uint64_t host = 0;
    uint64_t pim = 0;
    std::vector<std::string> phases;
    const uint64_t beforeCycle = feeder->currentClockCycle;

    unsigned avoidLo = 0;
    unsigned avoidHi = 0;
    auto addHostFor = [&](uint64_t items) {
        uint64_t count = items / 4;
        if (count == 0)
            count = 1;
        phases.push_back("HOST");
        enqueueHost(*feeder, codec, reads, count, cursor, avoidLo, avoidHi);
        host += count;
    };
    auto runKernels = [&](const std::vector<KernelSpec>& specs) {
        const int dim = job.nTiles * 128;
        for (const KernelSpec& spec : specs)
        {
            const size_t before = feeder->pendingCount();
            phases.push_back("PIM");
            kernel->executeEltwise(dim, pimBankType::ALL_BANK, kernelType(spec.op), spec.in0,
                                   spec.result, spec.in1);
            const uint64_t items = feeder->pendingCount() - before;
            pim += items;
            addHostFor(items);
        }
    };
    auto compactFanout = [](int base) {
        std::vector<KernelSpec> specs;
        specs.push_back(KernelSpec{"ADD", base, base + 2, base + 1});
        for (int i = 0; i < 16; ++i)
        {
            const int in1 = base + 3 + i;
            const int result = base + 19 + i;
            specs.push_back(KernelSpec{"MUL", base + 2, result, in1});
            specs.push_back(KernelSpec{"RELU", result, base + 35 + i, 0});
        }
        return specs;
    };

    // Streaming moves each batch onto a fresh row block.  Reuse keeps the
    // compute graph's addresses, including fan-out reads of the same source
    // and GEMV/DEC weight rows, and only the input vector changes.
    const bool streaming = mode == "streaming";
    const unsigned targetWindows = 16;
    int stride = 320;
    if (job.kind == "fanout")
        stride = 64;
    else if (job.kind == "gemv")
        stride = 160;
    else if (job.kind == "dec")
        stride = 256;
    else if (job.name == "W2")
        stride = 768;
    int batch = 0;
    uint64_t cycles = 0;
    const int grf = 8;
    const int inputTiles = std::max(1, (job.weight1 + grf - 1) / grf);
    const size_t bursts = static_cast<size_t>(std::max(job.input0, 1) * grf * inputTiles + grf);
    NumpyBurstType weight;
    NumpyBurstType input;
    if (job.kind == "gemv" || job.kind == "dec")
    {
        weight.bShape.push_back(static_cast<unsigned long>(job.weight0));
        weight.bShape.push_back(static_cast<unsigned long>(job.weight1));
        weight.bData.resize(std::max<size_t>(bursts, 1));
        input.bShape.push_back(static_cast<unsigned long>(job.input0));
        input.bShape.push_back(static_cast<unsigned long>(job.input1));
        input.bData.resize(std::max<size_t>(bursts, 1));
    }
    while (batch < 80 && (batch == 0 || observer.minCompleteWindows() < targetWindows))
    {
        const int shift = streaming ? batch * stride : 0;
        if (streaming && shift >= 7800)
            break;
        if (job.kind == "fanout")
            runKernels(streaming ? compactFanout(shift) : job.kernels);
        else if (job.kind == "eltwise")
        {
            std::vector<KernelSpec> specs;
            for (const KernelSpec& spec : job.kernels)
            {
                specs.push_back(KernelSpec{spec.op, spec.in0 + shift, spec.result + shift,
                                           spec.in1 + shift});
            }
            runKernels(specs);
        }
        else if (job.kind == "gemv" || job.kind == "dec")
        {
            for (size_t lane = 0; lane < input.bData.size(); ++lane)
                input.bData[lane].fp16Data_[0] = static_cast<uint16_t>(batch + lane + 1);
            const int repeats = (job.kind == "dec") ? job.gemvCount : 1;
            phase3GemvRowBias = streaming ? shift : 0;
            for (int i = 0; i < repeats; ++i)
            {
                const size_t before = feeder->pendingCount();
                phases.push_back("PIM");
                kernel->executeGemv(&weight, &input, false);
                const uint64_t items = feeder->pendingCount() - before;
                pim += items;
                addHostFor(items);
            }
            phase3GemvRowBias = 0;
            if (job.kind == "dec")
            {
                std::vector<KernelSpec> specs = job.kernels;
                if (streaming)
                {
                    specs.clear();
                    specs.push_back(KernelSpec{"ADD", shift + 180, shift + 182, shift + 181});
                    specs.push_back(KernelSpec{"RELU", shift + 182, shift + 183, 0});
                }
                runKernels(specs);
            }
        }
        else
        {
            throw std::runtime_error("unknown kind " + job.kind);
        }
        cycles += drain(*feeder, 8000000ull);
        batch += 1;
    }
    phase3GemvRowBias = 0;
    observer.flush();
    observer.writeJson(outDir + "/" + job.name + "." + mode + ".obs.json", job.name, mode);
    const bool alternates = host > 0 && pim > 0 && phases.size() >= 2 &&
                            phases.front() == "PIM" && phases[1] == "HOST";
    if (!alternates)
        throw std::runtime_error("host and PIM traffic did not alternate");
    if (feeder->currentClockCycle < beforeCycle)
        throw std::runtime_error("clock moved backwards");
    writeMeta(outDir + "/" + job.name + "." + mode + ".meta.json", job.name, "SUCCESS", cycles, host,
              pim, phases, "");
    phase3DetachAll();
}
}  // namespace

TEST(Phase3M0, OpenedBanks)
{
    const std::vector<int> single = phase3OpenedBanks(dramMode::SB, 3, 16);
    ASSERT_EQ(1u, single.size());
    EXPECT_EQ(3, single[0]);
    const std::vector<int> all = phase3OpenedBanks(dramMode::HAB, 0, 16);
    ASSERT_EQ(8u, all.size());
    for (int bank : all)
        EXPECT_EQ(0, bank % 2);
    const std::vector<int> odd = phase3OpenedBanks(dramMode::HAB_PIM, 1, 16);
    ASSERT_EQ(8u, odd.size());
    for (int bank : odd)
        EXPECT_EQ(1, bank % 2);
}

TEST(Phase3M0, RefreshWindow)
{
    const std::string device = requireEnv("PHASE3_DEVICE_INI");
    const std::string system = requireEnv("PHASE3_SYSTEM_INI");
    const std::string out = requireEnv("PHASE3_OUT");
    const IdleRun plain = runIdle(device, system, out, false);
    const IdleRun hooked = runIdle(device, system, out, true);
    EXPECT_EQ(plain.cycles, hooked.cycles);
    EXPECT_EQ(plain.refresh, hooked.refresh);
    EXPECT_EQ(ActivationObserver::kWindowCycles, hooked.cycles);
    EXPECT_GE(hooked.refresh, 8000u);
    EXPECT_LE(hooked.refresh, 8400u);
    EXPECT_EQ(hooked.refresh, hooked.observedRefresh);
    EXPECT_EQ(16384u, hooked.rowsRefreshed);
    std::ofstream outFile((out + "/refresh_window.json").c_str());
    outFile << "{\n  \"cycles_plain\": " << plain.cycles
            << ",\n  \"cycles_hooked\": " << hooked.cycles
            << ",\n  \"refresh_plain\": " << plain.refresh
            << ",\n  \"refresh_hooked\": " << hooked.refresh
            << ",\n  \"observed_refresh\": " << hooked.observedRefresh
            << ",\n  \"rows_refreshed\": " << hooked.rowsRefreshed
            << ",\n  \"cycle_delta\": " << (hooked.cycles - plain.cycles) << "\n}\n";
}

TEST(Phase3M0, Workloads)
{
    const std::string device = requireEnv("PHASE3_DEVICE_INI");
    const std::string system = requireEnv("PHASE3_SYSTEM_INI");
    const std::string out = requireEnv("PHASE3_OUT");
    const std::string jobsPath = requireEnv("PHASE3_JOBS");
    const std::vector<Job> jobs = readJobs(jobsPath);
    ASSERT_EQ(5u, jobs.size());
    for (const Job& job : jobs)
    {
        try
        {
            runJob(job, device, system, out, "streaming");
            runJob(job, device, system, out, "reuse");
        }
        catch (const std::exception& error)
        {
            writeMeta(out + "/" + job.name + ".meta.json", job.name, "FAILED", 0, 0, 0, {},
                      error.what());
            FAIL() << job.name << ": " << error.what();
        }
    }
}

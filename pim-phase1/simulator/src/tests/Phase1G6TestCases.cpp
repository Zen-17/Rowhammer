#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "tests/PIMKernel.h"
#include "Phase1Address.h"
#include "Phase1Driver.h"
#include "Phase1Feeder.h"
#include "Phase1Pause.h"
#include "Phase1Reports.h"
#include "Phase1Trace.h"

using namespace DRAMSim;

namespace
{
std::string trim(const std::string& input)
{
    const std::string ws = " \t\r\n";
    const std::size_t first = input.find_first_not_of(ws);
    if (first == std::string::npos)
        return "";
    const std::size_t last = input.find_last_not_of(ws);
    return input.substr(first, last - first + 1);
}

std::map<std::string, std::string> readParams(const std::string& path)
{
    std::ifstream input(path);
    if (!input)
        throw std::runtime_error("cannot open G6 params: " + path);
    std::map<std::string, std::string> values;
    std::string line;
    while (std::getline(input, line))
    {
        line = trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#')
            continue;
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos)
            throw std::runtime_error("malformed G6 param line");
        values[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
    return values;
}

std::string get(const std::map<std::string, std::string>& p, const std::string& key)
{
    auto it = p.find(key);
    if (it == p.end())
        throw std::runtime_error("missing G6 param: " + key);
    return it->second;
}

unsigned getU(const std::map<std::string, std::string>& p, const std::string& key)
{
    return static_cast<unsigned>(std::stoul(get(p, key)));
}

std::string join(const std::string& dir, const std::string& name)
{
    if (dir.empty())
        return name;
    return (dir.back() == '/') ? dir + name : dir + "/" + name;
}

void writeEmptyProtocol(const std::string& dir)
{
    std::ofstream reports(join(dir, "reports.csv"));
    reports << "report_seq,report_cycle,error_class,region_type,region_id\n";
    reports.close();
    std::ofstream truth(join(dir, "truth.csv"));
    truth << "record_type,detection_id,fault_id,fault_cycle,storage_key,codeword_index,"
             "bit_positions,raw_before,raw_after,target_access_id,detect_cycle,decoder_status,"
             "eligible_report_id,dropped\n";
    truth.close();
    Phase1Trace events(join(dir, "events.csv"), false);
}

uint64_t dataAccessCount(MultiChannelMemorySystem& memory, unsigned channel)
{
    uint64_t total = 0;
    for (Rank* rank : *memory.channels[channel]->ranks)
        for (Bank& bank : rank->banks)
            total += bank.dataAccessCount();
    return total;
}

uint64_t refreshCount(MultiChannelMemorySystem& memory, unsigned channel)
{
    uint64_t total = 0;
    for (Rank* rank : *memory.channels[channel]->ranks)
        total += rank->refreshCount;
    return total;
}

struct LoopStats
{
    uint64_t cycles = 0;
    bool timeout = false;
};

LoopStats runSyntheticPre(MultiChannelMemorySystem& memory, Phase1FeederMemory& feeder,
                          Phase1PauseController& pause, unsigned cycles, uint64_t watchdog,
                          bool nearCompletion)
{
    LoopStats stats;
    for (unsigned i = 0; i < cycles && stats.cycles < watchdog; i++)
    {
        if (nearCompletion && feeder.pendingCount() <= 16)
            break;
        feeder.tick();
        memory.update();
        pause.tick(memory.currentClockCycle);
        stats.cycles++;
    }
    return stats;
}

LoopStats drainToAck(MultiChannelMemorySystem& memory, Phase1PauseController& pause,
                     uint64_t watchdog, bool tickOtherChannel, Phase1FeederMemory* feeder,
                     unsigned otherChannel)
{
    LoopStats stats;
    while (!pause.acked())
    {
        if (stats.cycles >= watchdog)
        {
            stats.timeout = true;
            break;
        }
        if (tickOtherChannel && feeder != nullptr)
            feeder->tickChannel(otherChannel);
        memory.update();
        pause.tick(memory.currentClockCycle);
        stats.cycles++;
        if (!pause.requested())
            break;
    }
    return stats;
}

void writeMetrics(const std::string& path, const std::string& scenario, unsigned channels,
                  unsigned feederLimit, unsigned handlerDelay, const std::string& reason,
                  const Phase1PauseController& pause, uint64_t preCycles,
                  uint64_t externalPending, uint64_t dataBefore, uint64_t dataAfter,
                  uint64_t refreshBefore, uint64_t refreshAfter, uint64_t observationCycles,
                  uint64_t clockBefore, uint64_t clockAfter, bool timeout,
                  unsigned reportCount = 0, unsigned reportCycle = 0,
                  uint64_t otherChannelDataBefore = 0, uint64_t otherChannelDataAfter = 0)
{
    std::ofstream out(path);
    if (!out)
        throw std::runtime_error("cannot open G6 metrics");
    out << "{\n"
        << "  \"scenario\": \"" << scenario << "\",\n"
        << "  \"channels\": " << channels << ",\n"
        << "  \"feeder_limit\": " << feederLimit << ",\n"
        << "  \"handler_delay\": " << handlerDelay << ",\n"
        << "  \"reason\": \"" << reason << "\",\n"
        << "  \"requested\": " << (pause.requested() ? "true" : "false") << ",\n"
        << "  \"acked\": " << (pause.acked() ? "true" : "false") << ",\n"
        << "  \"request_cycle\": " << pause.requestCycle() << ",\n"
        << "  \"ack_cycle\": " << pause.ackCycle() << ",\n"
        << "  \"quiesce_cycles\": " << (pause.acked() ? pause.quiesceCycles() : 0) << ",\n"
        << "  \"pre_cycles\": " << preCycles << ",\n"
        << "  \"external_pending_frozen\": " << externalPending << ",\n"
        << "  \"data_before_ack\": " << dataBefore << ",\n"
        << "  \"data_after_observation\": " << dataAfter << ",\n"
        << "  \"data_delta_after_ack\": " << static_cast<long long>(dataAfter - dataBefore) << ",\n"
        << "  \"refresh_before\": " << refreshBefore << ",\n"
        << "  \"refresh_after\": " << refreshAfter << ",\n"
        << "  \"observation_cycles\": " << observationCycles << ",\n"
        << "  \"clock_before\": " << clockBefore << ",\n"
        << "  \"clock_after\": " << clockAfter << ",\n"
        << "  \"clock_delta\": " << (clockAfter - clockBefore) << ",\n"
        << "  \"report_count\": " << reportCount << ",\n"
        << "  \"report_cycle\": " << reportCycle << ",\n"
        << "  \"other_channel_data_before\": " << otherChannelDataBefore << ",\n"
        << "  \"other_channel_data_after\": " << otherChannelDataAfter << ",\n"
        << "  \"other_channel_data_delta\": "
        << static_cast<long long>(otherChannelDataAfter - otherChannelDataBefore) << ",\n"
        << "  \"timeout\": " << (timeout ? "true" : "false") << "\n"
        << "}\n";
}
}  // namespace

TEST(Phase1G6Fixture, RunCase)
{
    const char* configPath = std::getenv("PHASE1_CONFIG");
    const char* outputPath = std::getenv("PHASE1_OUT");
    if (configPath == nullptr)
        GTEST_SKIP() << "PHASE1_CONFIG is not set";
    ASSERT_NE(nullptr, outputPath);
    const std::string outputDir(outputPath);
    writeEmptyProtocol(outputDir);

    try
    {
        const auto params = readParams(configPath);
        const std::string scenario = get(params, "scenario");
        const unsigned channels = getU(params, "channels");
        const unsigned capacity = getU(params, "capacity_mb");
        const unsigned feederLimit = getU(params, "feeder_limit");
        const unsigned handlerDelay = getU(params, "handler_delay");
        const unsigned observation = getU(params, "observation_cycles");
        const uint64_t watchdog = std::stoull(get(params, "watchdog_cycles"));
        const std::string deviceIni = get(params, "device_ini");
        const std::string systemIni = get(params, "system_ini");

        auto feeder = std::make_shared<Phase1FeederMemory>(deviceIni, systemIni, ".", "phase1_g6",
                                                           capacity, feederLimit);
        feeder->setCPUClockSpeed(0);
        Phase1PauseController pause(*feeder, 0, handlerDelay);
        Phase1Geometry geometry = Phase1Address::fromConfiguration();
        ASSERT_EQ(channels, geometry.num_channels);

        uint64_t reportCycle = 0;
        unsigned reportCount = 0;
        uint64_t preCycles = 0;
        uint64_t dataBefore = 0;
        uint64_t dataAfter = 0;
        uint64_t refreshBefore = 0;
        uint64_t refreshAfter = 0;
        uint64_t clockBefore = 0;
        uint64_t clockAfter = 0;
        uint64_t otherBefore = 0;
        uint64_t otherAfter = 0;
        bool timeout = false;
        std::string reason = "SYNTHETIC_CONTROL";

        if (scenario == "SYNTHETIC_BEFORE" || scenario == "SYNTHETIC_QUEUE" ||
            scenario == "SYNTHETIC_INFLIGHT" || scenario == "SYNTHETIC_NEAR")
        {
            ASSERT_EQ(1u, channels);
            const unsigned nTiles = getU(params, "n_tiles");
            const unsigned dim = nTiles * 16u * 1u * 1u * 8u;
            std::vector<BurstType> input0(dim), input1(dim);
            for (unsigned b = 0; b < dim; b++)
                for (unsigned lane = 0; lane < 16; lane++)
                {
                    const uint64_t i = static_cast<uint64_t>(b) * 16u + lane;
                    input0[b].fp16Data_[lane] = convertF2H(
                        static_cast<float>((static_cast<long long>(i) + 0) % 17 - 8) / 8.0f);
                    input1[b].fp16Data_[lane] = convertF2H(
                        static_cast<float>((3 * static_cast<long long>(i) + 0) % 13 - 6) / 8.0f);
                }
            NumpyBurstType a;
            a.bShape.push_back(dim);
            a.bData = input0;
            NumpyBurstType x;
            x.bShape.push_back(dim);
            x.bData = input1;
            auto kernel = std::make_shared<PIMKernel>(feeder, 1, 1);
            kernel->preloadNoReplacement(&a, 0, 0);
            kernel->preloadNoReplacement(&x, 128, 0);
            kernel->executeEltwise(static_cast<int>(dim), pimBankType::ALL_BANK, KernelType::ADD,
                                   0, 256, 128);
            unsigned pre = 0;
            if (scenario == "SYNTHETIC_BEFORE")
                pre = 0;
            else if (scenario == "SYNTHETIC_QUEUE")
                pre = 1;
            else if (scenario == "SYNTHETIC_INFLIGHT")
                pre = 100;
            else
                pre = 100000;
            preCycles = runSyntheticPre(*feeder, *feeder, pause, pre, watchdog,
                                        scenario == "SYNTHETIC_NEAR").cycles;
            if (scenario == "SYNTHETIC_BEFORE") pause.request(0, "SYNTHETIC_CONTROL");
            else pause.request(feeder->currentClockCycle, "SYNTHETIC_CONTROL");
            LoopStats drain = drainToAck(*feeder, pause, watchdog, false, nullptr, 1);
            timeout = drain.timeout;
            preCycles += drain.cycles;
            dataBefore = dataAccessCount(*feeder, 0);
            refreshBefore = refreshCount(*feeder, 0);
            clockBefore = feeder->currentClockCycle;
            for (unsigned i = 0; i < observation; i++)
            {
                feeder->update();
            }
            dataAfter = dataAccessCount(*feeder, 0);
            refreshAfter = refreshCount(*feeder, 0);
            clockAfter = feeder->currentClockCycle;
        }
        else if (scenario == "ACTUAL_CE")
        {
            ASSERT_EQ(1u, channels);
            Phase1Address reportCodec(geometry);
            Phase1Reports reports(reportCodec, geometry.num_channels, geometry.num_banks,
                                  "bank", 0, 1, 64, true, true);
            for (MemorySystem* channel : feeder->channels)
                for (Rank* rank : *channel->ranks)
                    for (Bank& bank : rank->banks)
                        bank.setPhase1(Phase1Profile::LOCAL_VISIBLE, &reports);

            const unsigned dim = 1u * 16u * 1u * 1u * 8u;
            std::vector<BurstType> input0(dim), input1(dim);
            for (unsigned b = 0; b < dim; b++)
                for (unsigned lane = 0; lane < 16; lane++)
                {
                    const uint64_t i = static_cast<uint64_t>(b) * 16u + lane;
                    input0[b].fp16Data_[lane] = convertF2H(
                        static_cast<float>((static_cast<long long>(i) + 0) % 17 - 8) / 8.0f);
                    input1[b].fp16Data_[lane] = convertF2H(
                        static_cast<float>((3 * static_cast<long long>(i) + 0) % 13 - 6) / 8.0f);
                }
            NumpyBurstType a;
            a.bShape.push_back(dim);
            a.bData = input0;
            NumpyBurstType x;
            x.bShape.push_back(dim);
            x.bData = input1;
            auto kernel = std::make_shared<PIMKernel>(feeder, 1, 1);
            kernel->preloadNoReplacement(&a, 0, 0);
            kernel->preloadNoReplacement(&x, 128, 0);
            size_t consumedReports = 0;
            while (feeder->pendingCount() > 0 || !Phase1Driver::directIdle(*feeder))
            {
                feeder->tick();
                feeder->update();
                reports.tick(feeder->currentClockCycle);
                timeout = feeder->currentClockCycle > watchdog;
                if (timeout) break;
            }
            if (!timeout)
            {
                (*feeder->channels[0]->ranks)[0]->banks[0].injectRawDataBit(0, 0, 15);
                kernel->executeEltwise(static_cast<int>(dim), pimBankType::ALL_BANK,
                                       KernelType::ADD, 0, 256, 128);
                while (!pause.acked() &&
                       (feeder->pendingCount() > 0 || !Phase1Driver::directIdle(*feeder) ||
                        reports.pendingCount() > 0))
                {
                    if (feeder->currentClockCycle > watchdog)
                    {
                        timeout = true;
                        break;
                    }
                    feeder->tick();
                    feeder->update();
                    reports.tick(feeder->currentClockCycle);
                    while (consumedReports < reports.reports().size())
                    {
                        const Phase1PublicReport& report = reports.reports()[consumedReports++];
                        if (reportCycle == 0) reportCycle = report.report_cycle;
                        pause.notifyReport(report.report_cycle);
                        reportCount++;
                    }
                    pause.tick(feeder->currentClockCycle);
                    if (pause.acked()) break;
                }
                if (!pause.requested()) pause.request(feeder->currentClockCycle, "REPORT_TRIGGERED");
                reason = "REPORT_TRIGGERED";
                LoopStats drain = drainToAck(*feeder, pause, watchdog, false, nullptr, 1);
                timeout = timeout || drain.timeout;
                reports.writeCsv(join(outputDir, "reports.csv"));
            }
            dataBefore = dataAccessCount(*feeder, 0);
            refreshBefore = refreshCount(*feeder, 0);
            clockBefore = feeder->currentClockCycle;
            for (unsigned i = 0; i < observation; i++) feeder->update();
            dataAfter = dataAccessCount(*feeder, 0);
            refreshAfter = refreshCount(*feeder, 0);
            clockAfter = feeder->currentClockCycle;
        }
        else if (scenario == "TWO_CHANNEL")
        {
            ASSERT_EQ(2u, channels);
            Phase1Address codec(geometry);
            BurstType payload;
            for (unsigned lane = 0; lane < 16; lane++)
                payload.fp16Data_[lane] = convertF2H(static_cast<float>(lane + 1) / 8.0f);
            for (unsigned ch = 0; ch < 2; ch++)
                for (unsigned row = 0; row < 64; row++)
                {
                    Phase1AddrTuple tuple;
                    tuple.channel = ch;
                    tuple.rank = 0;
                    tuple.bank = 0;
                    tuple.row = row;
                    tuple.burst_column = 0;
                    feeder->addTransaction(true, codec.encode(tuple), &payload);
                }
            for (unsigned i = 0; i < 20; i++)
            {
                feeder->tickChannel(i % 2);
                feeder->update();
            }
            pause.request(feeder->currentClockCycle, "SYNTHETIC_CONTROL");
            LoopStats drain = drainToAck(*feeder, pause, watchdog, true, feeder.get(), 1);
            timeout = drain.timeout;
            preCycles = 20 + drain.cycles;
            dataBefore = dataAccessCount(*feeder, 0);
            otherBefore = dataAccessCount(*feeder, 1);
            refreshBefore = refreshCount(*feeder, 0);
            clockBefore = feeder->currentClockCycle;
            for (unsigned i = 0; i < observation; i++)
            {
                feeder->tickChannel(1);
                feeder->update();
            }
            dataAfter = dataAccessCount(*feeder, 0);
            otherAfter = dataAccessCount(*feeder, 1);
            refreshAfter = refreshCount(*feeder, 0);
            clockAfter = feeder->currentClockCycle;
        }
        else
        {
            FAIL() << "unknown G6 scenario";
        }

        writeMetrics(join(outputDir, "metrics.json"), scenario, channels, feederLimit, handlerDelay,
                     reason, pause, preCycles, feeder->pendingCount(), dataBefore, dataAfter,
                     refreshBefore, refreshAfter, observation, clockBefore, clockAfter, timeout,
                     reportCount, reportCycle, otherBefore, otherAfter);
    }
    catch (const std::exception& error)
    {
        FAIL() << "G6 case failed: " << error.what();
    }
}

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
#include "Phase1Reports.h"
#include "Phase1Runtime.h"
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
        throw std::runtime_error("cannot open G5 params: " + path);
    std::map<std::string, std::string> values;
    std::string line;
    while (std::getline(input, line))
    {
        line = trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#')
            continue;
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos)
            throw std::runtime_error("malformed G5 param line");
        values[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
    return values;
}

std::string get(const std::map<std::string, std::string>& p, const std::string& key)
{
    auto it = p.find(key);
    if (it == p.end())
        throw std::runtime_error("missing G5 param: " + key);
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

std::string stateName(Phase1Runtime& runtime, OutputHandle handle)
{
    return outputStateName(runtime.state(handle));
}

Phase1PublicReport makeReport(const std::string& errorClass, uint64_t cycle)
{
    Phase1PublicReport report;
    report.report_seq = 1;
    report.report_cycle = cycle;
    report.error_class = errorClass;
    report.region_type = "bank";
    report.region_id = 0;
    return report;
}

void configureBanks(MultiChannelMemorySystem& memory, Phase1ReportSink* sink, unsigned numBanks)
{
    for (MemorySystem* channel : memory.channels)
        for (Rank* rank : *channel->ranks)
            for (unsigned bank = 0; bank < numBanks; bank++)
                rank->banks[bank].setPhase1(Phase1Profile::LOCAL_VISIBLE, sink);
}

struct LoopStats
{
    uint64_t cycles = 0;
    bool timeout = false;
};

LoopStats runWithReports(MultiChannelMemorySystem& memory, Phase1Reports& reports,
                         Phase1Runtime& runtime, size_t& consumedReports, uint64_t watchdog,
                         bool stopWhenOutputTerminal)
{
    LoopStats stats;
    while (!Phase1Driver::directIdle(memory) || reports.pendingCount() > 0)
    {
        if (stats.cycles >= watchdog)
        {
            stats.timeout = true;
            break;
        }
        memory.update();
        reports.tick(memory.currentClockCycle);
        while (consumedReports < reports.reports().size())
        {
            const Phase1PublicReport& report = reports.reports()[consumedReports++];
            runtime.onReport(report, report.report_cycle);
        }
        runtime.tick(memory.currentClockCycle);
        stats.cycles++;
        (void)stopWhenOutputTerminal;
    }
    return stats;
}

std::string hex64(uint64_t value)
{
    std::ostringstream os;
    os << std::hex << std::setfill('0') << std::setw(16) << value;
    return os.str();
}

void writeTruth(const std::string& path, const std::string& fault, uint64_t faultCycle,
                const BurstType& source)
{
    std::ofstream out(path);
    out << "record_type,detection_id,fault_id,fault_cycle,storage_key,codeword_index,"
           "bit_positions,raw_before,raw_after,target_access_id,detect_cycle,decoder_status,"
           "eligible_report_id,dropped\n";
    if (fault == "F0")
        return;
    uint64_t before = 0;
    std::memcpy(&before, &source.u8Data_[0], sizeof(before));
    uint64_t after = before;
    std::string bits = "15";
    if (fault == "F1")
        after ^= (uint64_t(1) << 15);
    else
    {
        after ^= (uint64_t(1) << 0) ^ (uint64_t(1) << 1);
        bits = "0|1";
    }
    out << "INJECTION,," << fault << ',' << faultCycle
        << ",\"ch=0,rank=0,bank=0,row=0,burst_col=0,cw=0\",0," << bits << ','
        << hex64(before) << ',' << hex64(after) << ",W1,,,,\n";
}

void writeMetrics(const std::string& path, const std::string& fault, unsigned nTiles,
                  unsigned hold, const std::string& consumer, const std::string& stateAfterProducer,
                  const std::string& stateAfterHold, bool published, bool hostAdmitted,
                  bool pimAdmitted, bool hostBlocked, bool pimBlocked, bool bypassAssert,
                  bool lateReport, uint64_t publishCycle, uint64_t lockCycle,
                  uint64_t producerCycles, const Phase1Reports& reports, bool timeout)
{
    std::ofstream out(path);
    if (!out)
        throw std::runtime_error("cannot open G5 metrics: " + path);
    out << "{\n"
        << "  \"fault_kind\": \"" << fault << "\",\n"
        << "  \"n_tiles\": " << nTiles << ",\n"
        << "  \"hold_cycles\": " << hold << ",\n"
        << "  \"consumer\": \"" << consumer << "\",\n"
        << "  \"state_after_producer\": \"" << stateAfterProducer << "\",\n"
        << "  \"state_after_hold\": \"" << stateAfterHold << "\",\n"
        << "  \"published\": " << (published ? "true" : "false") << ",\n"
        << "  \"host_admitted\": " << (hostAdmitted ? "true" : "false") << ",\n"
        << "  \"pim_admitted\": " << (pimAdmitted ? "true" : "false") << ",\n"
        << "  \"host_blocked\": " << (hostBlocked ? "true" : "false") << ",\n"
        << "  \"pim_blocked\": " << (pimBlocked ? "true" : "false") << ",\n"
        << "  \"bypass_assert\": " << (bypassAssert ? "true" : "false") << ",\n"
        << "  \"late_report\": " << (lateReport ? "true" : "false") << ",\n"
        << "  \"producer_cycles\": " << producerCycles << ",\n"
        << "  \"publish_cycle\": " << publishCycle << ",\n"
        << "  \"lock_cycle\": " << lockCycle << ",\n"
        << "  \"internal_detections\": " << reports.internalDetections() << ",\n"
        << "  \"internal_ce\": " << reports.internalCe() << ",\n"
        << "  \"internal_ue\": " << reports.internalUe() << ",\n"
        << "  \"delivered_reports\": " << reports.delivered() << ",\n"
        << "  \"report_drops\": " << reports.dropped() << ",\n"
        << "  \"timeout\": " << (timeout ? "true" : "false") << "\n"
        << "}\n";
}
}  // namespace

TEST(Phase1Runtime, SameCycleAndStaleHandles)
{
    // Report one cycle before planned publish.
    Phase1Runtime before(5);
    OutputHandle h1 = before.allocateOutput(32, 0);
    before.beginProducing(h1, 0);
    before.producerComplete(h1, 10);
    before.onReport(makeReport("CE", 14), 14);
    before.tick(15);
    EXPECT_EQ(OutputState::SUSPECT, before.state(h1));
    EXPECT_FALSE(before.isPublished(h1));

    // Report in the publish cycle, before runtime tick: report wins.
    Phase1Runtime same(5);
    OutputHandle h2 = same.allocateOutput(32, 0);
    same.beginProducing(h2, 0);
    same.producerComplete(h2, 10);
    same.onReport(makeReport("CE", 15), 15);
    same.tick(15);
    EXPECT_EQ(OutputState::SUSPECT, same.state(h2));
    EXPECT_FALSE(same.isPublished(h2));
    EXPECT_EQ(0u, same.publishCycle());

    // No report: publish at the ready cycle; a later report is late.
    Phase1Runtime late(5);
    OutputHandle h3 = late.allocateOutput(32, 0);
    late.beginProducing(h3, 0);
    late.producerComplete(h3, 10);
    late.tick(15);
    EXPECT_EQ(OutputState::PUBLISHED, late.state(h3));
    late.onReport(makeReport("CE", 16), 16);
    EXPECT_EQ(OutputState::PUBLISHED, late.state(h3));
    EXPECT_EQ(1u, late.lateReports());

    // Stale handle after a new output version.
    Phase1Runtime versioned(0);
    OutputHandle oldHandle = versioned.allocateOutput(32, 0);
    versioned.beginProducing(oldHandle, 0);
    versioned.producerComplete(oldHandle, 0);
    versioned.tick(0);
    ASSERT_EQ(OutputState::PUBLISHED, versioned.state(oldHandle));
    OutputHandle newHandle = versioned.allocateOutput(32, 1);
    EXPECT_TRUE(versioned.isStale(oldHandle));
    EXPECT_FALSE(versioned.isStale(newHandle));
    EXPECT_FALSE(versioned.canConsume(oldHandle, ConsumerType::HOST, 2));
    EXPECT_THROW(versioned.assertConsumerAllowed(oldHandle, ConsumerType::HOST, 2),
                 std::logic_error);

    // Unpublished host/PIM consumers are blocked; bypass guard fails fast.
    Phase1Runtime blocked(5);
    OutputHandle unpublished = blocked.allocateOutput(32, 0);
    blocked.beginProducing(unpublished, 0);
    blocked.producerComplete(unpublished, 1);
    EXPECT_FALSE(blocked.canConsume(unpublished, ConsumerType::HOST, 2));
    EXPECT_FALSE(blocked.canAdmit(unpublished, ConsumerType::PIM, 2));
    EXPECT_THROW(blocked.assertConsumerAllowed(unpublished, ConsumerType::HOST, 2),
                 std::logic_error);
    EXPECT_FALSE(blocked.consume(unpublished, ConsumerType::HOST, 2));
    EXPECT_EQ(1u, blocked.blockedConsumerReads());
}

TEST(Phase1G5Fixture, RunCase)
{
    const char* configPath = std::getenv("PHASE1_CONFIG");
    const char* outputPath = std::getenv("PHASE1_OUT");
    if (configPath == nullptr)
        GTEST_SKIP() << "PHASE1_CONFIG is not set";
    ASSERT_NE(nullptr, outputPath);
    const std::string outputDir(outputPath);

    try
    {
        const auto params = readParams(configPath);
        const std::string fault = get(params, "fault_kind");
        const unsigned nTiles = getU(params, "n_tiles");
        const unsigned hold = getU(params, "hold_cycles");
        const std::string consumer = get(params, "consumer");
        const std::string deviceIni = get(params, "device_ini");
        const std::string systemIni = get(params, "system_ini");
        const uint64_t delay = std::stoull(get(params, "transport_delay_cycles"));
        const uint64_t interval = std::stoull(get(params, "service_interval_cycles"));
        const uint64_t watchdog = std::stoull(get(params, "watchdog_cycles"));

        auto memory = std::make_shared<MultiChannelMemorySystem>(
            deviceIni, systemIni, ".", "phase1_g5", 256);
        memory->setCPUClockSpeed(0);
        Phase1Geometry geometry = Phase1Address::fromConfiguration();
        Phase1Address codec(geometry);
        Phase1Reports reports(codec, geometry.num_channels, geometry.num_banks, "bank", delay,
                              interval, 64, true, true);
        configureBanks(*memory, &reports, geometry.num_banks);
        Phase1Runtime runtime(hold);

        const unsigned dim = nTiles * 16u * 1u * 1u * 8u;
        std::vector<BurstType> input0(dim), input1(dim), result(dim);
        for (unsigned b = 0; b < dim; b++)
            for (unsigned lane = 0; lane < 16; lane++)
            {
                const uint64_t i = static_cast<uint64_t>(b) * 16u + lane;
                input0[b].fp16Data_[lane] =
                    convertF2H(static_cast<float>((static_cast<long long>(i) + 0) % 17 - 8) / 8.0f);
                input1[b].fp16Data_[lane] =
                    convertF2H(static_cast<float>((3 * static_cast<long long>(i) + 0) % 13 - 6) /
                               8.0f);
            }
        NumpyBurstType a;
        a.bShape.push_back(dim);
        a.bData = input0;
        NumpyBurstType x;
        x.bShape.push_back(dim);
        x.bData = input1;
        auto kernel = std::make_shared<PIMKernel>(memory, 1, 1);
        OutputHandle handle = runtime.allocateOutput(static_cast<uint64_t>(dim) * 32u, 0);
        runtime.beginProducing(handle, 0);
        kernel->preloadNoReplacement(&a, 0, 0);
        kernel->preloadNoReplacement(&x, 128, 0);
        size_t preloadReports = 0;
        LoopStats preload = runWithReports(*memory, reports, runtime, preloadReports, watchdog,
                                           false);
        if (preload.timeout)
            throw std::runtime_error("input preload timed out");
        uint64_t faultCycle = 0;
        if (fault != "F0")
        {
            Bank& bank = (*memory->channels[0]->ranks)[0]->banks[0];
            if (fault == "F1")
                bank.injectRawDataBit(0, 0, 15);
            else
            {
                bank.injectRawDataBit(0, 0, 0);
                bank.injectRawDataBit(0, 0, 1);
            }
            faultCycle = memory->currentClockCycle;
        }
        kernel->executeEltwise(static_cast<int>(dim), pimBankType::ALL_BANK, KernelType::ADD, 0,
                               256, 128);
        size_t consumedReports = preloadReports;
        LoopStats producer = runWithReports(*memory, reports, runtime, consumedReports, watchdog,
                                            false);
        if (producer.timeout)
            throw std::runtime_error("producer timed out");
        runtime.producerComplete(handle, memory->currentClockCycle);
        const std::string stateAfterProducer = stateName(runtime, handle);

        // Deliver any already-ready reports before the first publish decision.
        reports.tick(memory->currentClockCycle);
        while (consumedReports < reports.reports().size())
        {
            const Phase1PublicReport& report = reports.reports()[consumedReports++];
            runtime.onReport(report, report.report_cycle);
        }
        runtime.tick(memory->currentClockCycle);
        uint64_t holdCycles = 0;
        const uint64_t holdLimit = hold + 2;
        while (runtime.state(handle) == OutputState::READY_UNPUBLISHED &&
               holdCycles < holdLimit)
        {
            memory->update();
            reports.tick(memory->currentClockCycle);
            while (consumedReports < reports.reports().size())
            {
                const Phase1PublicReport& report = reports.reports()[consumedReports++];
                runtime.onReport(report, report.report_cycle);
            }
            runtime.tick(memory->currentClockCycle);
            holdCycles++;
        }
        const std::string stateAfterHold = stateName(runtime, handle);

        bool hostAdmitted = runtime.canConsume(handle, ConsumerType::HOST,
                                                memory->currentClockCycle);
        bool pimAdmitted = runtime.canAdmit(handle, ConsumerType::PIM,
                                             memory->currentClockCycle);
        bool hostBlocked = !hostAdmitted;
        bool pimBlocked = !pimAdmitted;
        bool bypassAssert = false;
        if (hostBlocked || pimBlocked)
        {
            try
            {
                runtime.assertConsumerAllowed(handle, ConsumerType::HOST,
                                              memory->currentClockCycle);
            }
            catch (const std::logic_error&)
            {
                bypassAssert = true;
            }
        }

        std::ofstream output(join(outputDir, "output.bin"), std::ios::binary);
        if (consumer == "HOST" && hostAdmitted)
        {
            kernel->readData(result.data(), dim, 256, 0);
            LoopStats consumed = runWithReports(*memory, reports, runtime, consumedReports,
                                                watchdog, true);
            if (consumed.timeout)
                throw std::runtime_error("host consumer timed out");
            for (const BurstType& burst : result)
                output.write(reinterpret_cast<const char*>(burst.u8Data_), sizeof(burst.u8Data_));
            runtime.consume(handle, ConsumerType::HOST, memory->currentClockCycle);
        }
        else
        {
            output << "";
        }
        output.close();
        reports.writeCsv(join(outputDir, "reports.csv"));
        Phase1Trace events(join(outputDir, "events.csv"), false);
        writeTruth(join(outputDir, "truth.csv"), fault, faultCycle, input0[0]);
        writeMetrics(join(outputDir, "metrics.json"), fault, nTiles, hold, consumer,
                     stateAfterProducer, stateAfterHold, runtime.isPublished(handle), hostAdmitted,
                     pimAdmitted, hostBlocked, pimBlocked, bypassAssert, runtime.lateReports() > 0,
                     runtime.publishCycle(), runtime.lockCycle(), producer.cycles, reports,
                     producer.timeout);
        (void)codec;
    }
    catch (const std::exception& error)
    {
        FAIL() << "G5 case failed: " << error.what();
    }
}

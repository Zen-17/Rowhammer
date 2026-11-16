#include <cstdlib>
#include <cstring>
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
#include "Phase1Checkpoint.h"
#include "Phase1Driver.h"
#include "Phase1Feeder.h"
#include "Phase1Pause.h"
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
        throw std::runtime_error("cannot open G7 params: " + path);
    std::map<std::string, std::string> values;
    std::string line;
    while (std::getline(input, line))
    {
        line = trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#')
            continue;
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos)
            throw std::runtime_error("malformed G7 param line");
        values[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
    return values;
}

std::string get(const std::map<std::string, std::string>& p, const std::string& key)
{
    auto it = p.find(key);
    if (it == p.end())
        throw std::runtime_error("missing G7 param: " + key);
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

std::string getOr(const std::map<std::string, std::string>& p, const std::string& key,
                  const std::string& fallback)
{
    auto it = p.find(key);
    return it == p.end() ? fallback : it->second;
}

const char* modeName(dramMode mode)
{
    switch (mode)
    {
        case dramMode::SB: return "SB";
        case dramMode::HAB: return "HAB";
        case dramMode::HAB_PIM: return "HAB_PIM";
    }
    return "UNKNOWN";
}

bool zeroBurst(const BurstType& value)
{
    for (unsigned i = 0; i < sizeof(value.u8Data_); i++)
        if (value.u8Data_[i] != 0)
            return false;
    return true;
}

bool rawBurstMatches(Bank& bank, const Phase1AddrTuple& tuple, const BurstType& expected)
{
    for (unsigned cw = 0; cw < 4; cw++)
    {
        Phase1StorageKey key = {tuple.channel, tuple.rank, tuple.bank, tuple.row,
                                tuple.burst_column, cw};
        if (!bank.phase1Storage().contains(key))
            return false;
        uint64_t expectedWord = 0;
        std::memcpy(&expectedWord, &expected.u8Data_[cw * 8], sizeof(expectedWord));
        if (bank.phase1Storage().readRawData(key) != expectedWord)
            return false;
    }
    return true;
}

uint64_t rawMismatchCount(Rank& rank, Phase1Address& codec, uint64_t startAddress,
                          const std::vector<BurstType>& expected)
{
    uint64_t mismatches = 0;
    for (std::size_t i = 0; i < expected.size(); i++)
    {
        const Phase1AddrTuple tuple =
            codec.decode(startAddress + static_cast<uint64_t>(i) * 32u);
        if (tuple.bank >= rank.banks.size() ||
            !rawBurstMatches(rank.banks[tuple.bank], tuple, expected[i]))
            mismatches++;
    }
    return mismatches;
}

bool zeroPimState(PIMRank& pim)
{
    for (const PIMBlock& block : pim.pimBlocks)
    {
        if (!zeroBurst(block.srf) || !zeroBurst(block.mOut) || !zeroBurst(block.aOut))
            return false;
        for (unsigned i = 0; i < 8; i++)
            if (!zeroBurst(block.grfA[i]) || !zeroBurst(block.grfB[i]))
                return false;
    }
    for (unsigned i = 0; i < 32; i++)
        if (pim.crf.data[i] != 0)
            return false;
    return true;
}

BurstType expectedAdd(const BurstType& a, const BurstType& x)
{
    BurstType result;
    for (unsigned lane = 0; lane < 16; lane++)
        result.fp16Data_[lane] = a.fp16Data_[lane] + x.fp16Data_[lane];
    return result;
}

void writeEmptyReportsTruthEvents(const std::string& dir)
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

struct LoopStats
{
    uint64_t cycles = 0;
    bool timeout = false;
};

LoopStats runFeederLoop(Phase1FeederMemory& feeder, Phase1Reports& reports,
                        Phase1Runtime& runtime, Phase1PauseController* pause,
                        size_t& consumedReports, uint64_t watchdog, bool stopOnAck)
{
    LoopStats stats;
    while ((!pause || !pause->acked()) &&
           (feeder.pendingCount() > 0 || !Phase1Driver::directIdle(feeder) ||
            reports.pendingCount() > 0))
    {
        if (stats.cycles >= watchdog)
        {
            stats.timeout = true;
            break;
        }
        feeder.tick();
        feeder.update();
        reports.tick(feeder.currentClockCycle);
        while (consumedReports < reports.reports().size())
        {
            const Phase1PublicReport& report = reports.reports()[consumedReports++];
            runtime.onReport(report, report.report_cycle);
            if (pause != nullptr)
                pause->notifyReport(report.report_cycle);
        }
        if (pause != nullptr)
            pause->tick(feeder.currentClockCycle);
        runtime.tick(feeder.currentClockCycle);
        stats.cycles++;
        if (stopOnAck && pause != nullptr && pause->acked())
            break;
    }
    return stats;
}

LoopStats runDirectWrites(Phase1FeederMemory& feeder, uint64_t watchdog)
{
    LoopStats stats;
    while (!Phase1Driver::directIdle(feeder))
    {
        if (stats.cycles >= watchdog)
        {
            stats.timeout = true;
            break;
        }
        feeder.update();
        stats.cycles++;
    }
    return stats;
}
}  // namespace

TEST(Phase1Checkpoint, NegativeStatuses)
{
    Phase1CheckpointStore store;
    std::vector<BurstType> a(1), x(1);
    a[0].fp16Data_[0] = convertF2H(1.0f);
    x[0].fp16Data_[0] = convertF2H(0.5f);

    EXPECT_EQ(RecoveryStatus::CHECKPOINT_MISSING, store.validate(1));
    ASSERT_TRUE(store.create(1, a, x));
    EXPECT_EQ(RecoveryStatus::VERSION_MISMATCH, store.validate(2));
    store.corruptForTest();
    EXPECT_EQ(RecoveryStatus::CHECKPOINT_CORRUPT, store.validate(1));

    Phase1Runtime runtime(0);
    OutputHandle handle = runtime.allocateOutput(32, 0);
    runtime.beginProducing(handle, 0);
    runtime.producerComplete(handle, 0);
    runtime.tick(0);
    ASSERT_EQ(OutputState::PUBLISHED, runtime.state(handle));
    EXPECT_THROW(runtime.beginRestore(handle, 1), std::logic_error);

    Phase1Runtime versioned(0);
    OutputHandle oldHandle = versioned.allocateOutput(32, 0);
    versioned.beginProducing(oldHandle, 0);
    versioned.producerComplete(oldHandle, 0);
    versioned.tick(0);
    OutputHandle newHandle = versioned.allocateOutput(32, 1);
    EXPECT_TRUE(versioned.isStale(oldHandle));
    try
    {
        versioned.beginRestore(oldHandle, 2);
        FAIL() << "stale restore handle was accepted";
    }
    catch (const std::logic_error& error)
    {
        EXPECT_NE(std::string::npos, std::string(error.what()).find("STALE_HANDLE"));
    }
    (void)newHandle;

}

TEST(Phase1G7NegativeFixture, RunCase)
{
    const char* configPath = std::getenv("PHASE1_CONFIG");
    const char* outputPath = std::getenv("PHASE1_OUT");
    if (configPath == nullptr)
        GTEST_SKIP() << "PHASE1_CONFIG is not set";
    ASSERT_NE(nullptr, outputPath);
    const std::string outputDir(outputPath);
    bool pass = false;
    std::string expected = "OK";
    std::string observed = "NONE";
    std::string detail;

    try
    {
        const auto params = readParams(configPath);
        const std::string testCase = get(params, "case");
        expected = testCase;

        if (testCase == "CHECKPOINT_MISSING")
        {
            Phase1CheckpointStore store;
            const RecoveryStatus status = store.validate(1);
            observed = recoveryStatusName(status);
            pass = (status == RecoveryStatus::CHECKPOINT_MISSING);
        }
        else if (testCase == "VERSION_MISMATCH")
        {
            Phase1CheckpointStore store;
            std::vector<BurstType> a(1), x(1);
            ASSERT_TRUE(store.create(1, a, x));
            const RecoveryStatus status = store.validate(2);
            observed = recoveryStatusName(status);
            pass = (status == RecoveryStatus::VERSION_MISMATCH);
        }
        else if (testCase == "CHECKPOINT_CORRUPT")
        {
            Phase1CheckpointStore store;
            std::vector<BurstType> a(1), x(1);
            ASSERT_TRUE(store.create(1, a, x));
            store.corruptForTest();
            const RecoveryStatus status = store.validate(1);
            observed = recoveryStatusName(status);
            pass = (status == RecoveryStatus::CHECKPOINT_CORRUPT);
        }
        else if (testCase == "ALREADY_COMMITTED")
        {
            Phase1Runtime runtime(0);
            OutputHandle handle = runtime.allocateOutput(32, 0);
            runtime.beginProducing(handle, 0);
            runtime.producerComplete(handle, 0);
            runtime.tick(0);
            ASSERT_EQ(OutputState::PUBLISHED, runtime.state(handle));
            try
            {
                runtime.beginRestore(handle, 1);
                observed = "NO_EXCEPTION";
            }
            catch (const std::logic_error& error)
            {
                observed = error.what();
            }
            pass = observed.find("ALREADY_COMMITTED") != std::string::npos;
        }
        else if (testCase == "STALE_HANDLE")
        {
            Phase1Runtime runtime(0);
            OutputHandle oldHandle = runtime.allocateOutput(32, 0);
            runtime.beginProducing(oldHandle, 0);
            runtime.producerComplete(oldHandle, 0);
            runtime.tick(0);
            ASSERT_EQ(OutputState::PUBLISHED, runtime.state(oldHandle));
            OutputHandle currentHandle = runtime.allocateOutput(32, 1);
            const bool oldReadable = runtime.canConsume(oldHandle, ConsumerType::HOST, 1);
            try
            {
                runtime.assertConsumerAllowed(oldHandle, ConsumerType::HOST, 1);
                observed = "NO_EXCEPTION";
            }
            catch (const std::logic_error& error)
            {
                observed = error.what();
            }
            pass = !oldReadable && observed.find("STALE_HANDLE") != std::string::npos;
            detail = std::string("old_can_consume=") + (oldReadable ? "true" : "false") +
                     " current_version=" + std::to_string(currentHandle.version);
        }
        else
        {
            throw std::runtime_error("unknown G7 negative case: " + testCase);
        }
    }
    catch (const std::exception& error)
    {
        detail = error.what();
    }

    std::ofstream metrics(join(outputDir, "metrics.json"));
    metrics << "{\n"
            << "  \"case\": \"" << expected << "\",\n"
            << "  \"expected_status\": \"" << expected << "\",\n"
            << "  \"observed_status\": \"" << observed << "\",\n"
            << "  \"detail\": \"" << detail << "\",\n"
            << "  \"pass\": " << (pass ? "true" : "false") << "\n"
            << "}\n";
    metrics.close();
    EXPECT_TRUE(pass) << "expected " << expected << ", observed " << observed;
}

TEST(Phase1G7Fixture, RunCase)
{
    const char* configPath = std::getenv("PHASE1_CONFIG");
    const char* outputPath = std::getenv("PHASE1_OUT");
    if (configPath == nullptr)
        GTEST_SKIP() << "PHASE1_CONFIG is not set";
    ASSERT_NE(nullptr, outputPath);
    const std::string outputDir(outputPath);
    writeEmptyReportsTruthEvents(outputDir);

    try
    {
        const auto params = readParams(configPath);
        const unsigned nTiles = getU(params, "n_tiles");
        const unsigned seed = getU(params, "seed");
        const unsigned handlerDelay = getU(params, "handler_delay");
        const std::string trigger = getOr(params, "trigger", "REPORT");
        const std::string negativeControl = getOr(params, "negative_control", "NONE");
        const uint64_t watchdog = std::stoull(get(params, "watchdog_cycles"));
        const std::string deviceIni = get(params, "device_ini");
        const std::string systemIni = get(params, "system_ini");
        const unsigned dim = nTiles * 16u * 1u * 1u * 8u;

        std::vector<BurstType> inputA(dim), inputX(dim), expected(dim), result(dim);
        for (unsigned b = 0; b < dim; b++)
            for (unsigned lane = 0; lane < 16; lane++)
            {
                const uint64_t i = static_cast<uint64_t>(b) * 16u + lane;
                inputA[b].fp16Data_[lane] = convertF2H(
                    static_cast<float>((static_cast<long long>(i) + seed) % 17 - 8) / 8.0f);
                inputX[b].fp16Data_[lane] = convertF2H(
                    static_cast<float>((3 * static_cast<long long>(i) + seed) % 13 - 6) / 8.0f);
            }
        for (unsigned b = 0; b < dim; b++)
            expected[b] = expectedAdd(inputA[b], inputX[b]);

        Phase1CheckpointStore checkpoint;
        ASSERT_TRUE(checkpoint.create(1, inputA, inputX));
        EXPECT_EQ(RecoveryStatus::OK, checkpoint.validate(1));

        auto feeder = std::make_shared<Phase1FeederMemory>(deviceIni, systemIni, ".", "phase1_g7",
                                                           256, 8);
        feeder->setCPUClockSpeed(0);
        Phase1Geometry geometry = Phase1Address::fromConfiguration();
        Phase1Address codec(geometry);
        Phase1Address reportCodec(geometry);
        Phase1Reports reports(reportCodec, geometry.num_channels, geometry.num_banks, "bank", 0, 1,
                              64, true, true);
        for (MemorySystem* channel : feeder->channels)
            for (Rank* rank : *channel->ranks)
                for (Bank& bank : rank->banks)
                    bank.setPhase1(Phase1Profile::LOCAL_VISIBLE, &reports);
        Phase1Runtime runtime(0);
        Phase1PauseController pause(*feeder, 0, handlerDelay);
        OutputHandle oldVersion = runtime.allocateOutput(static_cast<uint64_t>(dim) * 32u, 0);
        runtime.beginProducing(oldVersion, 0);

        NumpyBurstType a;
        a.bShape.push_back(dim);
        a.bData = inputA;
        NumpyBurstType x;
        x.bShape.push_back(dim);
        x.bData = inputX;
        auto kernel = std::make_shared<PIMKernel>(feeder, 1, 1);
        kernel->preloadNoReplacement(&a, 0, 0);
        kernel->preloadNoReplacement(&x, 128, 0);
        size_t consumed = 0;
        LoopStats preload = runFeederLoop(*feeder, reports, runtime, nullptr, consumed, watchdog,
                                          false);
        if (preload.timeout)
            throw std::runtime_error("G7 preload timed out");

        if (trigger != "REPORT" && trigger != "RECOVERY_PROBE")
            throw std::runtime_error("unknown G7 trigger: " + trigger);
        const bool faultInjected = (trigger == "REPORT");
        if (faultInjected)
            (*feeder->channels[0]->ranks)[0]->banks[0].injectRawDataBit(0, 0, 15);
        kernel->executeEltwise(static_cast<int>(dim), pimBankType::ALL_BANK, KernelType::ADD, 0,
                               256, 128);
        if (trigger == "RECOVERY_PROBE")
            pause.request(feeder->currentClockCycle, "RECOVERY_PROBE");
        LoopStats oldRun = runFeederLoop(*feeder, reports, runtime, &pause, consumed, watchdog,
                                         true);
        if (oldRun.timeout || !pause.acked())
            throw std::runtime_error("G7 old kernel did not quiesce");

        const uint64_t oldReportCycle = reports.reports().empty() ? 0 : reports.reports()[0].report_cycle;
        const uint64_t oldReportCount = reports.reports().size();
        const std::string oldStateBeforeQuiesce = stateName(runtime, oldVersion);
        const bool oldPublishedBeforeRestore = runtime.isPublished(oldVersion);
        runtime.quiesceOutput(oldVersion, pause.ackCycle());
        runtime.beginRestore(oldVersion, pause.ackCycle());
        const bool recoveryStateRestoring = (runtime.state(oldVersion) == OutputState::RESTORING);
        const bool consumerBlockedDuringRestore =
            !runtime.canConsume(oldVersion, ConsumerType::HOST, pause.ackCycle()) &&
            !runtime.canConsume(oldVersion, ConsumerType::PIM, pause.ackCycle());
        const std::size_t discarded = feeder->clearPending();
        feeder->unfreezeChannel(0);

        PIMRank* pim = (*feeder->channels[0]->ranks)[0]->pimRank.get();
        pim->pimBlocks[0].grfA[0].set(convertF2H(3.0f));
        pim->pimBlocks[0].grfB[0].set(convertF2H(4.0f));
        pim->pimBlocks[0].srf.set(convertF2H(2.0f));
        pim->pimBlocks[0].mOut.set(convertF2H(5.0f));
        pim->pimBlocks[0].aOut.set(convertF2H(6.0f));
        pim->crf.data[0] = 0xdeadbeefu;
        const bool nonDefaultBeforeReset = !zeroPimState(*pim);
        const std::string modeBeforeReset = modeName((*feeder->channels[0]->ranks)[0]->mode_);
        const bool performReset = (negativeControl != "NO_STATE_RESET");
        if (performReset)
            pim->resetState();
        const bool resetOk = performReset && zeroPimState(*pim) &&
                             (*feeder->channels[0]->ranks)[0]->mode_ == dramMode::SB;
        const std::string modeAfterReset = modeName((*feeder->channels[0]->ranks)[0]->mode_);

        // Restore A/X through simulated host writes, never from reference.bin.
        const Phase1AddrTuple aStart{0, 0, 0, 0, 0};
        const Phase1AddrTuple xStart{0, 0, 0, 128, 0};
        const uint64_t aAddress = codec.encode(aStart);
        const uint64_t xAddress = codec.encode(xStart);
        const bool skipRestore = (negativeControl == "NO_INPUT_RESTORE");
        if (!skipRestore)
        {
            for (unsigned i = 0; i < dim; i++)
                feeder->MultiChannelMemorySystem::addTransaction(
                    true, aAddress + static_cast<uint64_t>(i) * 32u,
                    const_cast<BurstType*>(&checkpoint.inputA()[i]));
            for (unsigned i = 0; i < dim; i++)
                feeder->MultiChannelMemorySystem::addTransaction(
                    true, xAddress + static_cast<uint64_t>(i) * 32u,
                    const_cast<BurstType*>(&checkpoint.inputX()[i]));
            feeder->MultiChannelMemorySystem::addBarrier(0);
        }
        LoopStats restore = runDirectWrites(*feeder, watchdog);
        if (restore.timeout)
        {
            if (negativeControl == "NO_STATE_RESET")
            {
                std::ofstream aborted(join(outputDir, "metrics.json"));
                aborted << "{\n"
                        << "  \"n_tiles\": " << nTiles << ",\n"
                        << "  \"seed\": " << seed << ",\n"
                        << "  \"trigger_source\": \"" << trigger << "\",\n"
                        << "  \"negative_control\": \"" << negativeControl << "\",\n"
                        << "  \"restore_timeout\": true,\n"
                        << "  \"restore_incomplete\": true,\n"
                        << "  \"restore_cycles\": " << restore.cycles << ",\n"
                        << "  \"nondefault_before_reset\": "
                        << (nonDefaultBeforeReset ? "true" : "false") << ",\n"
                        << "  \"mode_before_reset\": \"" << modeBeforeReset << "\",\n"
                        << "  \"mode_after_reset\": \"" << modeAfterReset << "\",\n"
                        << "  \"state_reset_count\": " << (performReset ? 1 : 0) << ",\n"
                        << "  \"old_state_before_quiesce\": \"" << oldStateBeforeQuiesce << "\",\n"
                        << "  \"old_published_before_restore\": "
                        << (oldPublishedBeforeRestore ? "true" : "false") << ",\n"
                        << "  \"recovery_state_restoring\": "
                        << (recoveryStateRestoring ? "true" : "false") << ",\n"
                        << "  \"consumer_blocked_during_restore\": "
                        << (consumerBlockedDuringRestore ? "true" : "false") << ",\n"
                        << "  \"old_quiesce_cycles\": " << pause.quiesceCycles() << ",\n"
                        << "  \"discarded_pending\": " << discarded << ",\n"
                        << "  \"new_published\": false,\n"
                        << "  \"host_read\": false,\n"
                        << "  \"output_bit_exact\": false,\n"
                        << "  \"caught\": true\n"
                        << "}\n";
                return;
            }
            throw std::runtime_error("G7 input restore timed out");
        }
        const uint64_t restoreWrites = skipRestore ? 0u : 2u * dim;

        BurstType restoredA, restoredX, restoreScratch;
        BusPacket aPacket(READ, 0, 0, 0, 0, 0, &restoreScratch, feeder->getLogFile());
        (*feeder->channels[0]->ranks)[0]->banks[0].read(&aPacket, Phase1AccessType::PIM_OPERAND,
                                                        feeder->currentClockCycle);
        restoredA = restoreScratch;
        BusPacket xPacket(READ, 0, 0, 128, 0, 0, &restoreScratch, feeder->getLogFile());
        (*feeder->channels[0]->ranks)[0]->banks[0].read(&xPacket, Phase1AccessType::PIM_OPERAND,
                                                        feeder->currentClockCycle);
        restoredX = restoreScratch;
        const bool restoredAOk =
            std::memcmp(restoredA.u8Data_, inputA[0].u8Data_, sizeof(restoredA.u8Data_)) == 0;
        const bool restoredXOk =
            std::memcmp(restoredX.u8Data_, inputX[0].u8Data_, sizeof(restoredX.u8Data_)) == 0;
        Rank* storageRank = (*feeder->channels[0]->ranks)[0];
        const uint64_t restoredRawAMismatches =
            rawMismatchCount(*storageRank, codec, aAddress, checkpoint.inputA());
        const uint64_t restoredRawXMismatches =
            rawMismatchCount(*storageRank, codec, xAddress, checkpoint.inputX());
        const bool restoredRawAOk = (restoredRawAMismatches == 0);
        const bool restoredRawXOk = (restoredRawXMismatches == 0);

        OutputHandle newVersion = runtime.allocateOutput(static_cast<uint64_t>(dim) * 32u,
                                                         feeder->currentClockCycle);
        runtime.beginReexecute(newVersion, feeder->currentClockCycle);
        auto reexecKernel = std::make_shared<PIMKernel>(feeder, 1, 1);
        reexecKernel->executeEltwise(static_cast<int>(dim), pimBankType::ALL_BANK,
                                     KernelType::ADD, 0, 256, 128);
        const std::size_t reexecReportBase = reports.reports().size();
        size_t reexecReports = reexecReportBase;
        LoopStats reexec = runFeederLoop(*feeder, reports, runtime, nullptr, reexecReports,
                                         watchdog, false);
        if (reexec.timeout)
            throw std::runtime_error("G7 re-execution timed out");
        runtime.producerComplete(newVersion, feeder->currentClockCycle);
        runtime.tick(feeder->currentClockCycle);
        const uint64_t newReportCount = reports.reports().size() - reexecReportBase;
        uint64_t newCECount = 0;
        uint64_t newUECount = 0;
        for (std::size_t i = reexecReportBase; i < reports.reports().size(); i++)
        {
            if (reports.reports()[i].error_class == "CE") newCECount++;
            if (reports.reports()[i].error_class == "UE") newUECount++;
        }
        const uint64_t reexecPending = feeder->pendingCount();
        const uint64_t reexecOutstanding = feeder->outstandingCount();
        BurstType rawB, rawScratch;
        BusPacket bPacket(READ, 0, 0, 256, 0, 0, &rawScratch, feeder->getLogFile());
        (*feeder->channels[0]->ranks)[0]->banks[0].read(&bPacket, Phase1AccessType::PIM_OPERAND,
                                                        feeder->currentClockCycle);
        rawB = rawScratch;
        const bool rawBOk = std::memcmp(rawB.u8Data_, expected[0].u8Data_, sizeof(rawB.u8Data_)) == 0;
        const uint64_t bAddress = codec.encode(Phase1AddrTuple{0, 0, 0, 256, 0});
        const uint64_t rawBMismatches =
            rawMismatchCount(*storageRank, codec, bAddress, expected);
        const bool rawBAllOk = (rawBMismatches == 0);

        bool hostRead = runtime.canConsume(newVersion, ConsumerType::HOST,
                                           feeder->currentClockCycle);
        bool bitExact = false;
        if (hostRead)
        {
            reexecKernel->readData(result.data(), dim, 256, 0);
            size_t readReports = reports.reports().size();
            LoopStats consume = runFeederLoop(*feeder, reports, runtime, nullptr, readReports,
                                              watchdog, false);
            if (consume.timeout)
                throw std::runtime_error("G7 output read timed out");
            bitExact = true;
            for (unsigned b = 0; b < dim; b++)
                if (std::memcmp(result[b].u8Data_, expected[b].u8Data_, sizeof(result[b].u8Data_)) != 0)
                {
                    bitExact = false;
                    break;
                }
            runtime.consume(newVersion, ConsumerType::HOST, feeder->currentClockCycle);
        }

        std::ofstream output(join(outputDir, "output.bin"), std::ios::binary);
        if (hostRead)
            for (const BurstType& burst : result)
                output.write(reinterpret_cast<const char*>(burst.u8Data_), sizeof(burst.u8Data_));
        output.close();
        reports.writeCsv(join(outputDir, "reports.csv"));
        Phase1Trace events(join(outputDir, "events.csv"), false);
        std::ofstream truth(join(outputDir, "truth.csv"));
        truth << "record_type,detection_id,fault_id,fault_cycle,storage_key,codeword_index,"
                 "bit_positions,raw_before,raw_after,target_access_id,detect_cycle,decoder_status,"
                 "eligible_report_id,dropped\n";
        truth.close();

        std::ofstream metrics(join(outputDir, "metrics.json"));
        metrics << "{\n"
                << "  \"n_tiles\": " << nTiles << ",\n"
                << "  \"seed\": " << seed << ",\n"
                << "  \"handler_delay\": " << handlerDelay << ",\n"
                << "  \"trigger_source\": \"" << trigger << "\",\n"
                << "  \"fault_injected\": " << (faultInjected ? "true" : "false") << ",\n"
                << "  \"negative_control\": \"" << negativeControl << "\",\n"
                << "  \"checkpoint_version\": " << checkpoint.version() << ",\n"
                << "  \"checkpoint_bytes\": " << checkpoint.bytes() << ",\n"
                << "  \"checkpoint_checksum\": " << checkpoint.checksum() << ",\n"
                << "  \"restore_writes\": " << restoreWrites << ",\n"
                << "  \"restore_bytes\": " << (restoreWrites * 32u) << ",\n"
                << "  \"discarded_pending\": " << discarded << ",\n"
                << "  \"old_version\": " << oldVersion.version << ",\n"
                << "  \"new_version\": " << newVersion.version << ",\n"
                << "  \"version_changed\": "
                << (oldVersion.version != newVersion.version ? "true" : "false") << ",\n"
                << "  \"old_handle_stale\": "
                << (runtime.isStale(oldVersion) ? "true" : "false") << ",\n"
                << "  \"old_report_count\": " << oldReportCount << ",\n"
                << "  \"old_report_cycle\": " << oldReportCycle << ",\n"
                << "  \"old_state_before_quiesce\": \"" << oldStateBeforeQuiesce << "\",\n"
                << "  \"old_published_before_restore\": "
                << (oldPublishedBeforeRestore ? "true" : "false") << ",\n"
                << "  \"recovery_state_restoring\": "
                << (recoveryStateRestoring ? "true" : "false") << ",\n"
                << "  \"consumer_blocked_during_restore\": "
                << (consumerBlockedDuringRestore ? "true" : "false") << ",\n"
                << "  \"request_cycle\": " << pause.requestCycle() << ",\n"
                << "  \"ack_cycle\": " << pause.ackCycle() << ",\n"
                << "  \"report_to_request_cycles\": "
                << (oldReportCycle == 0 ? 0 : static_cast<long long>(pause.requestCycle()) -
                                                   static_cast<long long>(oldReportCycle)) << ",\n"
                << "  \"old_quiesce_cycles\": " << pause.quiesceCycles() << ",\n"
                << "  \"restore_cycles\": " << restore.cycles << ",\n"
                << "  \"restore_skipped\": " << (skipRestore ? "true" : "false") << ",\n"
                << "  \"restored_a_ok\": " << (restoredAOk ? "true" : "false") << ",\n"
                << "  \"restored_x_ok\": " << (restoredXOk ? "true" : "false") << ",\n"
                << "  \"restored_raw_a_ok\": " << (restoredRawAOk ? "true" : "false") << ",\n"
                << "  \"restored_raw_x_ok\": " << (restoredRawXOk ? "true" : "false") << ",\n"
                << "  \"restored_raw_a_mismatches\": " << restoredRawAMismatches << ",\n"
                << "  \"restored_raw_x_mismatches\": " << restoredRawXMismatches << ",\n"
                << "  \"reexec_cycles\": " << reexec.cycles << ",\n"
                << "  \"reexec_pending\": " << reexecPending << ",\n"
                << "  \"reexec_outstanding\": " << reexecOutstanding << ",\n"
                << "  \"reexec_report_count\": " << newReportCount << ",\n"
                << "  \"reexec_ce_count\": " << newCECount << ",\n"
                << "  \"reexec_ue_count\": " << newUECount << ",\n"
                << "  \"raw_b_ok\": " << (rawBOk ? "true" : "false") << ",\n"
                << "  \"raw_b_all_ok\": " << (rawBAllOk ? "true" : "false") << ",\n"
                << "  \"raw_b_mismatches\": " << rawBMismatches << ",\n"
                << "  \"nondefault_before_reset\": " << (nonDefaultBeforeReset ? "true" : "false")
                << ",\n"
                << "  \"mode_before_reset\": \"" << modeBeforeReset << "\",\n"
                << "  \"mode_after_reset\": \"" << modeAfterReset << "\",\n"
                << "  \"state_reset_count\": " << (performReset ? 1 : 0) << ",\n"
                << "  \"reset_ok\": " << (resetOk ? "true" : "false") << ",\n"
                << "  \"mode_after_reexec\": \""
                << modeName((*feeder->channels[0]->ranks)[0]->mode_) << "\",\n"
                << "  \"new_state\": \"" << stateName(runtime, newVersion) << "\",\n"
                << "  \"new_published\": " << (runtime.isPublished(newVersion) ? "true" : "false")
                << ",\n"
                << "  \"host_read\": " << (hostRead ? "true" : "false") << ",\n"
                << "  \"output_bit_exact\": " << (bitExact ? "true" : "false") << ",\n"
                << "  \"caught\": "
                << ((negativeControl != "NONE" && !bitExact && !runtime.isPublished(newVersion))
                        ? "true" : "false") << ",\n"
                << "  \"publish_cycle\": " << runtime.publishCycle() << ",\n"
                << "  \"recovery_cycles\": "
                << (runtime.isPublished(newVersion) && runtime.publishCycle() >= pause.ackCycle()
                        ? runtime.publishCycle() - pause.ackCycle()
                        : 0) << ",\n"
                << "  \"timeout\": false\n"
                << "}\n";
    }
    catch (const std::exception& error)
    {
        FAIL() << "G7 case failed: " << error.what();
    }
}

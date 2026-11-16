#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "tests/PIMKernel.h"
#include "Phase1Address.h"
#include "Phase1Driver.h"
#include "Phase1Feeder.h"
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
        throw std::runtime_error("cannot open cost params: " + path);
    std::map<std::string, std::string> values;
    std::string line;
    while (std::getline(input, line))
    {
        line = trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#')
            continue;
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos)
            throw std::runtime_error("malformed cost param line");
        values[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
    return values;
}

std::string get(const std::map<std::string, std::string>& p, const std::string& key)
{
    auto it = p.find(key);
    if (it == p.end())
        throw std::runtime_error("missing cost param: " + key);
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

BurstType expectedAdd(const BurstType& a, const BurstType& x)
{
    BurstType result;
    for (unsigned lane = 0; lane < 16; lane++)
        result.fp16Data_[lane] = a.fp16Data_[lane] + x.fp16Data_[lane];
    return result;
}

BurstType expectedDouble(const BurstType& value)
{
    BurstType result;
    for (unsigned lane = 0; lane < 16; lane++)
        result.fp16Data_[lane] = value.fp16Data_[lane] * convertF2H(2.0f);
    return result;
}

struct LoopStats
{
    uint64_t cycles = 0;
    bool timeout = false;
};

void consumeReports(Phase1Reports* reports, Phase1Runtime* runtime, size_t& consumed)
{
    if (reports == nullptr || runtime == nullptr)
        return;
    while (consumed < reports->reports().size())
    {
        const Phase1PublicReport& report = reports->reports()[consumed++];
        runtime->onReport(report, report.report_cycle);
    }
}

LoopStats runFeeder(Phase1FeederMemory& feeder, Phase1Reports* reports, Phase1Runtime* runtime,
                    size_t& consumed, uint64_t watchdog)
{
    LoopStats stats;
    while (feeder.pendingCount() > 0 || !Phase1Driver::directIdle(feeder) ||
           (reports != nullptr && reports->pendingCount() > 0))
    {
        if (stats.cycles >= watchdog)
        {
            stats.timeout = true;
            break;
        }
        feeder.tick();
        feeder.update();
        if (reports != nullptr)
        {
            reports->tick(feeder.currentClockCycle);
            consumeReports(reports, runtime, consumed);
        }
        if (runtime != nullptr)
            runtime->tick(feeder.currentClockCycle);
        stats.cycles++;
    }
    return stats;
}

void writeMetrics(const std::string& path, unsigned nTiles, unsigned seed,
                  const std::string& consumer, const std::string& policy, uint64_t hold,
                  uint64_t tRef, uint64_t preloadCycles, uint64_t producerCycles,
                  uint64_t endToEndCycles, bool published, const std::string& state,
                  bool consumed, bool bitExact, uint64_t parityBytes, uint64_t checkpointBytes,
                  uint64_t version, const Phase1Reports* reports, bool timeout)
{
    const uint64_t outputBytes = static_cast<uint64_t>(nTiles) * 128ULL * 32ULL;
    std::ofstream out(path);
    if (!out)
        throw std::runtime_error("cannot open cost metrics: " + path);
    out << "{\n"
        << "  \"experiment\": \"P1F_COST\",\n"
        << "  \"n_tiles\": " << nTiles << ",\n"
        << "  \"seed\": " << seed << ",\n"
        << "  \"consumer\": \"" << consumer << "\",\n"
        << "  \"policy\": \"" << policy << "\",\n"
        << "  \"hold_cycles\": " << hold << ",\n"
        << "  \"t_ref_cycles\": " << tRef << ",\n"
        << "  \"preload_cycles\": " << preloadCycles << ",\n"
        << "  \"producer_cycles\": " << producerCycles << ",\n"
        << "  \"end_to_end_cycles\": " << endToEndCycles << ",\n"
        << "  \"version\": " << version << ",\n"
        << "  \"published\": " << (published ? "true" : "false") << ",\n"
        << "  \"state\": \"" << state << "\",\n"
        << "  \"consumed\": " << (consumed ? "true" : "false") << ",\n"
        << "  \"output_bit_exact\": " << (consumed && bitExact ? "true" : "false") << ",\n"
        << "  \"host_transfer_bytes\": "
        << (consumer == "HOST" ? 1ULL : 2ULL) * outputBytes << ",\n"
        << "  \"restore_write_bytes\": 0,\n"
        << "  \"peak_held_output_bytes\": " << (policy == "EXT_OFF" ? 0ULL : outputBytes)
        << ",\n"
        << "  \"checkpoint_bytes\": " << checkpointBytes << ",\n"
        << "  \"parity_bytes\": " << parityBytes << ",\n"
        << "  \"runtime_metadata_bytes\": " << (policy == "EXT_OFF" ? 0 : 64) << ",\n"
        << "  \"detected_events\": " << (reports == nullptr ? 0 : reports->internalDetections())
        << ",\n"
        << "  \"delivered_reports\": " << (reports == nullptr ? 0 : reports->delivered())
        << ",\n"
        << "  \"dropped_reports\": " << (reports == nullptr ? 0 : reports->dropped()) << ",\n"
        << "  \"timeout\": " << (timeout ? "true" : "false") << "\n"
        << "}\n";
}
}  // namespace

TEST(Phase1G8CostFixture, RunCase)
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
        const unsigned nTiles = getU(params, "n_tiles");
        const unsigned seed = getU(params, "seed");
        const std::string consumer = get(params, "consumer");
        const std::string policy = get(params, "policy");
        const uint64_t hold = std::stoull(get(params, "hold_cycles"));
        const uint64_t tRef = std::stoull(get(params, "t_ref_cycles"));
        const uint64_t watchdog = std::stoull(get(params, "watchdog_cycles"));
        const std::string deviceIni = get(params, "device_ini");
        const std::string systemIni = get(params, "system_ini");
        const unsigned dim = nTiles * 16u * 1u * 1u * 8u;
        if (consumer != "HOST" && consumer != "PIM")
            throw std::runtime_error("invalid consumer");
        const bool protectedRun = (policy != "EXT_OFF");

        auto feeder = std::make_shared<Phase1FeederMemory>(deviceIni, systemIni, ".",
                                                           "phase1_p1f_cost", 256, 8);
        feeder->setCPUClockSpeed(0);
        Phase1Geometry geometry = Phase1Address::fromConfiguration();
        Phase1Address codec(geometry);
        Phase1Reports reports(codec, geometry.num_channels, geometry.num_banks, "bank", 0, 1,
                              64, true, true);
        if (protectedRun)
            for (MemorySystem* channel : feeder->channels)
                for (Rank* rank : *channel->ranks)
                    for (Bank& bank : rank->banks)
                        bank.setPhase1(Phase1Profile::LOCAL_VISIBLE, &reports);
        Phase1Runtime runtime(hold);

        std::vector<BurstType> inputA(dim), inputX(dim), expectedB(dim), expectedC(dim),
            result(dim);
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
        {
            expectedB[b] = expectedAdd(inputA[b], inputX[b]);
            expectedC[b] = expectedDouble(expectedB[b]);
        }

        NumpyBurstType a;
        a.bShape.push_back(dim);
        a.bData = inputA;
        NumpyBurstType x;
        x.bShape.push_back(dim);
        x.bData = inputX;
        NumpyBurstType k;
        k.bShape.push_back(dim);
        k.bData.resize(dim);
        for (unsigned b = 0; b < dim; b++)
            k.bData[b].set(convertF2H(2.0f));

        auto kernel = std::make_shared<PIMKernel>(feeder, 1, 1);
        OutputHandle handle;
        if (protectedRun)
        {
            handle = runtime.allocateOutput(static_cast<uint64_t>(dim) * 32u, 0);
            runtime.beginProducing(handle, 0);
        }
        kernel->preloadNoReplacement(&a, 0, 0);
        kernel->preloadNoReplacement(&x, 128, 0);
        if (consumer == "PIM")
            kernel->preloadNoReplacement(&k, 384, 0);
        size_t consumedReports = 0;
        LoopStats preload = runFeeder(*feeder, protectedRun ? &reports : nullptr,
                                      protectedRun ? &runtime : nullptr, consumedReports,
                                      watchdog);
        if (preload.timeout)
            throw std::runtime_error("cost preload timed out");

        const uint64_t modeStart = feeder->currentClockCycle;
        kernel->executeEltwise(static_cast<int>(dim), pimBankType::ALL_BANK, KernelType::ADD, 0,
                               256, 128);
        LoopStats producer = runFeeder(*feeder, protectedRun ? &reports : nullptr,
                                       protectedRun ? &runtime : nullptr, consumedReports,
                                       watchdog);
        bool timeout = producer.timeout;
        const uint64_t producerDone = feeder->currentClockCycle;
        bool published = true;
        std::string state = "DIRECT";
        if (protectedRun)
        {
            runtime.producerComplete(handle, producerDone);
            reports.tick(producerDone);
            consumeReports(&reports, &runtime, consumedReports);
            runtime.tick(producerDone);
            uint64_t holdCycles = 0;
            while (runtime.state(handle) == OutputState::READY_UNPUBLISHED &&
                   holdCycles < hold + 2)
            {
                feeder->update();
                reports.tick(feeder->currentClockCycle);
                consumeReports(&reports, &runtime, consumedReports);
                runtime.tick(feeder->currentClockCycle);
                holdCycles++;
            }
            published = runtime.isPublished(handle);
            state = outputStateName(runtime.state(handle));
        }

        bool consumed = false;
        bool bitExact = false;
        if (published)
        {
            if (protectedRun)
                consumed = runtime.consume(handle,
                                           consumer == "HOST" ? ConsumerType::HOST
                                                              : ConsumerType::PIM,
                                           feeder->currentClockCycle);
            else
                consumed = true;
            if (consumed)
            {
                if (consumer == "HOST")
                {
                    kernel->readData(result.data(), dim, 256, 0);
                }
                else
                {
                    kernel->executeEltwise(static_cast<int>(dim), pimBankType::ALL_BANK,
                                           KernelType::MUL, 256, 512, 384);
                }
                LoopStats consume = runFeeder(*feeder, protectedRun ? &reports : nullptr,
                                              protectedRun ? &runtime : nullptr, consumedReports,
                                              watchdog);
                timeout = timeout || consume.timeout;
                if (consumer == "PIM" && !consume.timeout)
                {
                    kernel->readData(result.data(), dim, 512, 0);
                    LoopStats read = runFeeder(*feeder, protectedRun ? &reports : nullptr,
                                               protectedRun ? &runtime : nullptr,
                                               consumedReports, watchdog);
                    timeout = timeout || read.timeout;
                }
                const std::vector<BurstType>& expected =
                    consumer == "HOST" ? expectedB : expectedC;
                bitExact = true;
                for (unsigned b = 0; b < dim; b++)
                    if (std::memcmp(result[b].u8Data_, expected[b].u8Data_,
                                    sizeof(result[b].u8Data_)) != 0)
                    {
                        bitExact = false;
                        break;
                    }
            }
        }
        const uint64_t endToEnd = feeder->currentClockCycle - modeStart;
        const uint64_t producerCycles = producerDone - modeStart;
        std::ofstream output(join(outputDir, "output.bin"), std::ios::binary);
        if (consumed)
            for (const BurstType& burst : result)
                output.write(reinterpret_cast<const char*>(burst.u8Data_),
                             sizeof(burst.u8Data_));
        output.close();
        reports.writeCsv(join(outputDir, "reports.csv"));
        Phase1Trace events(join(outputDir, "events.csv"), false);
        std::ofstream truth(join(outputDir, "truth.csv"));
        truth << "record_type,detection_id,fault_id,fault_cycle,storage_key,codeword_index,"
                 "bit_positions,raw_before,raw_after,target_access_id,detect_cycle,"
                 "decoder_status,eligible_report_id,dropped\n";
        truth.close();
        const uint64_t arrays = (consumer == "PIM") ? 4ULL : 3ULL;
        writeMetrics(join(outputDir, "metrics.json"), nTiles, seed, consumer, policy, hold,
                     tRef, modeStart, producerCycles, endToEnd, published, state, consumed,
                     bitExact, protectedRun ? arrays * static_cast<uint64_t>(dim) * 4ULL : 0,
                     protectedRun ? 2ULL * static_cast<uint64_t>(dim) * 32ULL : 0,
                     protectedRun ? runtime.currentVersion() : 0, protectedRun ? &reports : nullptr,
                     timeout);
    }
    catch (const std::exception& error)
    {
        FAIL() << "P1F cost case failed: " << error.what();
    }
}

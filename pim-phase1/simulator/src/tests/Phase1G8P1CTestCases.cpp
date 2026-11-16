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
        throw std::runtime_error("cannot open P1C params: " + path);
    std::map<std::string, std::string> values;
    std::string line;
    while (std::getline(input, line))
    {
        line = trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#')
            continue;
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos)
            throw std::runtime_error("malformed P1C param line");
        values[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
    return values;
}

std::string get(const std::map<std::string, std::string>& p, const std::string& key)
{
    auto it = p.find(key);
    if (it == p.end())
        throw std::runtime_error("missing P1C param: " + key);
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

std::string stateName(const Phase1Runtime& runtime, OutputHandle handle)
{
    return outputStateName(runtime.state(handle));
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

std::string hex64(uint64_t value)
{
    std::ostringstream os;
    os << std::hex << std::setfill('0') << std::setw(16) << value;
    return os.str();
}

struct LoopStats
{
    uint64_t cycles = 0;
    bool timeout = false;
};

struct ProducerState
{
    uint64_t producer_done_cycle = 0;
    uint64_t lock_cycle = 0;
    uint64_t pause_request_cycle = 0;
    uint64_t quiesce_ack_cycle = 0;
    uint64_t report_count = 0;
    bool pause_scheduled = false;
    bool timeout = false;
};

void consumeReports(Phase1Reports& reports, Phase1Runtime& runtime, size_t& consumed,
                    uint64_t& reportCount)
{
    while (consumed < reports.reports().size())
    {
        const Phase1PublicReport& report = reports.reports()[consumed++];
        runtime.onReport(report, report.report_cycle);
        reportCount++;
    }
}

ProducerState runProducer(MultiChannelMemorySystem& memory, Phase1Reports& reports,
                          Phase1Runtime& runtime, size_t& consumed, uint64_t modeStart,
                          uint64_t handlerDelay, uint64_t watchdog)
{
    ProducerState state;
    uint64_t cycles = 0;
    while (!Phase1Driver::directIdle(memory) ||
           (state.pause_scheduled && state.quiesce_ack_cycle == 0))
    {
        if (cycles >= watchdog)
        {
            state.timeout = true;
            break;
        }
        memory.update();
        reports.tick(memory.currentClockCycle);
        consumeReports(reports, runtime, consumed, state.report_count);
        runtime.tick(memory.currentClockCycle);

        if (state.lock_cycle == 0 && runtime.lockCycle() != 0)
        {
            state.lock_cycle = runtime.lockCycle();
            state.pause_request_cycle = state.lock_cycle + handlerDelay;
            state.pause_scheduled = true;
        }
        if (state.producer_done_cycle == 0 && Phase1Driver::directIdle(memory))
            state.producer_done_cycle = memory.currentClockCycle;
        if (state.pause_scheduled && state.quiesce_ack_cycle == 0 &&
            memory.currentClockCycle >= state.pause_request_cycle &&
            Phase1Driver::directIdle(memory))
        {
            state.quiesce_ack_cycle = memory.currentClockCycle;
            break;
        }
        cycles++;
    }
    if (state.producer_done_cycle == 0)
        state.producer_done_cycle = memory.currentClockCycle;
    (void)modeStart;
    return state;
}

LoopStats runConsumer(MultiChannelMemorySystem& memory, Phase1Reports& reports,
                      Phase1Runtime& runtime, size_t& consumed, uint64_t reportCount,
                      uint64_t watchdog)
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
        consumeReports(reports, runtime, consumed, reportCount);
        runtime.tick(memory.currentClockCycle);
        stats.cycles++;
    }
    return stats;
}

void writeTruth(const std::string& path, const std::string& fault, uint64_t faultCycle,
                const Phase1AddrTuple& tuple, const BurstType& source, unsigned targetBurst)
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
    out << "INJECTION,," << fault << ',' << faultCycle << ",\"ch=" << tuple.channel
        << ",rank=" << tuple.rank << ",bank=" << tuple.bank << ",row=" << tuple.row
        << ",burst_col=" << tuple.burst_column << ",cw=0\",0," << bits << ','
        << hex64(before) << ',' << hex64(after) << ",A_burst_" << targetBurst << ",,,,\n";
}

void writeMetrics(const std::string& path, unsigned nTiles, unsigned seed,
                  const std::string& profile, const std::string& consumer, unsigned targetBurst,
                  const std::string& fault, uint64_t delay, uint64_t hold, uint64_t tRef,
                  uint64_t handlerDelay,
                  const ProducerState& producer, const std::string& stateAfterHold,
                  bool published, bool consumed, bool bitExact, uint64_t changedElements,
                  uint64_t consumeCycle, uint64_t consumerDoneCycle, uint64_t modeStart,
                  uint64_t parityBytes, const Phase1Reports& reports, bool timeout)
{
    const bool outputChanged = consumed && !bitExact;
    const uint64_t producerCycles =
        producer.producer_done_cycle >= modeStart ? producer.producer_done_cycle - modeStart : 0;
    const uint64_t endCycle = consumed ? consumerDoneCycle
                                        : (producer.quiesce_ack_cycle != 0
                                               ? producer.quiesce_ack_cycle
                                               : producer.producer_done_cycle);
    const uint64_t endToEnd = endCycle >= modeStart ? endCycle - modeStart : 0;
    const bool formatUnsupported = reports.formatEvents() > 0;
    const std::string outcome = timeout ? "TIMEOUT"
                                 : formatUnsupported ? "FORMAT_UNSUPPORTED"
                                 : reports.internalUe() > 0 ? "POISONED"
                                 : (published ? (reports.delivered() > 0 ? "LATE_CONTROL"
                                                                         : "NO_REPORT")
                                              : (producer.lock_cycle != 0 ? "TIMELY_CONTROL"
                                                                          : "INVALID_CASE"));
    const std::string recoveryClass =
        (stateAfterHold == "SUSPECT" || stateAfterHold == "POISONED")
            ? "CONTROLLED_NOT_RECOVERED"
            : (formatUnsupported ? "FORMAT_UNSUPPORTED" : "NONE");
    const bool contractPass = !timeout &&
                              (formatUnsupported
                                   ? true
                                   : (published ? (consumed && bitExact) : !consumed)) &&
                              (producer.report_count == 0 ||
                               (producer.lock_cycle != 0 || reports.delivered() > 0));

    std::ofstream out(path);
    if (!out)
        throw std::runtime_error("cannot open P1C metrics: " + path);
    out << "{\n"
        << "  \"experiment\": \"P1C\",\n"
        << "  \"n_tiles\": " << nTiles << ",\n"
        << "  \"seed\": " << seed << ",\n"
        << "  \"profile\": \"" << profile << "\",\n"
        << "  \"consumer\": \"" << consumer << "\",\n"
        << "  \"target_burst\": " << targetBurst << ",\n"
        << "  \"fault_kind\": \"" << fault << "\",\n"
        << "  \"transport_delay_cycles\": " << delay << ",\n"
        << "  \"hold_cycles\": " << hold << ",\n"
        << "  \"t_ref_cycles\": " << tRef << ",\n"
        << "  \"handler_delay_cycles\": " << handlerDelay << ",\n"
        << "  \"contract_pass\": " << (contractPass ? "true" : "false") << ",\n"
        << "  \"outcome_class\": \"" << outcome << "\",\n"
        << "  \"recovery_class\": \"" << recoveryClass << "\",\n"
        << "  \"output_changed\": " << (consumed ? (outputChanged ? "true" : "false") : "null")
        << ",\n"
        << "  \"changed_elements\": " << changedElements << ",\n"
        << "  \"invalid_outputs\": " << ((consumed && !bitExact) ? 1 : 0) << ",\n"
        << "  \"detected_events\": " << reports.internalDetections() << ",\n"
        << "  \"delivered_reports\": " << reports.delivered() << ",\n"
        << "  \"dropped_reports\": " << reports.dropped() << ",\n"
        << "  \"format_events\": " << reports.formatEvents() << ",\n"
        << "  \"report_count_consumed\": " << producer.report_count << ",\n"
        << "  \"report_latency_cycles\": [";
    const std::vector<uint64_t>& latencies = reports.reportLatencies();
    for (std::size_t i = 0; i < latencies.size(); i++)
        out << (i == 0 ? "" : ",") << latencies[i];
    out << "],\n"
        << "  \"first_report_cycle\": "
        << (reports.reports().empty() ? 0 : reports.reports()[0].report_cycle) << ",\n"
        << "  \"lock_cycle\": " << producer.lock_cycle << ",\n"
        << "  \"pause_request_cycle\": " << producer.pause_request_cycle << ",\n"
        << "  \"quiesce_ack_cycle\": " << producer.quiesce_ack_cycle << ",\n"
        << "  \"quiesce_cycles\": "
        << (producer.quiesce_ack_cycle >= producer.pause_request_cycle &&
                    producer.quiesce_ack_cycle != 0
                ? producer.quiesce_ack_cycle - producer.pause_request_cycle
                : 0) << ",\n"
        << "  \"restore_cycles\": null,\n"
        << "  \"reexec_cycles\": null,\n"
        << "  \"producer_cycles\": " << producerCycles << ",\n"
        << "  \"end_to_end_cycles\": " << endToEnd << ",\n"
        << "  \"consume_cycle\": " << consumeCycle << ",\n"
        << "  \"host_transfer_bytes\": "
        << (consumer == "HOST" ? 1ULL : 2ULL) * static_cast<uint64_t>(nTiles) * 128ULL * 32ULL
        << ",\n"
        << "  \"restore_write_bytes\": 0,\n"
        << "  \"peak_held_output_bytes\": " << static_cast<uint64_t>(nTiles) * 128ULL * 32ULL
        << ",\n"
        << "  \"checkpoint_bytes\": 0,\n"
        << "  \"parity_bytes\": " << parityBytes << ",\n"
        << "  \"runtime_metadata_bytes\": 64,\n"
        << "  \"published\": " << (published ? "true" : "false") << ",\n"
        << "  \"state_after_hold\": \"" << stateAfterHold << "\",\n"
        << "  \"consumed\": " << (consumed ? "true" : "false") << ",\n"
        << "  \"host_read\": " << (consumer == "HOST" && consumed ? "true" : "false") << ",\n"
        << "  \"pim_read\": " << (consumer == "PIM" && consumed ? "true" : "false") << ",\n"
        << "  \"output_bit_exact\": " << (consumed && bitExact ? "true" : "false") << ",\n"
        << "  \"timeout\": " << (timeout ? "true" : "false") << "\n"
        << "}\n";
}
}  // namespace

TEST(Phase1G8P1CFixture, RunCase)
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
        const std::string profileName = get(params, "profile");
        const std::string consumer = get(params, "consumer");
        const std::string fault = get(params, "fault_kind");
        const unsigned targetBurst = getU(params, "target_burst");
        const uint64_t delay = std::stoull(get(params, "transport_delay_cycles"));
        const uint64_t hold = std::stoull(get(params, "hold_cycles"));
        const uint64_t tRef = std::stoull(get(params, "t_ref_cycles"));
        const uint64_t handlerDelay = std::stoull(get(params, "handler_delay_cycles"));
        const uint64_t interval = std::stoull(get(params, "service_interval_cycles"));
        const unsigned queueCapacity = getU(params, "report_queue_capacity");
        const uint64_t watchdog = std::stoull(get(params, "watchdog_cycles"));
        const std::string deviceIni = get(params, "device_ini");
        const std::string systemIni = get(params, "system_ini");
        const unsigned dim = nTiles * 16u * 1u * 1u * 8u;
        if (targetBurst >= dim)
            throw std::runtime_error("target_burst outside A range");
        if (consumer != "HOST" && consumer != "PIM")
            throw std::runtime_error("invalid consumer");
        if (fault != "F0" && fault != "F1" && fault != "F2")
            throw std::runtime_error("invalid fault_kind");
        Phase1Profile profile = Phase1Profile::LOCAL_VISIBLE;
        if (profileName == "LOCAL_VISIBLE") profile = Phase1Profile::LOCAL_VISIBLE;
        else if (profileName == "LOCAL_HIDDEN") profile = Phase1Profile::LOCAL_HIDDEN;
        else if (profileName == "HOST_ONLY") profile = Phase1Profile::HOST_ONLY;
        else throw std::runtime_error("invalid profile");
        const bool deliverCe = profile != Phase1Profile::LOCAL_HIDDEN;
        const bool deliverUe = true;

        auto memory = std::make_shared<MultiChannelMemorySystem>(
            deviceIni, systemIni, ".", "phase1_p1c", 256);
        memory->setCPUClockSpeed(0);
        Phase1Geometry geometry = Phase1Address::fromConfiguration();
        Phase1Address codec(geometry);
        Phase1Reports reports(codec, geometry.num_channels, geometry.num_banks, "bank", delay,
                              interval, queueCapacity, deliverCe, deliverUe);
        for (MemorySystem* channel : memory->channels)
            for (Rank* rank : *channel->ranks)
                for (Bank& bank : rank->banks)
                    bank.setPhase1(profile, &reports);
        Phase1Runtime runtime(hold);

        std::vector<BurstType> inputA(dim), inputX(dim), expectedB(dim), expectedC(dim);
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

        auto kernel = std::make_shared<PIMKernel>(memory, 1, 1);
        OutputHandle handle = runtime.allocateOutput(static_cast<uint64_t>(dim) * 32u, 0);
        runtime.beginProducing(handle, 0);
        kernel->preloadNoReplacement(&a, 0, 0);
        kernel->preloadNoReplacement(&x, 128, 0);
        if (consumer == "PIM")
            kernel->preloadNoReplacement(&k, 384, 0);

        size_t consumedReports = 0;
        uint64_t preloadReportCount = 0;
        LoopStats preload = runConsumer(*memory, reports, runtime, consumedReports,
                                        preloadReportCount, watchdog);
        if (preload.timeout)
            throw std::runtime_error("P1C preload timed out");

        const Phase1AddrTuple aStart{0, 0, 0, 0, 0};
        const uint64_t aAddress = codec.encode(aStart);
        const Phase1AddrTuple targetTuple =
            codec.decode(aAddress + static_cast<uint64_t>(targetBurst) * 32u);
        uint64_t faultCycle = 0;
        if (fault != "F0")
        {
            Bank& bank = (*memory->channels[0]->ranks)[0]->banks[targetTuple.bank];
            if (fault == "F1")
                bank.injectRawDataBit(targetTuple.row, targetTuple.burst_column, 15);
            else
            {
                bank.injectRawDataBit(targetTuple.row, targetTuple.burst_column, 0);
                bank.injectRawDataBit(targetTuple.row, targetTuple.burst_column, 1);
            }
            faultCycle = memory->currentClockCycle;
        }

        const uint64_t modeStart = memory->currentClockCycle;
        kernel->executeEltwise(static_cast<int>(dim), pimBankType::ALL_BANK, KernelType::ADD, 0,
                               256, 128);
        ProducerState producer =
            runProducer(*memory, reports, runtime, consumedReports, modeStart, handlerDelay,
                        watchdog);
        bool timeout = producer.timeout;
        uint64_t producerReportCount = producer.report_count;

        if (producer.lock_cycle == 0 && !timeout)
        {
            runtime.producerComplete(handle, producer.producer_done_cycle);
            reports.tick(producer.producer_done_cycle);
            consumeReports(reports, runtime, consumedReports, producerReportCount);
            runtime.tick(producer.producer_done_cycle);
            uint64_t holdCycles = 0;
            while (runtime.state(handle) == OutputState::READY_UNPUBLISHED &&
                   holdCycles < hold + 2)
            {
                memory->update();
                reports.tick(memory->currentClockCycle);
                consumeReports(reports, runtime, consumedReports, producerReportCount);
                if (producer.lock_cycle == 0 && runtime.lockCycle() != 0)
                {
                    producer.lock_cycle = runtime.lockCycle();
                    producer.pause_request_cycle = producer.lock_cycle + handlerDelay;
                    producer.pause_scheduled = true;
                }
                runtime.tick(memory->currentClockCycle);
                holdCycles++;
                if (runtime.state(handle) != OutputState::READY_UNPUBLISHED)
                    break;
            }
            if (runtime.state(handle) == OutputState::SUSPECT &&
                producer.pause_scheduled && producer.quiesce_ack_cycle == 0)
            {
                while (memory->currentClockCycle < producer.pause_request_cycle)
                {
                    memory->update();
                    reports.tick(memory->currentClockCycle);
                    consumeReports(reports, runtime, consumedReports, producerReportCount);
                    runtime.tick(memory->currentClockCycle);
                }
                producer.quiesce_ack_cycle = memory->currentClockCycle;
            }
            producer.report_count = producerReportCount;
        }
        else
        {
            runtime.producerComplete(handle, producer.producer_done_cycle);
            producer.report_count = producerReportCount;
        }

        const std::string stateAfterHold = stateName(runtime, handle);
        const bool published = runtime.isPublished(handle);
        bool consumed = false;
        bool bitExact = false;
        uint64_t changedElements = 0;
        uint64_t consumeCycle = 0;
        uint64_t consumerDoneCycle = memory->currentClockCycle;
        std::vector<BurstType> result(dim);
        if (published)
        {
            consumeCycle = memory->currentClockCycle;
            consumed = runtime.consume(handle,
                                       consumer == "HOST" ? ConsumerType::HOST
                                                          : ConsumerType::PIM,
                                       consumeCycle);
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
                LoopStats consume = runConsumer(*memory, reports, runtime, consumedReports,
                                                producerReportCount, watchdog);
                timeout = timeout || consume.timeout;
                if (consumer == "PIM" && !consume.timeout)
                    kernel->readData(result.data(), dim, 512, 0);
                if (consumer == "PIM" && !consume.timeout)
                {
                    LoopStats read = runConsumer(*memory, reports, runtime, consumedReports,
                                                 producerReportCount, watchdog);
                    timeout = timeout || read.timeout;
                }
                consumerDoneCycle = memory->currentClockCycle;
                const std::vector<BurstType>& expected =
                    consumer == "HOST" ? expectedB : expectedC;
                bitExact = true;
                for (unsigned b = 0; b < dim; b++)
                {
                    if (std::memcmp(result[b].u8Data_, expected[b].u8Data_,
                                    sizeof(result[b].u8Data_)) != 0)
                    {
                        bitExact = false;
                        changedElements++;
                    }
                }
            }
        }

        std::ofstream output(join(outputDir, "output.bin"), std::ios::binary);
        if (consumed)
            for (const BurstType& burst : result)
                output.write(reinterpret_cast<const char*>(burst.u8Data_),
                             sizeof(burst.u8Data_));
        output.close();
        reports.writeCsv(join(outputDir, "reports.csv"));
        Phase1Trace events(join(outputDir, "events.csv"), false);
        writeTruth(join(outputDir, "truth.csv"), fault, faultCycle, targetTuple,
                   inputA[targetBurst], targetBurst);

        const uint64_t arrays = (consumer == "PIM") ? 4ULL : 3ULL;
        writeMetrics(join(outputDir, "metrics.json"), nTiles, seed, profileName, consumer,
                     targetBurst, fault, delay, hold, tRef, handlerDelay, producer,
                     stateAfterHold, published,
                     consumed, bitExact, changedElements, consumeCycle, consumerDoneCycle,
                     modeStart, arrays * static_cast<uint64_t>(dim) * 4ULL, reports, timeout);
    }
    catch (const std::exception& error)
    {
        FAIL() << "P1C case failed: " << error.what();
    }
}

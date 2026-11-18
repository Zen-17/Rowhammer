#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <initializer_list>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "tests/PIMKernel.h"
#include "Phase1Address.h"
#include "Phase1Driver.h"
#include "Phase1Ecc.h"
#include "Phase1Reports.h"
#include "Phase1Runtime.h"
#include "Phase1Storage.h"
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
        throw std::runtime_error("cannot open P1G params: " + path);
    std::map<std::string, std::string> values;
    std::string line;
    while (std::getline(input, line))
    {
        line = trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#')
            continue;
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos)
            throw std::runtime_error("malformed P1G param line");
        values[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
    return values;
}

std::string get(const std::map<std::string, std::string>& p, const std::string& key)
{
    auto it = p.find(key);
    if (it == p.end())
        throw std::runtime_error("missing P1G param: " + key);
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

constexpr uint64_t kEscapeRawMask = (1ULL << 0) | (1ULL << 1) | (1ULL << 4);
constexpr uint64_t kEscapeMask = kEscapeRawMask | (1ULL << 10);
constexpr uint64_t kWitnessMask = 1ULL << 15;
constexpr uint64_t kHeldBRow = 256;

std::string eccStatusName(Phase1EccStatus status)
{
    switch (status)
    {
        case Phase1EccStatus::CLEAN:
            return "CLEAN";
        case Phase1EccStatus::CORRECTED:
            return "CORRECTED";
        case Phase1EccStatus::UNCORRECTABLE:
            return "UNCORRECTABLE";
        case Phase1EccStatus::INVALID:
            return "INVALID";
        case Phase1EccStatus::UNPROTECTED_FORMAT:
            return "UNPROTECTED_FORMAT";
    }
    return "UNKNOWN";
}

std::string bitPositions(std::initializer_list<unsigned> bits)
{
    std::ostringstream os;
    bool first = true;
    for (unsigned bit : bits)
    {
        os << (first ? "" : "|") << bit;
        first = false;
    }
    return os.str();
}

std::string storageKeyString(const Phase1AddrTuple& tuple)
{
    Phase1StorageKey key;
    key.channel = tuple.channel;
    key.rank = tuple.rank;
    key.actual_bank = tuple.bank;
    key.row = tuple.row;
    key.burst_column = tuple.burst_column;
    key.codeword_index = 0;
    return phase1StorageKeyToString(key);
}

void writeTruthHeader(const std::string& path)
{
    std::ofstream out(path, std::ios::out | std::ios::trunc);
    if (!out)
        throw std::runtime_error("cannot open G10 truth.csv: " + path);
    out << "record_type,detection_id,fault_id,fault_cycle,storage_key,codeword_index,"
           "bit_positions,raw_before,raw_after,target_access_id,detect_cycle,decoder_status,"
           "eligible_report_id,dropped\n";
}

void appendInjection(const std::string& path, const std::string& fault, uint64_t faultCycle,
                     const Phase1AddrTuple& tuple, std::initializer_list<unsigned> bits,
                     uint64_t before, uint64_t after, unsigned targetBurst,
                     Phase1EccStatus status)
{
    std::ofstream out(path, std::ios::out | std::ios::app);
    if (!out)
        throw std::runtime_error("cannot append G10 truth.csv: " + path);
    out << "INJECTION,," << fault << ',' << faultCycle << ",\"" << storageKeyString(tuple)
        << "\",0," << bitPositions(bits) << ',' << hex64(before) << ',' << hex64(after)
        << ",A_burst_" << targetBurst << ",," << eccStatusName(status) << ",,\n";
}

BurstType corruptFirstWord(const BurstType& source, uint64_t mask)
{
    BurstType result = source;
    uint64_t word = 0;
    std::memcpy(&word, result.u8Data_, sizeof(word));
    word ^= mask;
    std::memcpy(result.u8Data_, &word, sizeof(word));
    return result;
}

int heldOutputState(MultiChannelMemorySystem& memory, const Phase1Address& codec,
                    unsigned corruptBurst, const BurstType& expected)
{
    const Phase1AddrTuple bStart{0, 0, 0, kHeldBRow, 0};
    const Phase1AddrTuple tuple =
        codec.decode(codec.encode(bStart) + static_cast<uint64_t>(corruptBurst) * 32ULL);
    Bank& bank = (*memory.channels[0]->ranks)[0]->banks[tuple.bank];
    const Phase1Storage& storage = bank.phase1Storage();
    BurstType held;
    for (unsigned cw = 0; cw < 4; cw++)
    {
        const Phase1StorageKey key = {tuple.channel, tuple.rank, tuple.bank, tuple.row,
                                      tuple.burst_column, cw};
        if (!storage.contains(key))
            return -1;
        const uint64_t word = storage.readRawData(key);
        std::memcpy(&held.u8Data_[cw * 8], &word, sizeof(word));
    }
    return std::memcmp(held.u8Data_, expected.u8Data_, sizeof(held.u8Data_)) == 0 ? 0 : 1;
}

const char* jsonBool(bool value)
{
    return value ? "true" : "false";
}

const char* jsonOptionalBool(bool consumed, bool value)
{
    return consumed ? jsonBool(value) : "null";
}

std::string jsonOptionalInt(int value)
{
    return value < 0 ? "null" : std::to_string(value);
}

const char* jsonOptionalBoolState(int value)
{
    return value < 0 ? "null" : jsonBool(value != 0);
}

struct EscapeMetrics
{
    unsigned n_tiles = 0;
    unsigned seed = 0;
    unsigned target_burst = 0;
    int witness_burst = -1;
    int corrupt_burst = -1;
    int witness_bank = -1;
    int corrupt_bank = -1;
    std::string profile;
    std::string consumer;
    std::string fault_kind;
    std::string escape_placement;
    uint64_t transport_delay = 0;
    uint64_t hold = 0;
    uint64_t t_ref = 0;
    uint64_t handler_delay = 0;
    ProducerState producer;
    std::string state_after_hold;
    bool published = false;
    bool consumed = false;
    bool bit_exact = false;
    bool timeout = false;
    bool corruption_propagates = false;
    bool report_covers_corrupt_bank = false;
    bool contract_pass = false;
    int held_output_corrupt = -1;
    uint64_t changed_elements = 0;
    uint64_t consume_cycle = 0;
    uint64_t consumer_done_cycle = 0;
    uint64_t mode_start = 0;
    uint64_t parity_bytes = 0;
    std::vector<uint64_t> report_region_ids;
    std::string escape_outcome;
};

void writeMetrics(const std::string& path, const EscapeMetrics& m, const Phase1Reports& reports)
{
    const bool outputChanged = m.consumed && !m.bit_exact;
    const uint64_t producerCycles =
        m.producer.producer_done_cycle >= m.mode_start ? m.producer.producer_done_cycle - m.mode_start
                                                       : 0;
    const uint64_t endCycle = m.consumed ? m.consumer_done_cycle
                                         : (m.producer.quiesce_ack_cycle != 0
                                                ? m.producer.quiesce_ack_cycle
                                                : m.producer.producer_done_cycle);
    const uint64_t endToEnd = endCycle >= m.mode_start ? endCycle - m.mode_start : 0;
    const bool formatUnsupported = reports.formatEvents() > 0;
    const std::string outcome = m.timeout ? "TIMEOUT"
                                 : formatUnsupported ? "FORMAT_UNSUPPORTED"
                                 : reports.internalUe() > 0 ? "POISONED"
                                 : (m.published ? (reports.delivered() > 0 ? "LATE_CONTROL"
                                                                          : "NO_REPORT")
                                                : (m.producer.lock_cycle != 0 ? "TIMELY_CONTROL"
                                                                              : "INVALID_CASE"));
    const std::string recovery =
        (m.state_after_hold == "SUSPECT" || m.state_after_hold == "POISONED")
            ? "CONTROLLED_NOT_RECOVERED"
            : (formatUnsupported ? "FORMAT_UNSUPPORTED" : "NONE");

    std::ofstream out(path);
    if (!out)
        throw std::runtime_error("cannot open G10 metrics: " + path);
    out << "{\n"
        << "  \"experiment\": \"P1G\",\n"
        << "  \"n_tiles\": " << m.n_tiles << ",\n"
        << "  \"seed\": " << m.seed << ",\n"
        << "  \"profile\": \"" << m.profile << "\",\n"
        << "  \"consumer\": \"" << m.consumer << "\",\n"
        << "  \"target_burst\": " << m.target_burst << ",\n"
        << "  \"fault_kind\": \"" << m.fault_kind << "\",\n"
        << "  \"escape_placement\": \"" << m.escape_placement << "\",\n"
        << "  \"witness_burst\": " << jsonOptionalInt(m.witness_burst) << ",\n"
        << "  \"corrupt_burst\": " << m.corrupt_burst << ",\n"
        << "  \"witness_bank\": " << jsonOptionalInt(m.witness_bank) << ",\n"
        << "  \"corrupt_bank\": " << m.corrupt_bank << ",\n"
        << "  \"transport_delay_cycles\": " << m.transport_delay << ",\n"
        << "  \"hold_cycles\": " << m.hold << ",\n"
        << "  \"t_ref_cycles\": " << m.t_ref << ",\n"
        << "  \"handler_delay_cycles\": " << m.handler_delay << ",\n"
        << "  \"contract_pass\": " << jsonBool(m.contract_pass) << ",\n"
        << "  \"outcome_class\": \"" << outcome << "\",\n"
        << "  \"escape_outcome\": \"" << m.escape_outcome << "\",\n"
        << "  \"recovery_class\": \"" << recovery << "\",\n"
        << "  \"output_changed\": " << jsonOptionalBool(m.consumed, outputChanged) << ",\n"
        << "  \"changed_elements\": " << m.changed_elements << ",\n"
        << "  \"invalid_outputs\": " << ((m.consumed && !m.bit_exact) ? 1 : 0) << ",\n"
        << "  \"detected_events\": " << reports.internalDetections() << ",\n"
        << "  \"delivered_reports\": " << reports.delivered() << ",\n"
        << "  \"internal_ce\": " << reports.internalCe() << ",\n"
        << "  \"internal_ue\": " << reports.internalUe() << ",\n"
        << "  \"dropped_reports\": " << reports.dropped() << ",\n"
        << "  \"format_events\": " << reports.formatEvents() << ",\n"
        << "  \"report_count_consumed\": " << m.producer.report_count << ",\n"
        << "  \"report_latency_cycles\": [";
    const std::vector<uint64_t>& latencies = reports.reportLatencies();
    for (std::size_t i = 0; i < latencies.size(); i++)
        out << (i == 0 ? "" : ",") << latencies[i];
    out << "],\n"
        << "  \"first_report_cycle\": "
        << (reports.reports().empty() ? 0 : reports.reports()[0].report_cycle) << ",\n"
        << "  \"report_region_ids\": [";
    for (std::size_t i = 0; i < m.report_region_ids.size(); i++)
        out << (i == 0 ? "" : ",") << m.report_region_ids[i];
    out << "],\n"
        << "  \"report_covers_corrupt_bank\": " << jsonBool(m.report_covers_corrupt_bank) << ",\n"
        << "  \"corruption_propagates\": " << jsonBool(m.corruption_propagates) << ",\n"
        << "  \"held_output_corrupt\": " << jsonOptionalBoolState(m.held_output_corrupt) << ",\n"
        << "  \"lock_cycle\": " << m.producer.lock_cycle << ",\n"
        << "  \"pause_request_cycle\": " << m.producer.pause_request_cycle << ",\n"
        << "  \"quiesce_ack_cycle\": " << m.producer.quiesce_ack_cycle << ",\n"
        << "  \"quiesce_cycles\": "
        << (m.producer.quiesce_ack_cycle >= m.producer.pause_request_cycle &&
                    m.producer.quiesce_ack_cycle != 0
                ? m.producer.quiesce_ack_cycle - m.producer.pause_request_cycle
                : 0) << ",\n"
        << "  \"restore_cycles\": null,\n"
        << "  \"reexec_cycles\": null,\n"
        << "  \"producer_cycles\": " << producerCycles << ",\n"
        << "  \"end_to_end_cycles\": " << endToEnd << ",\n"
        << "  \"consume_cycle\": " << m.consume_cycle << ",\n"
        << "  \"host_transfer_bytes\": "
        << (m.consumer == "HOST" ? 1ULL : 2ULL) * static_cast<uint64_t>(m.n_tiles) * 128ULL * 32ULL
        << ",\n"
        << "  \"restore_write_bytes\": 0,\n"
        << "  \"peak_held_output_bytes\": " << static_cast<uint64_t>(m.n_tiles) * 128ULL * 32ULL
        << ",\n"
        << "  \"checkpoint_bytes\": 0,\n"
        << "  \"parity_bytes\": " << m.parity_bytes << ",\n"
        << "  \"runtime_metadata_bytes\": 64,\n"
        << "  \"published\": " << jsonBool(m.published) << ",\n"
        << "  \"state_after_hold\": \"" << m.state_after_hold << "\",\n"
        << "  \"consumed\": " << jsonBool(m.consumed) << ",\n"
        << "  \"host_read\": " << jsonBool(m.consumer == "HOST" && m.consumed) << ",\n"
        << "  \"pim_read\": " << jsonBool(m.consumer == "PIM" && m.consumed) << ",\n"
        << "  \"output_bit_exact\": " << jsonBool(m.consumed && m.bit_exact) << ",\n"
        << "  \"timeout\": " << jsonBool(m.timeout) << "\n"
        << "}\n";
}
}  // namespace

TEST(Phase1G10EscapeFixture, RunCase)
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
        const std::string placement = get(params, "escape_placement");
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
        if (fault != "F3M" && fault != "F4S" && fault != "F1W_F4S")
            throw std::runtime_error("invalid fault_kind");
        if (fault == "F3M" || fault == "F4S")
        {
            if (placement != "NONE")
                throw std::runtime_error("F3M/F4S require escape_placement=NONE");
        }
        else if (placement != "SAME_BANK" && placement != "DIFF_BANK")
            throw std::runtime_error("F1W_F4S requires SAME_BANK or DIFF_BANK");
        if (profileName != "LOCAL_VISIBLE")
            throw std::runtime_error("G10 smoke requires LOCAL_VISIBLE");

        auto memory = std::make_shared<MultiChannelMemorySystem>(
            deviceIni, systemIni, ".", "phase1_g10_escape", 256);
        memory->setCPUClockSpeed(0);
        Phase1Geometry geometry = Phase1Address::fromConfiguration();
        Phase1Address codec(geometry);
        Phase1Reports reports(codec, geometry.num_channels, geometry.num_banks, "bank", delay,
                              interval, queueCapacity, true, true);
        class RecordingSink : public Phase1ReportSink
        {
          public:
            explicit RecordingSink(Phase1Reports& reports) : reports_(reports) {}
            void onDetected(const Phase1Detection& detection) override
            {
                detections_.push_back(detection);
                reports_.onDetected(detection);
            }
            void onUnprotectedFormat(const Phase1FormatEvent& event) override
            {
                reports_.onUnprotectedFormat(event);
            }
            const std::vector<Phase1Detection>& detections() const { return detections_; }

          private:
            Phase1Reports& reports_;
            std::vector<Phase1Detection> detections_;
        } recorder(reports);
        for (MemorySystem* channel : memory->channels)
            for (Rank* rank : *channel->ranks)
                for (Bank& bank : rank->banks)
                    bank.setPhase1(Phase1Profile::LOCAL_VISIBLE, &recorder);
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
            throw std::runtime_error("G10 preload timed out");

        const Phase1AddrTuple aStart{0, 0, 0, 0, 0};
        const uint64_t aAddress = codec.encode(aStart);
        const Phase1AddrTuple targetTuple =
            codec.decode(aAddress + static_cast<uint64_t>(targetBurst) * 32u);

        unsigned witnessBurst = targetBurst;
        unsigned corruptBurst = targetBurst;
        Phase1AddrTuple witnessTuple = targetTuple;
        Phase1AddrTuple corruptTuple = targetTuple;
        const bool hasWitness = fault != "F4S";
        if (fault == "F1W_F4S")
        {
            bool found = false;
            for (unsigned step = 1; step <= dim; step++)
            {
                const unsigned b = (targetBurst + step) % dim;
                const Phase1AddrTuple tuple =
                    codec.decode(aAddress + static_cast<uint64_t>(b) * 32u);
                const bool sameBank = tuple.bank == witnessTuple.bank;
                if ((placement == "SAME_BANK" && sameBank) ||
                    (placement == "DIFF_BANK" && !sameBank))
                {
                    corruptBurst = b;
                    corruptTuple = tuple;
                    found = true;
                    break;
                }
            }
            if (!found)
                throw std::runtime_error("no corrupt_burst satisfies escape_placement");
            if (corruptBurst == witnessBurst)
                throw std::runtime_error("corrupt_burst equals witness_burst");
        }
        const std::string truthPath = join(outputDir, "truth.csv");
        writeTruthHeader(truthPath);
        const uint64_t faultCycle = memory->currentClockCycle;
        bool evaluatorOk = true;
        auto bankFor = [&](const Phase1AddrTuple& tuple) -> Bank& {
            return (*memory->channels[0]->ranks)[0]->banks[tuple.bank];
        };
        auto injectBurst = [&](unsigned burst, const Phase1AddrTuple& tuple,
                               std::initializer_list<unsigned> bits,
                               uint64_t mask) -> Phase1EccStatus {
            Bank& bank = bankFor(tuple);
            const Phase1StorageKey key = {tuple.channel, tuple.rank, tuple.bank, tuple.row,
                                          tuple.burst_column, 0};
            const uint64_t before = bank.phase1Storage().readRawData(key);
            const uint8_t parity = bank.phase1Storage().readRawParity(key);
            for (unsigned bit : bits)
                bank.injectRawDataBit(tuple.row, tuple.burst_column, bit);
            const uint64_t after = bank.phase1Storage().readRawData(key);
            const Phase1EccResult decoded = Phase1Ecc::decode(after, parity);
            appendInjection(truthPath, fault, faultCycle, tuple, bits, before, after, burst,
                            decoded.status);
            if (after != (before ^ mask))
                evaluatorOk = false;
            return decoded.status;
        };

        Phase1EccStatus witnessStatus = Phase1EccStatus::CLEAN;
        Phase1EccStatus corruptStatus = Phase1EccStatus::CLEAN;
        if (fault == "F3M")
        {
            witnessStatus = injectBurst(witnessBurst, witnessTuple, {0, 1, 4}, kEscapeRawMask);
            corruptStatus = witnessStatus;
        }
        else if (fault == "F4S")
        {
            corruptStatus =
                injectBurst(corruptBurst, corruptTuple, {0, 1, 4, 10}, kEscapeMask);
        }
        else
        {
            witnessStatus = injectBurst(witnessBurst, witnessTuple, {15}, kWitnessMask);
            corruptStatus =
                injectBurst(corruptBurst, corruptTuple, {0, 1, 4, 10}, kEscapeMask);
        }

        auto firstWord = [](const BurstType& burst) {
            uint64_t word = 0;
            std::memcpy(&word, burst.u8Data_, sizeof(word));
            return word;
        };
        const uint64_t corruptOriginal = firstWord(inputA[corruptBurst]);
        const uint64_t corruptRawMask = (fault == "F3M") ? kEscapeRawMask : kEscapeMask;
        const uint64_t corruptRaw = corruptOriginal ^ corruptRawMask;
        const Phase1EccResult corruptDecoded =
            Phase1Ecc::decode(corruptRaw, Phase1Ecc::encodeParity(corruptOriginal));
        if (fault == "F3M")
            evaluatorOk = evaluatorOk && corruptDecoded.status == Phase1EccStatus::CORRECTED &&
                          corruptDecoded.data == (corruptOriginal ^ kEscapeMask);
        else
            evaluatorOk = evaluatorOk && corruptDecoded.status == Phase1EccStatus::CLEAN &&
                          corruptDecoded.data == corruptRaw;
        if (fault == "F1W_F4S")
        {
            const uint64_t witnessOriginal = firstWord(inputA[witnessBurst]);
            const Phase1EccResult witnessDecoded = Phase1Ecc::decode(
                witnessOriginal ^ kWitnessMask, Phase1Ecc::encodeParity(witnessOriginal));
            evaluatorOk = evaluatorOk &&
                          witnessStatus == Phase1EccStatus::CORRECTED &&
                          witnessDecoded.status == Phase1EccStatus::CORRECTED &&
                          witnessDecoded.data == witnessOriginal;
        }

        const BurstType badA = corruptFirstWord(inputA[corruptBurst], kEscapeMask);
        const BurstType badB = expectedAdd(badA, inputX[corruptBurst]);
        bool corruptionPropagates = false;
        if (consumer == "HOST")
            corruptionPropagates =
                std::memcmp(badB.u8Data_, expectedB[corruptBurst].u8Data_, sizeof(badB.u8Data_)) !=
                0;
        else
        {
            const BurstType badC = expectedDouble(badB);
            corruptionPropagates =
                std::memcmp(badC.u8Data_, expectedC[corruptBurst].u8Data_, sizeof(badC.u8Data_)) !=
                0;
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
        const int heldOutputCorrupt =
            published ? -1 : heldOutputState(*memory, codec, corruptBurst, expectedB[corruptBurst]);
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

        const bool outputChanged = consumed && !bitExact;
        std::vector<uint64_t> reportRegionIds;
        for (const Phase1PublicReport& report : reports.reports())
            reportRegionIds.push_back(report.region_id);
        const uint64_t corruptBankRegion =
            static_cast<uint64_t>(corruptTuple.channel) * geometry.num_banks + corruptTuple.bank;
        bool reportCoversCorruptBank = false;
        for (const Phase1PublicReport& report : reports.reports())
            if (report.region_type == "bank" && report.region_id == corruptBankRegion)
                reportCoversCorruptBank = true;
        bool corruptDetected = false;
        for (const Phase1Detection& detection : recorder.detections())
            if (detection.channel == corruptTuple.channel && detection.rank == corruptTuple.rank &&
                detection.actual_bank == corruptTuple.bank && detection.row == corruptTuple.row &&
                detection.burst_column == corruptTuple.burst_column)
                corruptDetected = true;

        std::string escapeOutcome;
        if (timeout)
            escapeOutcome = "TIMEOUT";
        else if (!evaluatorOk)
            escapeOutcome = "INVALID_CASE";
        else if (!corruptionPropagates)
            escapeOutcome = "MASKED";
        else if (consumed && outputChanged)
            escapeOutcome = "CORRUPT_CONSUMED";
        else if (!published && producer.lock_cycle != 0 && heldOutputCorrupt == 1)
            escapeOutcome = "PREVENTED";
        else
            escapeOutcome = "UNCLASSIFIED";

        bool contract = !timeout && evaluatorOk &&
                        (escapeOutcome == "PREVENTED" || escapeOutcome == "CORRUPT_CONSUMED" ||
                         escapeOutcome == "MASKED") &&
                        reports.internalUe() == 0;
        if (fault == "F4S")
            contract = contract && reports.delivered() == 0 && reports.internalDetections() == 0;
        if (fault == "F3M")
            contract = contract && reports.internalDetections() >= 1 &&
                       corruptStatus == Phase1EccStatus::CORRECTED;
        if (fault == "F1W_F4S")
            contract = contract && reports.internalDetections() >= 1 && !corruptDetected &&
                       witnessStatus == Phase1EccStatus::CORRECTED;

        std::ofstream output(join(outputDir, "output.bin"), std::ios::binary);
        if (consumed)
            for (const BurstType& burst : result)
                output.write(reinterpret_cast<const char*>(burst.u8Data_),
                             sizeof(burst.u8Data_));
        output.close();
        reports.writeCsv(join(outputDir, "reports.csv"));
        Phase1Trace events(join(outputDir, "events.csv"), false);

        EscapeMetrics metrics;
        metrics.n_tiles = nTiles;
        metrics.seed = seed;
        metrics.target_burst = targetBurst;
        metrics.witness_burst = hasWitness ? static_cast<int>(witnessBurst) : -1;
        metrics.corrupt_burst = static_cast<int>(corruptBurst);
        metrics.witness_bank = hasWitness ? static_cast<int>(witnessTuple.bank) : -1;
        metrics.corrupt_bank = static_cast<int>(corruptTuple.bank);
        metrics.profile = profileName;
        metrics.consumer = consumer;
        metrics.fault_kind = fault;
        metrics.escape_placement = placement;
        metrics.transport_delay = delay;
        metrics.hold = hold;
        metrics.t_ref = tRef;
        metrics.handler_delay = handlerDelay;
        metrics.producer = producer;
        metrics.state_after_hold = stateAfterHold;
        metrics.published = published;
        metrics.consumed = consumed;
        metrics.bit_exact = bitExact;
        metrics.timeout = timeout;
        metrics.corruption_propagates = corruptionPropagates;
        metrics.report_covers_corrupt_bank = reportCoversCorruptBank;
        metrics.contract_pass = contract;
        metrics.held_output_corrupt = heldOutputCorrupt;
        metrics.changed_elements = changedElements;
        metrics.consume_cycle = consumeCycle;
        metrics.consumer_done_cycle = consumerDoneCycle;
        metrics.mode_start = modeStart;
        metrics.parity_bytes =
            ((consumer == "PIM") ? 4ULL : 3ULL) * static_cast<uint64_t>(dim) * 4ULL;
        metrics.report_region_ids = reportRegionIds;
        metrics.escape_outcome = escapeOutcome;
        writeMetrics(join(outputDir, "metrics.json"), metrics, reports);
    }
    catch (const std::exception& error)
    {
        FAIL() << "G10 escape case failed: " << error.what();
    }
}

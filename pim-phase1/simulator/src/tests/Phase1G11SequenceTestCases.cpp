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
#include "Phase1Address.h"
#include "Phase1Driver.h"
#include "Phase1Ecc.h"
#include "Phase1Reports.h"
#include "Phase1Storage.h"

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
        throw std::runtime_error("cannot open P1H params: " + path);
    std::map<std::string, std::string> values;
    std::string line;
    while (std::getline(input, line))
    {
        line = trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#')
            continue;
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos)
            throw std::runtime_error("malformed P1H param line");
        values[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
    return values;
}

std::string get(const std::map<std::string, std::string>& p, const std::string& key)
{
    auto it = p.find(key);
    if (it == p.end())
        throw std::runtime_error("missing P1H param: " + key);
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

std::string hex64(uint64_t value)
{
    std::ostringstream os;
    os << std::hex << std::setfill('0') << std::setw(16) << value;
    return os.str();
}

std::string statusName(Phase1EccStatus status)
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

BurstType patternBurst(unsigned seed)
{
    BurstType burst;
    const uint64_t base = 0x0123456789ABCDEFULL ^
                          (static_cast<uint64_t>(seed) * 0x9E3779B97F4A7C15ULL);
    for (unsigned cw = 0; cw < 4; cw++)
        std::memcpy(&burst.u8Data_[cw * 8], &base, sizeof(base));
    return burst;
}

void configureBanks(MultiChannelMemorySystem& memory, Phase1Profile profile,
                    Phase1ReportSink* sink, unsigned numBanks)
{
    for (MemorySystem* channel : memory.channels)
    {
        for (Rank* rank : *channel->ranks)
        {
            for (unsigned bank = 0; bank < numBanks; bank++)
                rank->banks[bank].setPhase1(profile, sink);
        }
    }
}

struct RunOutcome
{
    bool timeout = false;
    uint64_t cycles = 0;
};

bool memoryIdle(MultiChannelMemorySystem& memory)
{
    return Phase1Driver::directIdle(memory);
}

RunOutcome runMemory(MultiChannelMemorySystem& memory, Phase1Reports& reports,
                     uint64_t watchdog)
{
    RunOutcome outcome;
    while (!memoryIdle(memory) || reports.pendingCount() > 0)
    {
        if (outcome.cycles >= watchdog)
        {
            outcome.timeout = true;
            break;
        }
        memory.update();
        reports.tick(memory.currentClockCycle);
        outcome.cycles++;
    }
    return outcome;
}

void hostWrite(MultiChannelMemorySystem& memory, uint64_t address, const BurstType& payload,
               Phase1Reports& reports, uint64_t watchdog)
{
    if (!memory.addTransaction(true, address, const_cast<BurstType*>(&payload)))
        throw std::runtime_error("host write was not accepted");
    if (!memory.addBarrier(0))
        throw std::runtime_error("host write barrier was not accepted");
    RunOutcome outcome = runMemory(memory, reports, watchdog);
    if (outcome.timeout)
        throw std::runtime_error("host write timed out");
}

BurstType hostRead(MultiChannelMemorySystem& memory, uint64_t address, Phase1Reports& reports,
                   uint64_t watchdog)
{
    BurstType result;
    if (!memory.addTransaction(false, address, &result))
        throw std::runtime_error("host read was not accepted");
    if (!memory.addBarrier(0))
        throw std::runtime_error("host read barrier was not accepted");
    RunOutcome outcome = runMemory(memory, reports, watchdog);
    if (outcome.timeout)
        throw std::runtime_error("host read timed out");
    return result;
}

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
};

struct InjectionRow
{
    std::string fault_id;
    uint64_t fault_cycle = 0;
    Phase1AddrTuple tuple;
    unsigned codeword = 0;
    unsigned bit = 0;
    uint64_t before = 0;
    uint64_t after = 0;
    std::string access_id;
    Phase1EccStatus status = Phase1EccStatus::CLEAN;
};

std::string storageKeyString(const Phase1AddrTuple& tuple, unsigned codeword)
{
    Phase1StorageKey key;
    key.channel = tuple.channel;
    key.rank = tuple.rank;
    key.actual_bank = tuple.bank;
    key.row = tuple.row;
    key.burst_column = tuple.burst_column;
    key.codeword_index = codeword;
    return phase1StorageKeyToString(key);
}

std::string locationOf(const Phase1AddrTuple& tuple, const Phase1AddrTuple& p,
                       const Phase1AddrTuple& q, const Phase1AddrTuple& r)
{
    if (tuple == p)
        return "P";
    if (tuple == q)
        return "Q";
    if (tuple == r)
        return "R";
    return "?";
}

void writeTruth(const std::string& path, const std::vector<InjectionRow>& injections,
                const std::vector<Phase1Detection>& detections, const Phase1AddrTuple& p,
                const Phase1AddrTuple& q, const Phase1AddrTuple& r)
{
    std::ofstream out(path, std::ios::out | std::ios::trunc);
    if (!out)
        throw std::runtime_error("cannot open P1H truth.csv: " + path);
    out << "record_type,detection_id,fault_id,fault_cycle,storage_key,codeword_index,"
           "bit_positions,raw_before,raw_after,target_access_id,detect_cycle,decoder_status,"
           "eligible_report_id,dropped\n";
    for (const InjectionRow& row : injections)
    {
        out << "INJECTION,," << row.fault_id << ',' << row.fault_cycle << ",\""
            << storageKeyString(row.tuple, row.codeword) << "\"," << row.codeword << ","
            << row.bit << ',' << hex64(row.before) << ',' << hex64(row.after) << ','
            << row.access_id << ",," << statusName(row.status) << ",,\n";
    }
    unsigned detectionId = 0;
    for (const Phase1Detection& detection : detections)
    {
        Phase1AddrTuple tuple;
        tuple.channel = detection.channel;
        tuple.rank = detection.rank;
        tuple.bank = detection.actual_bank;
        tuple.row = detection.row;
        tuple.burst_column = detection.burst_column;
        out << "DETECTION," << ++detectionId << ",,,"
            << "\"" << storageKeyString(tuple, detection.codeword_index) << "\","
            << detection.codeword_index << ",,,,"
            << locationOf(tuple, p, q, r) << ',' << detection.detect_cycle << ','
            << statusName(detection.status) << ",,\n";
    }
}

void writeMetrics(const std::string& path, const std::string& scenario,
                  const std::string& granularity, unsigned scrub, unsigned seed,
                  unsigned bankP, unsigned bankR, unsigned rounds, unsigned injections,
                  uint64_t detectedEvents, uint64_t deliveredReports, uint64_t internalCe,
                  uint64_t internalUe, uint64_t scrubWrites, const std::string& signature,
                  bool contractPass, bool timeout)
{
    std::ofstream out(path);
    if (!out)
        throw std::runtime_error("cannot open P1H metrics: " + path);
    out << "{\n"
        << "  \"experiment\": \"P1H\",\n"
        << "  \"scenario\": \"" << scenario << "\",\n"
        << "  \"granularity\": \"" << granularity << "\",\n"
        << "  \"scrub\": " << scrub << ",\n"
        << "  \"seed\": " << seed << ",\n"
        << "  \"bank_p\": " << bankP << ",\n"
        << "  \"bank_r\": " << bankR << ",\n"
        << "  \"rounds\": " << rounds << ",\n"
        << "  \"injections\": " << injections << ",\n"
        << "  \"detected_events\": " << detectedEvents << ",\n"
        << "  \"delivered_reports\": " << deliveredReports << ",\n"
        << "  \"internal_ce\": " << internalCe << ",\n"
        << "  \"internal_ue\": " << internalUe << ",\n"
        << "  \"scrub_writes\": " << scrubWrites << ",\n"
        << "  \"report_signature\": \"" << signature << "\",\n"
        << "  \"contract_pass\": " << (contractPass ? "true" : "false") << ",\n"
        << "  \"timeout\": " << (timeout ? "true" : "false") << "\n"
        << "}\n";
}
}  // namespace

TEST(Phase1G11SequenceFixture, RunCase)
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
        const std::string scenario = get(params, "scenario");
        const std::string granularity = get(params, "granularity");
        const unsigned scrub = getU(params, "scrub");
        const unsigned seed = getU(params, "seed");
        const unsigned bankP = getU(params, "bank_p");
        const unsigned bankR = getU(params, "bank_r");
        const unsigned rounds = getU(params, "rounds");
        const uint64_t delay = std::stoull(get(params, "transport_delay_cycles"));
        const uint64_t interval = std::stoull(get(params, "service_interval_cycles"));
        const unsigned capacity = getU(params, "report_queue_capacity");
        const uint64_t watchdog = std::stoull(get(params, "watchdog_cycles"));
        const std::string deviceIni = get(params, "device_ini");
        const std::string systemIni = get(params, "system_ini");
        const std::string profileName = get(params, "profile");

        if (profileName != "LOCAL_VISIBLE")
            throw std::runtime_error("P1H requires LOCAL_VISIBLE");
        if (granularity != "bank" && granularity != "page" && granularity != "codeword")
            throw std::runtime_error("invalid granularity");
        if (scrub != 0 && scrub != 1)
            throw std::runtime_error("scrub must be 0 or 1");
        if (bankP >= 16 || bankR >= 16 || bankP == bankR)
            throw std::runtime_error("invalid bank_p/bank_r");
        if (rounds != 4)
            throw std::runtime_error("rounds must be 4");
        const bool knownScenario = scenario == "S1_SINGLE" || scenario == "S2_DIFF_CW" ||
                                   scenario == "S3_DIFF_BURST_SAME_BANK" ||
                                   scenario == "S4_SAME_CW_TWO_BITS" ||
                                   scenario == "S5_DIFF_BANK" || scenario == "S6_SAME_BIT_RECUR";
        if (!knownScenario)
            throw std::runtime_error("invalid scenario");
        if (scenario == "S6_SAME_BIT_RECUR" && scrub != 1)
            throw std::runtime_error("S6 requires scrub=1");

        auto memory = std::make_shared<MultiChannelMemorySystem>(
            deviceIni, systemIni, ".", "phase1_g11_sequence", 256);
        memory->setCPUClockSpeed(0);
        const Phase1Geometry geometry = Phase1Address::fromConfiguration();
        Phase1Address codec(geometry);
        Phase1Reports reports(codec, geometry.num_channels, geometry.num_banks, granularity,
                              delay, interval, capacity, true, true);
        RecordingSink sink(reports);
        configureBanks(*memory, Phase1Profile::LOCAL_VISIBLE, &sink, geometry.num_banks);
        if (scrub != 0)
        {
            for (MemorySystem* channel : memory->channels)
                for (Rank* rank : *channel->ranks)
                    for (Bank& bank : rank->banks)
                        bank.setPhase1Scrub(true);
        }

        const Phase1AddrTuple p{0, 0, bankP, 0, 0};
        const Phase1AddrTuple q{0, 0, bankP, 1, 0};
        const Phase1AddrTuple r{0, 0, bankR, 0, 0};
        const uint64_t pAddress = codec.encode(p);
        const uint64_t qAddress = codec.encode(q);
        const uint64_t rAddress = codec.encode(r);
        if (codec.decode(pAddress) != p || codec.decode(qAddress) != q ||
            codec.decode(rAddress) != r)
            throw std::runtime_error("P/Q/R address round-trip failed");

        const BurstType pattern = patternBurst(seed);
        hostWrite(*memory, pAddress, pattern, reports, watchdog);
        hostWrite(*memory, qAddress, pattern, reports, watchdog);
        hostWrite(*memory, rAddress, pattern, reports, watchdog);

        auto bankAt = [&](const Phase1AddrTuple& tuple) -> Bank& {
            return (*memory->channels[0]->ranks)[0]->banks[tuple.bank];
        };
        std::vector<InjectionRow> injections;
        bool invalidCase = false;

        auto inject = [&](const std::string& location, const Phase1AddrTuple& tuple,
                          unsigned bit, unsigned round) {
            Bank& bank = bankAt(tuple);
            const unsigned codeword = bit / 64;
            const Phase1StorageKey key = {tuple.channel, tuple.rank, tuple.bank, tuple.row,
                                          tuple.burst_column, codeword};
            const uint64_t before = bank.phase1Storage().readRawData(key);
            const uint8_t parity = bank.phase1Storage().readRawParity(key);
            bank.injectRawDataBit(tuple.row, tuple.burst_column, bit);
            const uint64_t after = bank.phase1Storage().readRawData(key);
            const Phase1EccResult decoded = Phase1Ecc::decode(after, parity);
            InjectionRow row;
            row.fault_id = scenario;
            row.fault_cycle = memory->currentClockCycle;
            row.tuple = tuple;
            row.codeword = codeword;
            row.bit = bit;
            row.before = before;
            row.after = after;
            row.access_id = location + "_round" + std::to_string(round);
            row.status = decoded.status;
            injections.push_back(row);
        };

        const uint64_t initialFirstWord = [&] {
            uint64_t word = 0;
            std::memcpy(&word, pattern.u8Data_, sizeof(word));
            return word;
        }();

        for (unsigned round = 0; round < rounds; round++)
        {
            if (round == 0)
                inject("P", p, 15, round);
            if (round == 1)
            {
                if (scenario == "S2_DIFF_CW")
                    inject("P", p, 79, round);
                else if (scenario == "S3_DIFF_BURST_SAME_BANK")
                    inject("Q", q, 15, round);
                else if (scenario == "S4_SAME_CW_TWO_BITS")
                    inject("P", p, 14, round);
                else if (scenario == "S5_DIFF_BANK")
                    inject("R", r, 15, round);
                else if (scenario == "S6_SAME_BIT_RECUR")
                {
                    const Phase1StorageKey key = {p.channel, p.rank, p.bank, p.row,
                                                  p.burst_column, 0};
                    const uint64_t raw = bankAt(p).phase1Storage().readRawData(key);
                    const unsigned expectedBit =
                        static_cast<unsigned>((initialFirstWord >> 15) & 1ULL);
                    if (((raw >> 15) & 1ULL) != expectedBit)
                        invalidCase = true;
                    inject("P", p, 15, round);
                }
            }

            // Fixed P, Q, R order in every round; every read drains to idle
            // and empties the report queue before the next access.
            (void)hostRead(*memory, pAddress, reports, watchdog);
            (void)hostRead(*memory, qAddress, reports, watchdog);
            (void)hostRead(*memory, rAddress, reports, watchdog);
        }

        uint64_t scrubWrites = 0;
        for (MemorySystem* channel : memory->channels)
            for (Rank* rank : *channel->ranks)
                for (Bank& bank : rank->banks)
                    scrubWrites += bank.phase1ScrubWrites();

        std::ostringstream signature;
        for (std::size_t i = 0; i < reports.reports().size(); i++)
        {
            if (i != 0)
                signature << ';';
            signature << reports.reports()[i].error_class << ':'
                      << reports.reports()[i].region_id;
        }

        const unsigned expectedInjections = scenario == "S1_SINGLE" ? 1u : 2u;
        const bool contract = !invalidCase && injections.size() == expectedInjections &&
                              reports.delivered() == reports.internalDetections() &&
                              reports.internalDetections() == sink.detections().size() &&
                              reports.dropped() == 0 && reports.pendingCount() == 0;

        writeTruth(join(outputDir, "truth.csv"), injections, sink.detections(), p, q, r);
        reports.writeCsv(join(outputDir, "reports.csv"));
        writeMetrics(join(outputDir, "metrics.json"), scenario, granularity, scrub, seed, bankP,
                      bankR, rounds, static_cast<unsigned>(injections.size()),
                      reports.internalDetections(), reports.delivered(), reports.internalCe(),
                      reports.internalUe(), scrubWrites, signature.str(), contract, false);
    }
    catch (const std::exception& error)
    {
        FAIL() << "P1H case failed: " << error.what();
    }
}

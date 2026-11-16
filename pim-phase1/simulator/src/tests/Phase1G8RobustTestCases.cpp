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
        throw std::runtime_error("cannot open robustness params: " + path);
    std::map<std::string, std::string> values;
    std::string line;
    while (std::getline(input, line))
    {
        line = trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#')
            continue;
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos)
            throw std::runtime_error("malformed robustness param line");
        values[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
    return values;
}

std::string get(const std::map<std::string, std::string>& p, const std::string& key)
{
    auto it = p.find(key);
    if (it == p.end())
        throw std::runtime_error("missing robustness param: " + key);
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

class DroppingSink : public Phase1ReportSink
{
  public:
    DroppingSink(Phase1Reports& target, unsigned dropEvery)
        : target_(target), dropEvery_(dropEvery)
    {
    }

    void onDetected(const Phase1Detection& detection) override
    {
        detectionSeq_++;
        if (dropEvery_ != 0 && detectionSeq_ % dropEvery_ == 0)
        {
            droppedSeqs_.push_back(detectionSeq_);
            return;
        }
        target_.onDetected(detection);
    }

    void onUnprotectedFormat(const Phase1FormatEvent& event) override
    {
        target_.onUnprotectedFormat(event);
    }

    uint64_t detections() const { return detectionSeq_; }
    const std::vector<uint64_t>& droppedSeqs() const { return droppedSeqs_; }

  private:
    Phase1Reports& target_;
    unsigned dropEvery_ = 0;
    uint64_t detectionSeq_ = 0;
    std::vector<uint64_t> droppedSeqs_;
};

struct LoopStats
{
    uint64_t cycles = 0;
    bool timeout = false;
};

LoopStats runMemory(MultiChannelMemorySystem& memory, Phase1Reports& reports,
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
        stats.cycles++;
    }
    return stats;
}

void writeTruth(const std::string& path, uint64_t faultCycle, const BurstType& original)
{
    std::ofstream out(path);
    out << "record_type,detection_id,fault_id,fault_cycle,storage_key,codeword_index,"
           "bit_positions,raw_before,raw_after,target_access_id,detect_cycle,decoder_status,"
           "eligible_report_id,dropped\n";
    uint64_t before = 0;
    std::memcpy(&before, &original.u8Data_[0], sizeof(before));
    uint64_t after = before ^ (uint64_t(1) << 15);
    out << "INJECTION,,F1," << faultCycle
        << ",\"ch=0,rank=0,bank=0,row=0,burst_col=0,cw=0\",0,15," << hex64(before) << ','
        << hex64(after) << ",HREAD_BURST,,,,\n";
}

void writeMetrics(const std::string& path, unsigned queueCapacity, uint64_t interval,
                  const std::string& dropRate, unsigned repeatReads, uint64_t delay,
                  const DroppingSink& sink, const Phase1Reports& reports,
                  const LoopStats& outcome)
{
    const auto& latencies = reports.reportLatencies();
    const auto& publicReports = reports.reports();
    std::ofstream out(path);
    if (!out)
        throw std::runtime_error("cannot open robustness metrics: " + path);
    uint64_t minGap = 0;
    bool gapsOk = true;
    for (std::size_t i = 1; i < publicReports.size(); i++)
    {
        const uint64_t gap = publicReports[i].report_cycle - publicReports[i - 1].report_cycle;
        if (i == 1 || gap < minGap)
            minGap = gap;
        if (gap < interval)
            gapsOk = false;
    }
    const bool accounting = (reports.internalDetections() + sink.droppedSeqs().size() ==
                             sink.detections()) &&
                            (reports.delivered() + reports.dropped() ==
                             reports.internalDetections());
    out << "{\n"
        << "  \"experiment\": \"P1F_ROBUST\",\n"
        << "  \"queue_capacity\": " << queueCapacity << ",\n"
        << "  \"service_interval_cycles\": " << interval << ",\n"
        << "  \"drop_rate\": \"" << dropRate << "\",\n"
        << "  \"repeat_reads\": " << repeatReads << ",\n"
        << "  \"transport_delay_cycles\": " << delay << ",\n"
        << "  \"cycles\": " << outcome.cycles << ",\n"
        << "  \"timeout\": " << (outcome.timeout ? "true" : "false") << ",\n"
        << "  \"total_detections\": " << sink.detections() << ",\n"
        << "  \"forwarded_detections\": " << reports.internalDetections() << ",\n"
        << "  \"injected_drops\": " << sink.droppedSeqs().size() << ",\n"
        << "  \"delivered_reports\": " << reports.delivered() << ",\n"
        << "  \"queue_drops\": " << reports.dropped() << ",\n"
        << "  \"overflow\": " << (reports.overflow() ? "true" : "false") << ",\n"
        << "  \"max_queue_depth\": " << reports.maxQueueDepth() << ",\n"
        << "  \"accounting_ok\": " << (accounting ? "true" : "false") << ",\n"
        << "  \"service_interval_ok\": " << (gapsOk ? "true" : "false") << ",\n"
        << "  \"min_report_gap\": " << minGap << ",\n"
        << "  \"contract_pass\": "
        << ((!outcome.timeout && accounting && gapsOk &&
             (latencies.empty() || latencies[0] >= std::max<uint64_t>(1, delay)))
                ? "true"
                : "false")
        << ",\n"
        << "  \"outcome_class\": \"ROBUSTNESS\",\n"
        << "  \"report_latency_cycles\": [";
    for (std::size_t i = 0; i < latencies.size(); i++)
        out << (i == 0 ? "" : ",") << latencies[i];
    out << "],\n"
        << "  \"report_cycles\": [";
    for (std::size_t i = 0; i < publicReports.size(); i++)
        out << (i == 0 ? "" : ",") << publicReports[i].report_cycle;
    out << "],\n"
        << "  \"dropped_detection_seqs\": [";
    for (std::size_t i = 0; i < sink.droppedSeqs().size(); i++)
        out << (i == 0 ? "" : ",") << sink.droppedSeqs()[i];
    out << "]\n}\n";
}
}  // namespace

TEST(Phase1G8RobustFixture, RunCase)
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
        const unsigned queueCapacity = getU(params, "queue_capacity");
        const uint64_t interval = std::stoull(get(params, "service_interval_cycles"));
        const std::string dropRate = get(params, "drop_rate");
        const unsigned repeatReads = getU(params, "repeat_reads");
        const uint64_t delay = std::stoull(get(params, "transport_delay_cycles"));
        const uint64_t watchdog = std::stoull(get(params, "watchdog_cycles"));
        const std::string deviceIni = get(params, "device_ini");
        const std::string systemIni = get(params, "system_ini");
        unsigned dropEvery = 0;
        if (dropRate == "0.1")
            dropEvery = 10;
        else if (dropRate != "0")
            throw std::runtime_error("drop_rate must be 0 or 0.1");

        auto memory = std::make_shared<MultiChannelMemorySystem>(
            deviceIni, systemIni, ".", "phase1_p1f_robust", 256);
        memory->setCPUClockSpeed(0);
        Phase1Geometry geometry = Phase1Address::fromConfiguration();
        Phase1Address codec(geometry);
        Phase1Reports reports(codec, geometry.num_channels, geometry.num_banks, "bank", delay,
                              interval, queueCapacity, true, true);
        DroppingSink sink(reports, dropEvery);
        for (MemorySystem* channel : memory->channels)
            for (Rank* rank : *channel->ranks)
                for (Bank& bank : rank->banks)
                    bank.setPhase1(Phase1Profile::LOCAL_VISIBLE, &sink);

        const Phase1AddrTuple tuple{0, 0, 0, 0, 0};
        const uint64_t address = codec.encode(tuple);
        BurstType original;
        for (unsigned lane = 0; lane < 16; lane++)
            original.fp16Data_[lane] = convertF2H(static_cast<float>(lane + 1) / 8.0f);
        if (!memory->addTransaction(true, address, &original))
            throw std::runtime_error("host write was not accepted");
        if (!memory->addBarrier(0))
            throw std::runtime_error("host write barrier was not accepted");
        LoopStats write = runMemory(*memory, reports, watchdog);
        if (write.timeout)
            throw std::runtime_error("host write timed out");

        Bank& bank = (*memory->channels[0]->ranks)[0]->banks[0];
        bank.injectRawDataBit(0, 0, 15);
        const uint64_t faultCycle = memory->currentClockCycle;

        std::vector<BurstType> received(repeatReads);
        for (unsigned i = 0; i < repeatReads; i++)
            if (!memory->addTransaction(false, address, &received[i]))
                throw std::runtime_error("burst read was not accepted");
        if (!memory->addBarrier(0))
            throw std::runtime_error("burst read barrier was not accepted");
        LoopStats outcome = runMemory(*memory, reports, watchdog);

        reports.writeCsv(join(outputDir, "reports.csv"));
        Phase1Trace events(join(outputDir, "events.csv"), false);
        writeTruth(join(outputDir, "truth.csv"), faultCycle, original);
        std::ofstream output(join(outputDir, "output.bin"), std::ios::binary);
        output.close();
        writeMetrics(join(outputDir, "metrics.json"), queueCapacity, interval, dropRate,
                     repeatReads, delay, sink, reports, outcome);
    }
    catch (const std::exception& error)
    {
        FAIL() << "P1F robustness case failed: " << error.what();
    }
}

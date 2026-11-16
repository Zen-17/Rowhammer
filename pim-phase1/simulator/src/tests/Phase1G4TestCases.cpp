#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "tests/PIMKernel.h"
#include "Phase1Address.h"
#include "Phase1Driver.h"
#include "Phase1Reports.h"
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
        throw std::runtime_error("cannot open G4 params: " + path);
    std::map<std::string, std::string> values;
    std::string line;
    while (std::getline(input, line))
    {
        line = trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#')
            continue;
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos)
            throw std::runtime_error("malformed G4 param line");
        values[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
    return values;
}

std::string get(const std::map<std::string, std::string>& p, const std::string& key)
{
    auto it = p.find(key);
    if (it == p.end())
        throw std::runtime_error("missing G4 param: " + key);
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

std::string jsonEscape(const std::string& s)
{
    std::string out;
    for (char c : s)
    {
        if (c == '"' || c == '\\')
            out += '\\';
        out += c;
    }
    return out;
}

std::string packetStatusName(Phase1PacketStatus status)
{
    switch (status)
    {
        case Phase1PacketStatus::OK: return "OK";
        case Phase1PacketStatus::CORRECTED: return "CORRECTED";
        case Phase1PacketStatus::UNPROTECTED_FORMAT: return "UNPROTECTED_FORMAT";
        case Phase1PacketStatus::UNCORRECTABLE: return "UNCORRECTABLE";
    }
    return "UNKNOWN";
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

unsigned bankParity(unsigned bank)
{
    return bank & 1u;
}

unsigned bankPimBlock(unsigned bank)
{
    return bank / 2u;
}

BurstType pimRead(MultiChannelMemorySystem& memory, unsigned targetBank, Phase1PacketStatus& status,
                  Phase1Reports& reports, uint64_t watchdog)
{
    Rank* rank = (*memory.channels[0]->ranks)[0];
    BurstType scratch;
    BurstType result;
    BusPacket packet(READ, 0, 0, 0, 0, bankParity(targetBank), &scratch, memory.getLogFile());
    rank->pimRank->readOpd(bankPimBlock(targetBank), result,
                           bankParity(targetBank) ? PIMOpdType::ODD_BANK : PIMOpdType::EVEN_BANK,
                           &packet, 0, false, false);
    status = packet.phase1_status;
    RunOutcome outcome = runMemory(memory, reports, watchdog);
    if (outcome.timeout)
        throw std::runtime_error("PIM read timed out");
    return result;
}

void pimWrite(MultiChannelMemorySystem& memory, unsigned targetBank, const BurstType& payload,
              Phase1Reports& reports, uint64_t watchdog)
{
    Rank* rank = (*memory.channels[0]->ranks)[0];
    BurstType scratch;
    BusPacket packet(WRITE, 0, 0, 0, 0, bankParity(targetBank), &scratch, memory.getLogFile());
    rank->pimRank->writeOpd(bankPimBlock(targetBank), const_cast<BurstType&>(payload),
                            bankParity(targetBank) ? PIMOpdType::ODD_BANK : PIMOpdType::EVEN_BANK,
                            &packet, 0, false, false);
    RunOutcome outcome = runMemory(memory, reports, watchdog);
    if (outcome.timeout)
        throw std::runtime_error("PIM write timed out");
}

void inject(Bank& bank, const std::string& fault, unsigned targetLane, unsigned dataBitInLane,
            unsigned faultBit2)
{
    if (fault == "F1")
    {
        const unsigned bit = targetLane * 16u + dataBitInLane;
        bank.injectRawDataBit(0, 0, bit);
    }
    else if (fault == "F2")
    {
        const unsigned codeword = targetLane / 4u;
        bank.injectRawDataBit(0, 0, codeword * 64u);
        bank.injectRawDataBit(0, 0, codeword * 64u + faultBit2);
    }
}

void writeTruth(const std::string& path, const std::string& fault, unsigned targetBank,
                unsigned targetLane, unsigned dataBitInLane, unsigned faultBit2,
                uint64_t faultCycle, const std::string& accessPath, const BurstType& original)
{
    std::ofstream out(path);
    if (!out)
        throw std::runtime_error("cannot open truth.csv: " + path);
    out << "record_type,detection_id,fault_id,fault_cycle,storage_key,codeword_index,"
           "bit_positions,raw_before,raw_after,target_access_id,detect_cycle,decoder_status,"
           "eligible_report_id,dropped\n";
    if (fault == "F0")
        return;
    unsigned bit = targetLane * 16u + dataBitInLane;
    unsigned codeword = bit / 64u;
    unsigned bitInWord = bit % 64u;
    std::string bits = std::to_string(bitInWord);
    if (fault == "F2")
    {
        codeword = targetLane / 4u;
        bitInWord = 0;
        bits = "0|" + std::to_string(faultBit2);
    }
    uint64_t before = 0;
    std::memcpy(&before, &original.u8Data_[codeword * 8], sizeof(before));
    uint64_t after = before;
    if (fault == "F1")
        after ^= (uint64_t(1) << bitInWord);
    else
        after ^= (uint64_t(1) << 0) ^ (uint64_t(1) << faultBit2);
    std::ostringstream key;
    key << "ch=0,rank=0,bank=" << targetBank << ",row=0,burst_col=0,cw=" << codeword;
    out << "INJECTION,," << fault << ',' << faultCycle << ",\"" << key.str() << "\","
        << codeword << ',' << bits << ',' << hex64(before) << ',' << hex64(after) << ','
        << accessPath << ",,,,\n";
}

void writeJson(const std::string& path, const std::string& profile, const std::string& pathName,
               const std::string& fault, unsigned targetBank, const std::string& granularity,
               uint64_t delay, uint64_t interval, const RunOutcome& outcome,
               const Phase1Reports& reports, const BurstType& output, const BurstType& original,
               bool formatInvalid, Phase1PacketStatus packetStatus)
{
    const bool equal = (std::memcmp(output.u8Data_, original.u8Data_, sizeof(output.u8Data_)) == 0);
    std::ofstream out(path);
    if (!out)
        throw std::runtime_error("cannot open G4 metrics: " + path);
    out << "{\n"
        << "  \"profile\": \"" << jsonEscape(profile) << "\",\n"
        << "  \"path\": \"" << jsonEscape(pathName) << "\",\n"
        << "  \"fault_kind\": \"" << jsonEscape(fault) << "\",\n"
        << "  \"target_bank\": " << targetBank << ",\n"
        << "  \"granularity\": \"" << jsonEscape(granularity) << "\",\n"
        << "  \"transport_delay_cycles\": " << delay << ",\n"
        << "  \"service_interval_cycles\": " << interval << ",\n"
        << "  \"cycles\": " << outcome.cycles << ",\n"
        << "  \"timeout\": " << (outcome.timeout ? "true" : "false") << ",\n"
        << "  \"internal_detections\": " << reports.internalDetections() << ",\n"
        << "  \"internal_ce\": " << reports.internalCe() << ",\n"
        << "  \"internal_ue\": " << reports.internalUe() << ",\n"
        << "  \"delivered_reports\": " << reports.delivered() << ",\n"
        << "  \"dropped_reports\": " << reports.dropped() << ",\n"
        << "  \"max_queue_depth\": " << reports.maxQueueDepth() << ",\n"
        << "  \"format_events\": " << reports.formatEvents() << ",\n"
        << "  \"format_invalid\": " << (formatInvalid ? "true" : "false") << ",\n"
        << "  \"packet_status\": \"" << packetStatusName(packetStatus) << "\",\n"
        << "  \"output_equals_original\": " << (equal ? "true" : "false") << ",\n"
        << "  \"report_latencies\": [";
    const auto& latencies = reports.reportLatencies();
    for (std::size_t i = 0; i < latencies.size(); i++)
    {
        if (i) out << ",";
        out << latencies[i];
    }
    out << "],\n"
        << "  \"report_classes\": [";
    const auto& publicReports = reports.reports();
    for (std::size_t i = 0; i < publicReports.size(); i++)
    {
        if (i) out << ",";
        out << "\"" << publicReports[i].error_class << "\"";
    }
    out << "]\n}\n";
}
}  // namespace

TEST(Phase1G4Fixture, RunCase)
{
    const char* configPath = std::getenv("PHASE1_CONFIG");
    const char* outputPath = std::getenv("PHASE1_OUT");
    if (configPath == nullptr)
        GTEST_SKIP() << "PHASE1_CONFIG is not set";
    ASSERT_NE(nullptr, outputPath) << "PHASE1_OUT is not set";
    const std::string outputDir(outputPath);

    try
    {
        const auto params = readParams(configPath);
        const std::string deviceIni = get(params, "device_ini");
        const std::string systemIni = get(params, "system_ini");
        const std::string profileName = get(params, "profile");
        const std::string path = get(params, "path");
        const std::string fault = get(params, "fault_kind");
        const std::string granularity = get(params, "granularity");
        const unsigned targetBank = getU(params, "target_bank");
        const unsigned targetLane = getU(params, "target_lane");
        const unsigned dataBitInLane = getU(params, "data_bit_in_lane");
        const unsigned faultBit2 = getU(params, "fault_bit2");
        const uint64_t delay = std::stoull(get(params, "transport_delay_cycles"));
        const uint64_t interval = std::stoull(get(params, "service_interval_cycles"));
        const unsigned queueCapacity = getU(params, "report_queue_capacity");
        const uint64_t watchdog = std::stoull(get(params, "watchdog_cycles"));

        Phase1Profile profile = Phase1Profile::NATIVE;
        if (profileName == "NATIVE") profile = Phase1Profile::NATIVE;
        else if (profileName == "LOCAL_VISIBLE") profile = Phase1Profile::LOCAL_VISIBLE;
        else if (profileName == "LOCAL_HIDDEN") profile = Phase1Profile::LOCAL_HIDDEN;
        else if (profileName == "HOST_ONLY") profile = Phase1Profile::HOST_ONLY;
        else throw std::runtime_error("invalid profile");

        auto memory = std::make_shared<MultiChannelMemorySystem>(
            deviceIni, systemIni, ".", "phase1_g4", 256);
        memory->setCPUClockSpeed(0);
        Phase1Geometry geometry = Phase1Address::fromConfiguration();
        ASSERT_EQ(1u, geometry.num_channels);
        ASSERT_EQ(1u, geometry.num_ranks);
        ASSERT_LT(targetBank, geometry.num_banks);
        Phase1Address codec(geometry);

        const bool deliverCe = profile != Phase1Profile::NATIVE &&
                               profile != Phase1Profile::LOCAL_HIDDEN;
        const bool deliverUe = profile != Phase1Profile::NATIVE;
        Phase1Reports reports(codec, geometry.num_channels, geometry.num_banks, granularity, delay,
                              interval, queueCapacity, deliverCe, deliverUe);
        configureBanks(*memory, profile, &reports, geometry.num_banks);

        Phase1AddrTuple tuple;
        tuple.channel = 0;
        tuple.rank = 0;
        tuple.bank = targetBank;
        tuple.row = 0;
        tuple.burst_column = 0;
        const uint64_t address = codec.encode(tuple);

        BurstType original;
        for (unsigned lane = 0; lane < 16; lane++)
            original.fp16Data_[lane] = convertF2H(static_cast<float>(lane + 1) / 8.0f);

        BurstType output;
        Phase1PacketStatus packetStatus = Phase1PacketStatus::OK;
        uint64_t faultCycle = 0;
        if (path == "HREAD" || path == "HREAD_BURST")
        {
            hostWrite(*memory, address, original, reports, watchdog);
            if (fault != "F0") inject((*memory->channels[0]->ranks)[0]->banks[targetBank], fault,
                                      targetLane, dataBitInLane, faultBit2);
                faultCycle = memory->currentClockCycle;
            if (path == "HREAD_BURST")
            {
                const unsigned reads = getU(params, "repeat_reads");
                std::vector<BurstType> receive(reads);
                for (unsigned i = 0; i < reads; i++)
                    ASSERT_TRUE(memory->addTransaction(false, address, &receive[i]));
                if (!memory->addBarrier(0))
                    throw std::runtime_error("burst read barrier was not accepted");
                RunOutcome outcome = runMemory(*memory, reports, watchdog);
                ASSERT_FALSE(outcome.timeout);
                output = receive.back();
            }
            else
            {
                output = hostRead(*memory, address, reports, watchdog);
            }
        }
        else if (path == "PREAD")
        {
            hostWrite(*memory, address, original, reports, watchdog);
            if (fault != "F0") inject((*memory->channels[0]->ranks)[0]->banks[targetBank], fault,
                                      targetLane, dataBitInLane, faultBit2);
                faultCycle = memory->currentClockCycle;
            output = pimRead(*memory, targetBank, packetStatus, reports, watchdog);
        }
        else if (path == "PWRITE_HREAD")
        {
            pimWrite(*memory, targetBank, original, reports, watchdog);
            if (fault != "F0") inject((*memory->channels[0]->ranks)[0]->banks[targetBank], fault,
                                      targetLane, dataBitInLane, faultBit2);
                faultCycle = memory->currentClockCycle;
            output = hostRead(*memory, address, reports, watchdog);
        }
        else if (path == "PWRITE_PREAD")
        {
            pimWrite(*memory, targetBank, original, reports, watchdog);
            if (fault != "F0") inject((*memory->channels[0]->ranks)[0]->banks[targetBank], fault,
                                      targetLane, dataBitInLane, faultBit2);
                faultCycle = memory->currentClockCycle;
            output = pimRead(*memory, targetBank, packetStatus, reports, watchdog);
        }
        else
        {
            FAIL() << "invalid path";
        }

        reports.writeCsv(join(outputDir, "reports.csv"));
        Phase1Trace events(join(outputDir, "events.csv"), false);  // G4 does not use feeder events
        const std::string outputFile = join(outputDir, "output.bin");
        std::ofstream bin(outputFile, std::ios::binary);
        bin.write(reinterpret_cast<const char*>(output.u8Data_), sizeof(output.u8Data_));
        bin.close();
        writeTruth(join(outputDir, "truth.csv"), fault, targetBank, targetLane, dataBitInLane,
                   faultBit2, faultCycle, path, original);
        writeJson(join(outputDir, "metrics.json"), profileName, path, fault, targetBank, granularity,
                  delay, interval, RunOutcome{false, memory->currentClockCycle}, reports, output,
                  original, reports.formatEvents() > 0 ||
                                   packetStatus == Phase1PacketStatus::UNPROTECTED_FORMAT,
                  packetStatus);
    }
    catch (const std::exception& error)
    {
        FAIL() << "G4 case failed: " << error.what();
    }
}

#include <sys/stat.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "tests/PIMKernel.h"
#include "Phase1Address.h"
#include "Phase1Driver.h"
#include "Phase1Feeder.h"
#include "Phase1Trace.h"

using namespace DRAMSim;

namespace
{
std::string envOrEmpty(const char* name)
{
    const char* value = std::getenv(name);
    return value == nullptr ? "" : std::string(value);
}

void ensureDirectory(const std::string& path)
{
    if (path.empty())
        throw std::runtime_error("PHASE1_OUT is empty");
    struct stat info;
    if (stat(path.c_str(), &info) == 0)
    {
        if (!S_ISDIR(info.st_mode))
            throw std::runtime_error("PHASE1_OUT is not a directory: " + path);
        return;
    }
    if (mkdir(path.c_str(), 0755) != 0)
        throw std::runtime_error("cannot create PHASE1_OUT: " + path);
}

std::string joinPath(const std::string& directory, const std::string& name)
{
    if (directory.empty())
        return name;
    if (directory.back() == '/')
        return directory + name;
    return directory + "/" + name;
}

void writeRawBursts(const std::string& path, const std::vector<BurstType>& bursts)
{
    std::ofstream output(path, std::ios::binary);
    if (!output)
        throw std::runtime_error("cannot open output: " + path);
    for (const BurstType& burst : bursts)
        output.write(reinterpret_cast<const char*>(burst.u8Data_), sizeof(burst.u8Data_));
    if (!output)
        throw std::runtime_error("failed writing output: " + path);
}

void writeTiming(const std::string& path, const Phase1RunConfig& config,
                 const Phase1RunStats& stats, unsigned dimBursts, unsigned elements,
                 unsigned burstBytes, const std::string& outputPath)
{
    std::ofstream output(path);
    if (!output)
        throw std::runtime_error("cannot open timing output: " + path);
    output << "{\n"
           << "  \"experiment\": \"" << config.experiment << "\",\n"
           << "  \"workload\": \"" << config.workload << "\",\n"
           << "  \"mode\": \"" << config.mode << "\",\n"
           << "  \"trace_mode\": \"" << config.trace_mode << "\",\n"
           << "  \"channels\": " << config.channels << ",\n"
           << "  \"ranks\": " << config.ranks << ",\n"
           << "  \"capacity_mb\": " << config.capacity_mb << ",\n"
           << "  \"n_tiles\": " << config.n_tiles << ",\n"
           << "  \"seed\": " << config.seed << ",\n"
           << "  \"dim_bursts\": " << dimBursts << ",\n"
           << "  \"elements\": " << elements << ",\n"
           << "  \"burst_bytes\": " << burstBytes << ",\n"
           << "  \"cycles\": " << stats.cycles << ",\n"
           << "  \"updates\": " << stats.updates << ",\n"
           << "  \"timeout\": " << (stats.timeout ? "true" : "false") << ",\n"
           << "  \"feeder_used\": " << (stats.feeder_used ? "true" : "false") << ",\n"
           << "  \"pending_at_end\": " << stats.pending_at_end << ",\n"
           << "  \"outstanding_at_end\": " << stats.outstanding_at_end << ",\n"
           << "  \"max_external_pending\": " << stats.max_external_pending << ",\n"
           << "  \"max_accepted_outstanding\": " << stats.max_accepted_outstanding << ",\n"
           << "  \"controller_rejections\": " << stats.controller_rejections << ",\n"
           << "  \"barriers_retired\": " << stats.barriers_retired << ",\n"
           << "  \"output\": \"" << outputPath << "\"\n"
           << "}\n";
}

Phase1Geometry requireGeometry(MultiChannelMemorySystem& memory, const Phase1RunConfig& config)
{
    Phase1Geometry geometry = Phase1Address::fromConfiguration();
    if (!geometry.valid())
        throw std::runtime_error("invalid upstream memory geometry");
    EXPECT_EQ(geometry.num_channels, config.channels);
    EXPECT_EQ(geometry.num_ranks, config.ranks);
    EXPECT_EQ(geometry.num_banks, 16u);
    EXPECT_EQ(geometry.num_bank_groups, 4u);
    EXPECT_EQ(geometry.num_rows, 16384u);
    EXPECT_EQ(geometry.num_cols, 128u);
    EXPECT_EQ(geometry.burst_length, 4u);
    EXPECT_EQ(geometry.jedec_data_bus_bits, 64u);
    EXPECT_EQ(geometry.burst_bytes(), 32u);
    EXPECT_EQ(memory.channels.size(), config.channels);
    return geometry;
}

void runAddressCheck(const Phase1RunConfig& config, const std::string& outputDirectory)
{
    auto memory = std::make_shared<MultiChannelMemorySystem>(
        config.device_ini, config.system_ini, ".", "phase1_address", config.capacity_mb);
    memory->setCPUClockSpeed(0);
    Phase1Geometry geometry = requireGeometry(*memory, config);
    Phase1Address codec(geometry);

    const std::vector<unsigned> banks = {0u, 3u, geometry.num_banks - 1u};
    const std::vector<unsigned> rows = {0u, 1u, geometry.num_rows / 2u, geometry.num_rows - 1u};
    const std::vector<unsigned> columns = {0u, 1u, geometry.burst_columns() / 2u,
                                           geometry.burst_columns() - 1u};

    std::ofstream csv(joinPath(outputDirectory, "address_mapping.csv"));
    if (!csv)
        throw std::runtime_error("cannot write address_mapping.csv");
    csv << "channel,bank,row,burst_column,address,decoded_channel,decoded_rank,"
           "decoded_bank,decoded_row,decoded_burst_column,roundtrip_ok,upstream_decoder_ok,"
           "unique_address\n";

    std::set<uint64_t> addresses;
    unsigned roundtripPass = 0;
    unsigned upstreamPass = 0;
    unsigned uniqueCount = 0;
    unsigned total = 0;
    for (unsigned channel = 0; channel < geometry.num_channels; channel++)
    {
        for (unsigned bank : banks)
        {
            for (unsigned row : rows)
            {
                for (unsigned column : columns)
                {
                    Phase1AddrTuple tuple;
                    tuple.channel = channel;
                    tuple.rank = 0;
                    tuple.bank = bank;
                    tuple.row = row;
                    tuple.burst_column = column;
                    const uint64_t address = codec.encode(tuple);
                    const Phase1AddrTuple decoded = codec.decode(address);
                    const bool roundtrip = (decoded == tuple);
                    const bool unique = addresses.insert(address).second;
                    if (!unique)
                    {
                        ADD_FAILURE() << "address collision at " << phase1AddrToString(tuple)
                                      << " address=0x" << std::hex << address << std::dec;
                    }

                    unsigned upstreamChannel = 0, upstreamRank = 0, upstreamBank = 0,
                             upstreamRow = 0, upstreamColumn = 0;
                    memory->addrMapping->addressMapping(address, upstreamChannel, upstreamRank,
                                                        upstreamBank, upstreamRow, upstreamColumn);
                    const bool upstream =
                        upstreamChannel == tuple.channel && upstreamRank == tuple.rank &&
                        upstreamBank == tuple.bank && upstreamRow == tuple.row &&
                        upstreamColumn == tuple.burst_column;
                    const bool aligned = (address % geometry.burst_bytes()) == 0;
                    const bool inRange = address <= codec.addressMask();
                    EXPECT_TRUE(roundtrip);
                    EXPECT_TRUE(upstream);
                    EXPECT_TRUE(aligned);
                    EXPECT_TRUE(inRange);
                    if (roundtrip)
                        roundtripPass++;
                    if (upstream)
                        upstreamPass++;
                    if (unique)
                        uniqueCount++;
                    total++;
                    csv << channel << "," << bank << "," << row << "," << column << ","
                        << address << "," << decoded.channel << "," << decoded.rank << ","
                        << decoded.bank << "," << decoded.row << "," << decoded.burst_column << ","
                        << (roundtrip ? 1 : 0) << "," << (upstream ? 1 : 0) << ","
                        << (unique ? 1 : 0) << "\n";
                }
            }
        }
    }

    const bool allPass = total == roundtripPass && total == upstreamPass && total == uniqueCount;
    std::ofstream summary(joinPath(outputDirectory, "address_mapping.json"));
    if (!summary)
        throw std::runtime_error("cannot write address_mapping.json");
    summary << "{\n"
            << "  \"channels\": " << config.channels << ",\n"
            << "  \"capacity_mb\": " << config.capacity_mb << ",\n"
            << "  \"banks_checked\": [0, 3, " << (geometry.num_banks - 1) << "],\n"
            << "  \"rows_checked\": [0, 1, " << (geometry.num_rows / 2) << ", "
            << (geometry.num_rows - 1) << "],\n"
            << "  \"burst_columns_checked\": [0, 1, " << (geometry.burst_columns() / 2) << ", "
            << (geometry.burst_columns() - 1) << "],\n"
            << "  \"burst_bytes\": " << geometry.burst_bytes() << ",\n"
            << "  \"address_bits\": " << (codec.addressMask() == 0 ? 0 : 64 - __builtin_clzll(codec.addressMask())) << ",\n"
            << "  \"tuples\": " << total << ",\n"
            << "  \"unique_addresses\": " << uniqueCount << ",\n"
            << "  \"roundtrip_pass\": " << roundtripPass << ",\n"
            << "  \"upstream_decoder_pass\": " << upstreamPass << ",\n"
            << "  \"all_pass\": " << (allPass ? "true" : "false") << "\n"
            << "}\n";
    EXPECT_TRUE(allPass);
}

void runW0(const Phase1RunConfig& config, const std::string& outputDirectory)
{
    std::shared_ptr<MultiChannelMemorySystem> memory;
    std::shared_ptr<Phase1FeederMemory> feeder;
    if (config.mode == "feeder")
    {
        feeder = std::make_shared<Phase1FeederMemory>(
            config.device_ini, config.system_ini, ".", "phase1_w0", config.capacity_mb,
            config.feeder_outstanding_limit);
        memory = feeder;
    }
    else
    {
        memory = std::make_shared<MultiChannelMemorySystem>(
            config.device_ini, config.system_ini, ".", "phase1_w0", config.capacity_mb);
    }
    memory->setCPUClockSpeed(0);
    Phase1Geometry geometry = requireGeometry(*memory, config);
    Phase1Address codec(geometry);
    Phase1AddrTuple tuple;
    tuple.channel = 0;
    tuple.rank = 0;
    tuple.bank = 0;
    tuple.row = 0;
    tuple.burst_column = 0;
    const uint64_t address = codec.encode(tuple);

    BurstType writeBurst;
    BurstType readBurst;
    for (unsigned lane = 0; lane < 16; lane++)
        writeBurst.fp16Data_[lane] = convertF2H(static_cast<float>(lane + 1) / 8.0f);

    if (config.mode == "feeder")
    {
        EXPECT_TRUE(feeder->addTransaction(true, address, &writeBurst));
        EXPECT_TRUE(feeder->addBarrier(0));
        EXPECT_TRUE(feeder->addTransaction(false, address, &readBurst));
        EXPECT_TRUE(feeder->addBarrier(0));
    }
    else
    {
        EXPECT_TRUE(memory->addTransaction(true, address, &writeBurst));
        EXPECT_TRUE(memory->addBarrier(0));
        EXPECT_TRUE(memory->addTransaction(false, address, &readBurst));
        EXPECT_TRUE(memory->addBarrier(0));
    }

    Phase1RunStats stats = (config.mode == "feeder")
                                ? Phase1Driver::runFeeder(*feeder, config.watchdog_cycles)
                                : Phase1Driver::runDirect(*memory, config.watchdog_cycles);
    EXPECT_FALSE(stats.timeout);
    EXPECT_EQ(0, std::memcmp(writeBurst.u8Data_, readBurst.u8Data_, sizeof(writeBurst.u8Data_)));

    std::vector<BurstType> output{readBurst};
    const std::string outputPath = joinPath(outputDirectory, "output.bin");
    writeRawBursts(outputPath, output);
    writeTiming(joinPath(outputDirectory, "timing.json"), config, stats, 1u, 16u,
                geometry.burst_bytes(), outputPath);
}

void fillEltwiseInputs(unsigned dimBursts, unsigned seed, std::vector<BurstType>& input0,
                       std::vector<BurstType>& input1)
{
    input0.assign(dimBursts, BurstType());
    input1.assign(dimBursts, BurstType());
    const uint64_t elements = static_cast<uint64_t>(dimBursts) * 16u;
    for (uint64_t i = 0; i < elements; i++)
    {
        const unsigned burst = static_cast<unsigned>(i / 16u);
        const unsigned lane = static_cast<unsigned>(i % 16u);
        const float a = static_cast<float>((static_cast<long long>(i) + seed) % 17 - 8) / 8.0f;
        const float x =
            static_cast<float>((3 * static_cast<long long>(i) + seed) % 13 - 6) / 8.0f;
        input0[burst].fp16Data_[lane] = convertF2H(a);
        input1[burst].fp16Data_[lane] = convertF2H(x);
    }
}

void runW1(const Phase1RunConfig& config, const std::string& outputDirectory)
{
    std::shared_ptr<MultiChannelMemorySystem> memory;
    std::shared_ptr<Phase1FeederMemory> feeder;
    if (config.mode == "feeder")
    {
        feeder = std::make_shared<Phase1FeederMemory>(
            config.device_ini, config.system_ini, ".", "phase1_w1", config.capacity_mb,
            config.feeder_outstanding_limit);
        memory = feeder;
    }
    else
    {
        memory = std::make_shared<MultiChannelMemorySystem>(
            config.device_ini, config.system_ini, ".", "phase1_w1", config.capacity_mb);
    }
    memory->setCPUClockSpeed(0);
    Phase1Geometry geometry = requireGeometry(*memory, config);
    Phase1Trace trace(joinPath(outputDirectory, "events.csv"), config.trace_mode == "ext_on");
    if (feeder)
        feeder->setTrace(&trace);

    const unsigned tileDivisor =
        geometry.num_banks * config.channels * config.ranks * 8u;  // 8 GRF bursts
    ASSERT_EQ(0u, tileDivisor % (geometry.num_banks * config.channels * config.ranks));
    const unsigned dimBursts = config.n_tiles * tileDivisor;
    const uint64_t elements = static_cast<uint64_t>(dimBursts) * 16u;
    ASSERT_EQ(0u, dimBursts % tileDivisor);
    ASSERT_EQ(config.n_tiles, dimBursts / tileDivisor);

    std::vector<BurstType> input0;
    std::vector<BurstType> input1;
    fillEltwiseInputs(dimBursts, config.seed, input0, input1);
    std::vector<BurstType> result(dimBursts);

    NumpyBurstType a;
    a.bShape.push_back(dimBursts);
    a.bData = input0;
    NumpyBurstType x;
    x.bShape.push_back(dimBursts);
    x.bData = input1;

    auto kernel = std::make_shared<PIMKernel>(memory, config.channels, config.ranks);
    kernel->preloadNoReplacement(&a, 0, 0);
    kernel->preloadNoReplacement(&x, 128, 0);
    kernel->executeEltwise(static_cast<int>(dimBursts), pimBankType::ALL_BANK, KernelType::ADD,
                           0, 256, 128);
    kernel->readData(result.data(), dimBursts, 256, 0);

    trace.event(memory->currentClockCycle, "START", "RUN_START", "1", "1", "", -1, -1, -1, -1,
                -1, "", -1, "started");
    Phase1RunStats stats = (config.mode == "feeder")
                                ? Phase1Driver::runFeeder(*feeder, config.watchdog_cycles)
                                : Phase1Driver::runDirect(*memory, config.watchdog_cycles);
    trace.event(memory->currentClockCycle, "FINALIZE", "RUN_END", "1", "1", "", -1, -1, -1, -1,
                -1, "", -1, stats.timeout ? "timeout" : "finished");
    EXPECT_FALSE(stats.timeout);

    const std::string outputPath = joinPath(outputDirectory, "output.bin");
    writeRawBursts(outputPath, result);
    writeRawBursts(joinPath(outputDirectory, "input_a.bin"), input0);
    writeRawBursts(joinPath(outputDirectory, "input_x.bin"), input1);
    writeTiming(joinPath(outputDirectory, "timing.json"), config, stats, dimBursts,
                static_cast<unsigned>(elements), geometry.burst_bytes(), outputPath);
}

}  // namespace

TEST(Phase1Contract, FeederRetainsOnControllerRejection)
{
    const std::string systemIni =
        envOrEmpty("PHASE1_SYSTEM_INI").empty() ? "../configs/phase1_system_1ch.ini"
                                                : envOrEmpty("PHASE1_SYSTEM_INI");
    if (!std::ifstream(systemIni).good())
    {
        GTEST_SKIP() << "Phase1 system ini not available: " << systemIni;
    }

    try
    {
        auto feeder = std::make_shared<Phase1FeederMemory>(
            "ini/HBM2_samsung_2M_16B_x64.ini", systemIni, ".", "phase1_contract", 256, 8);
        feeder->setCPUClockSpeed(0);
        BurstType payload;
        for (unsigned lane = 0; lane < 16; lane++)
            payload.fp16Data_[lane] = convertF2H(static_cast<float>(lane + 1) / 8.0f);

        Phase1Geometry geometry = Phase1Address::fromConfiguration();
        Phase1Address codec(geometry);
        Phase1AddrTuple tuple;
        tuple.channel = 0;
        tuple.rank = 0;
        tuple.bank = 0;
        tuple.row = 0;
        tuple.burst_column = 0;
        const uint64_t address = codec.encode(tuple);

        // Fill the controller's bounded transaction queue directly through the
        // upstream base API.  This is a test-only admission-pressure setup; it
        // does not bypass the storage path or inspect truth.
        unsigned prefill = 0;
        for (; prefill < getConfigParam(UINT, "TRANS_QUEUE_DEPTH"); prefill++)
        {
            if (!feeder->MultiChannelMemorySystem::addTransaction(true, address, &payload))
                break;
        }
        ASSERT_EQ(getConfigParam(UINT, "TRANS_QUEUE_DEPTH"), prefill);

        BurstType received;
        EXPECT_TRUE(feeder->addTransaction(false, address, &received));
        EXPECT_FALSE(feeder->tick());  // controller full: must retain descriptor
        EXPECT_GE(feeder->controllerRejections(), 1u);
        EXPECT_EQ(1u, feeder->pendingCount());

        Phase1RunStats stats = Phase1Driver::runFeeder(*feeder, 100000);
        EXPECT_FALSE(stats.timeout);
        EXPECT_EQ(0u, feeder->pendingCount());
        EXPECT_EQ(0u, feeder->outstandingCount());
        EXPECT_GE(feeder->controllerRejections(), 1u);
        EXPECT_EQ(0, std::memcmp(payload.u8Data_, received.u8Data_, sizeof(payload.u8Data_)));
    }
    catch (const std::exception& error)
    {
        FAIL() << "feeder admission test failed: " << error.what();
    }
}

TEST(Phase1Fixture, RunCase)
{
    const std::string configPath = envOrEmpty("PHASE1_CONFIG");
    const std::string outputDirectory = envOrEmpty("PHASE1_OUT");
    if (configPath.empty())
    {
        GTEST_SKIP() << "PHASE1_CONFIG is not set";
    }
    ASSERT_FALSE(outputDirectory.empty()) << "PHASE1_OUT is not set";

    try
    {
        ensureDirectory(outputDirectory);
        Phase1RunConfig config = loadPhase1Config(configPath);
        if (config.workload == "ADDRESS")
            runAddressCheck(config, outputDirectory);
        else if (config.workload == "W0")
            runW0(config, outputDirectory);
        else if (config.workload == "W1")
            runW1(config, outputDirectory);
        else
            FAIL() << "unsupported workload " << config.workload;
    }
    catch (const std::exception& error)
    {
        FAIL() << "Phase1 case failed: " << error.what();
    }
}

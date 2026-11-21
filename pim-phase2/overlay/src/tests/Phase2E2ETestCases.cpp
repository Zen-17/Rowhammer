#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "gtest/gtest.h"

#include "Phase1Address.h"
#include "Phase1Driver.h"
#include "Phase1Feeder.h"
#include "Phase1Pause.h"
#include "Phase1Reports.h"
#include "Phase1Trace.h"
#include "Phase2Checkpoint.h"
#include "Phase2Engine.h"
#include "Phase2Gate.h"
#include "Phase2Isolation.h"
#include "Phase2Layout.h"
#include "Phase2Ledger.h"
#include "Phase2Types.h"
#include "tests/PIMKernel.h"

using namespace DRAMSim;
using namespace DRAMSim::phase2;

namespace
{
struct P2LoopStats
{
    uint64_t cycles = 0;
    bool timeout = false;
};

std::string p2Trim(const std::string& input)
{
    const std::string ws = " \t\r\n";
    const std::size_t first = input.find_first_not_of(ws);
    if (first == std::string::npos)
        return "";
    const std::size_t last = input.find_last_not_of(ws);
    return input.substr(first, last - first + 1);
}

std::map<std::string, std::string> p2ReadParams(const std::string& path)
{
    std::ifstream input(path);
    if (!input)
        throw std::runtime_error("cannot open Phase 2 params: " + path);
    std::map<std::string, std::string> values;
    std::string line;
    while (std::getline(input, line))
    {
        line = p2Trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#')
            continue;
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos)
            throw std::runtime_error("malformed Phase 2 param line");
        values[p2Trim(line.substr(0, eq))] = p2Trim(line.substr(eq + 1));
    }
    return values;
}

std::string p2Get(const std::map<std::string, std::string>& params, const std::string& key)
{
    auto found = params.find(key);
    if (found == params.end())
        throw std::runtime_error("missing Phase 2 param: " + key);
    return found->second;
}

std::string p2GetOr(const std::map<std::string, std::string>& params, const std::string& key,
                    const std::string& fallback)
{
    auto found = params.find(key);
    return found == params.end() ? fallback : found->second;
}

unsigned p2GetU(const std::map<std::string, std::string>& params, const std::string& key)
{
    return static_cast<unsigned>(std::stoul(p2Get(params, key)));
}

uint64_t p2GetU64(const std::map<std::string, std::string>& params, const std::string& key)
{
    return std::stoull(p2Get(params, key));
}

uint64_t p2GetU64Or(const std::map<std::string, std::string>& params, const std::string& key,
                    uint64_t fallback)
{
    auto found = params.find(key);
    return found == params.end() ? fallback : std::stoull(found->second);
}

bool p2GetBool(const std::map<std::string, std::string>& params, const std::string& key,
               bool fallback)
{
    auto found = params.find(key);
    if (found == params.end())
        return fallback;
    const std::string value = found->second;
    return value == "1" || value == "true" || value == "ON" || value == "on";
}

std::vector<double> p2GetDoubleList(const std::map<std::string, std::string>& params,
                                    const std::string& key)
{
    std::vector<double> values;
    auto found = params.find(key);
    if (found == params.end() || found->second.empty())
        return values;
    std::stringstream stream(found->second);
    std::string part;
    while (std::getline(stream, part, ','))
        if (!part.empty())
            values.push_back(std::stod(part));
    return values;
}

std::string p2KernelTypeName(KernelType type)
{
    switch (type)
    {
        case KernelType::ADD: return "ADD";
        case KernelType::MUL: return "MUL";
        case KernelType::RELU: return "RELU";
    }
    return "UNKNOWN";
}

struct P2KernelTiming
{
    int kernel_idx = 0;
    std::string kernel_type;
    uint64_t t_start = 0;
    uint64_t t_end = 0;
    unsigned n_tiles = 0;
};

std::string p2Join(const std::string& dir, const std::string& name)
{
    if (dir.empty())
        return name;
    return (dir.back() == '/') ? dir + name : dir + "/" + name;
}

void p2EnsureDir(const std::string& dir)
{
    if (dir.empty())
        return;
    if (::mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST)
        throw std::runtime_error("cannot create directory: " + dir);
}

P2LoopStats p2RunFeederLoop(Phase1FeederMemory& feeder, Phase1Reports& reports,
                            Phase1PauseController* pause, std::size_t& consumedReports,
                            uint64_t watchdog, bool stopOnAck, bool waitReports = true)
{
    P2LoopStats stats;
    while ((pause == nullptr || !pause->acked()) &&
           (feeder.pendingCount() > 0 || !Phase1Driver::directIdle(feeder) ||
            (waitReports && reports.pendingCount() > 0)))
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
            if (pause != nullptr)
                pause->notifyReport(report.report_cycle);
        }
        if (pause != nullptr)
            pause->tick(feeder.currentClockCycle);
        stats.cycles++;
        if (stopOnAck && pause != nullptr && pause->acked())
            break;
    }
    return stats;
}

P2LoopStats p2RunDirectWrites(Phase1FeederMemory& feeder, uint64_t watchdog)
{
    P2LoopStats stats;
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

BurstType p2ExpectedAdd(const BurstType& a, const BurstType& x)
{
    BurstType result;
    for (unsigned lane = 0; lane < 16; lane++)
        result.fp16Data_[lane] = a.fp16Data_[lane] + x.fp16Data_[lane];
    return result;
}

BurstType p2ExpectedMul(const BurstType& a, const BurstType& x)
{
    BurstType result;
    for (unsigned lane = 0; lane < 16; lane++)
        result.fp16Data_[lane] = a.fp16Data_[lane] * x.fp16Data_[lane];
    return result;
}

BurstType p2ExpectedRelu(const BurstType& value)
{
    BurstType result;
    for (unsigned lane = 0; lane < 16; lane++)
    {
        if (convertH2F(value.fp16Data_[lane]) <= 0.0f)
            result.fp16Data_[lane] = convertF2H(0.0f);
        else
            result.fp16Data_[lane] = value.fp16Data_[lane];
    }
    return result;
}

struct P2KernelRows
{
    KernelType ktype;
    int input0_row;
    int result_row;
    int input1_row;
};

struct P2LayoutSpec
{
    std::map<uint16_t, unsigned> base_rows;
    std::vector<P2KernelRows> kernels;
    int result_row = 0;
};

P2LayoutSpec p2LayoutFor(const std::string& workload)
{
    P2LayoutSpec spec;
    if (workload == "W1")
    {
        spec.base_rows = {{0, 0}, {1, 128}, {2, 256}};
        spec.kernels = {{KernelType::ADD, 0, 256, 128}};
        spec.result_row = 256;
    }
    else if (workload == "W2")
    {
        spec.base_rows = {{0, 0}, {1, 128}, {3, 256}, {4, 384}, {5, 512}, {6, 640}};
        spec.kernels = {{KernelType::ADD, 0, 256, 128},
                        {KernelType::MUL, 256, 512, 384},
                        {KernelType::RELU, 512, 640, 0}};
        spec.result_row = 640;
    }
    else
    {
        throw std::runtime_error("unsupported workload: " + workload);
    }
    return spec;
}

unsigned p2InputRow(const P2LayoutSpec& layout, uint16_t operand)
{
    auto found = layout.base_rows.find(operand);
    if (found == layout.base_rows.end())
        throw std::runtime_error("fault operand is not in the workload layout");
    return found->second;
}

struct P2ObserverState
{
    Ledger* ledger = nullptr;
    std::map<std::pair<int, int>, uint64_t> op_ids;
    int current_kernel = -1;
    P2KernelRows rows;
    std::map<std::pair<int, int>, uint32_t> counts;
    std::map<std::pair<int, int>, uint32_t> expected;
};

void p2InstallObserver(Phase1FeederMemory& feeder,
                       const std::shared_ptr<P2ObserverState>& state)
{
    feeder.setCompletionObserver([state](const P1CompletionEvent& event) {
        if (state->ledger == nullptr || state->current_kernel < 0)
            return;
        int row_base = -1;
        const int kernel = state->current_kernel;
        if (kernel == 0 || kernel == 1)
        {
            if (event.tag == "BANK_TO_GRF_")
                row_base = state->rows.input0_row;
            else if (event.tag == "ADD")
                row_base = state->rows.input1_row;
            else if (event.tag == "GRF_TO_BANK")
                row_base = state->rows.result_row;
            else
                return;
        }
        else if (kernel == 2)
        {
            if (event.tag == "FILL&ReLU")
                row_base = state->rows.input0_row;
            else if (event.tag == "GRF_A_TO_EVEN_BANK" ||
                     event.tag == "GRF_B_TO_ODD_BANK")
                row_base = state->rows.result_row;
            else
                return;
        }
        else
        {
            return;
        }
        if (event.row < static_cast<unsigned>(row_base))
            return;
        const int tile = static_cast<int>(event.row - row_base) * 4 +
                         static_cast<int>(event.column) / 8;
        if (tile < 0)
            return;
        const auto key = std::make_pair(kernel, tile);
        auto op_it = state->op_ids.find(key);
        if (op_it == state->op_ids.end())
            return;
        OpEntry* op = state->ledger->findMutable(op_it->second);
        if (op == nullptr)
            return;
        if (op->t_issue == UINT64_MAX)
            state->ledger->recordIssue(op->op_id, event.submit_cycle);
        uint32_t& count = state->counts[key];
        count++;
        auto expected_it = state->expected.find(key);
        if (!op->completed && expected_it != state->expected.end() &&
            count >= expected_it->second)
            state->ledger->recordComplete(op->op_id, event.complete_cycle);
    });
}

struct P2ExecuteResult
{
    uint64_t cycles = 0;
    bool timeout = false;
};

P2ExecuteResult p2ExecuteKernel(std::shared_ptr<PIMKernel> kernel,
                                const P2KernelRows& rows, int tile_begin, int tile_count,
                                const std::string& mode, int kernel_idx,
                                const std::shared_ptr<P2ObserverState>& observer,
                                Phase1FeederMemory& feeder, Phase1Reports& reports,
                                Phase1PauseController* pause, std::size_t& consumed,
                                uint64_t watchdog, bool waitReports = true)
{
    observer->current_kernel = kernel_idx;
    observer->rows = rows;
    if (mode == "full" && tile_begin == 0)
    {
        const int dim = tile_count * 128;
        kernel->executeEltwise(dim, pimBankType::ALL_BANK, rows.ktype, rows.input0_row,
                               rows.result_row, rows.input1_row);
    }
    else if (mode == "segments")
    {
        int remaining = tile_count;
        int offset = tile_begin;
        int segments = std::min(3, tile_count);
        for (int segment = 0; segment < segments; segment++)
        {
            const int count = remaining / (segments - segment);
            kernel->executeEltwiseTiles(rows.ktype, offset, count, rows.input0_row,
                                        rows.result_row, rows.input1_row);
            offset += count;
            remaining -= count;
        }
    }
    else
    {
        kernel->executeEltwiseTiles(rows.ktype, tile_begin, tile_count, rows.input0_row,
                                    rows.result_row, rows.input1_row);
    }
    const P2LoopStats stats =
        p2RunFeederLoop(feeder, reports, pause, consumed, watchdog, pause != nullptr,
                        waitReports);
    P2ExecuteResult result;
    result.cycles = stats.cycles;
    result.timeout = stats.timeout;
    return result;
}

std::string p2Refs(const std::vector<DataRef>& refs)
{
    return dataRefsCsv(refs);
}

std::string p2TileList(const std::vector<uint32_t>& tiles)
{
    std::ostringstream os;
    for (std::size_t i = 0; i < tiles.size(); i++)
    {
        if (i != 0) os << ';';
        os << tiles[i];
    }
    return os.str();
}

bool p2Contains(const std::vector<uint64_t>& values, uint64_t value)
{
    return std::find(values.begin(), values.end(), value) != values.end();
}

std::string p2JsonBool(bool value)
{
    return value ? "true" : "false";
}
}  // namespace

TEST(Phase2E2EFixture, RunCase)
{
    const char* configPath = std::getenv("PHASE2_CONFIG");
    const char* outputPath = std::getenv("PHASE2_OUT");
    if (configPath == nullptr)
        GTEST_SKIP() << "PHASE2_CONFIG is not set";
    ASSERT_NE(nullptr, outputPath);
    const std::string outputDir(outputPath);
    p2EnsureDir(outputDir);

    try
    {
        const auto params = p2ReadParams(configPath);
        const std::string workload = p2Get(params, "workload");
        const unsigned nTiles = p2GetU(params, "n_tiles");
        const unsigned seed = p2GetU(params, "seed");
        const std::string granularity = p2Get(params, "granularity");
        const unsigned transportDelay = p2GetU(params, "transport_delay");
        const unsigned serviceInterval = p2GetU(params, "service_interval");
        const unsigned capacity = p2GetU(params, "capacity");
        const unsigned handlerDelay = p2GetU(params, "handler_delay");
        const std::string faultKind = p2Get(params, "fault_kind");
        const uint16_t faultOperand =
            static_cast<uint16_t>(std::stoul(p2Get(params, "fault_operand")));
        const unsigned faultTile = p2GetU(params, "fault_tile");
        const unsigned faultBank = p2GetU(params, "fault_bank");
        const std::string policy = p2Get(params, "policy");
        const unsigned radiusRows = p2GetU(params, "radius_rows");
        const std::string negativeControl = p2GetOr(params, "negative_control", "NONE");
        const uint64_t watchdog = p2GetU64(params, "watchdog_cycles");
        const std::string deviceIni = p2Get(params, "device_ini");
        const std::string systemIni = p2Get(params, "system_ini");
        const std::string accounting = p2GetOr(params, "accounting", "ON");
        const std::string observerMode = p2GetOr(params, "observer", "ON");
        const std::string kernelMode = p2GetOr(params, "kernel_mode", "tiles");
        const uint64_t maxLogEntries = p2GetU64Or(params, "max_log_entries", 1000000);
        const std::string checkpointMissing = p2GetOr(params, "checkpoint_missing", "");
        const std::string release = p2GetOr(params, "release", "GLOBAL_HOLD");
        const uint64_t holdCycles = p2GetU64Or(params, "hold_cycles", 0);
        const std::string windowMode = p2GetOr(params, "window_mode", "AUTO");
        const std::vector<double> kernelCFix = p2GetDoubleList(params, "kernel_c_fix");
        const std::vector<double> kernelCTile = p2GetDoubleList(params, "kernel_c_tile");
        const uint64_t baselineEndToEnd = p2GetU64Or(params, "baseline_end_to_end", 0);
        const bool releaseTest = p2GetBool(params, "release_test", false);
        uint64_t deltaCycles = p2GetU64Or(params, "delta_cycles", 0);
        if (deltaCycles == 0)
            deltaCycles = transportDelay + capacity * serviceInterval + 1;

        const bool accountingOn = (accounting == "ON" || accounting == "on");
        const P2LayoutSpec layout = p2LayoutFor(workload);
        const unsigned dim = nTiles * 128u;
        const bool w2 = (workload == "W2");

        std::vector<BurstType> inputA(dim), inputX(dim), inputY(dim), expected(dim), result(dim);
        for (unsigned b = 0; b < dim; b++)
            for (unsigned lane = 0; lane < 16; lane++)
            {
                const uint64_t i = static_cast<uint64_t>(b) * 16u + lane;
                inputA[b].fp16Data_[lane] = convertF2H(
                    static_cast<float>((static_cast<long long>(i) + seed) % 17 - 8) / 8.0f);
                inputX[b].fp16Data_[lane] = convertF2H(
                    static_cast<float>((3 * static_cast<long long>(i) + seed) % 13 - 6) /
                    8.0f);
                inputY[b].fp16Data_[lane] = convertF2H(
                    static_cast<float>((5 * static_cast<long long>(i) + seed) % 11 - 5) /
                    8.0f);
            }
        if (w2)
        {
            std::vector<BurstType> tmp(dim);
            for (unsigned b = 0; b < dim; b++)
            {
                tmp[b] = p2ExpectedAdd(inputA[b], inputX[b]);
                tmp[b] = p2ExpectedMul(tmp[b], inputY[b]);
                expected[b] = p2ExpectedRelu(tmp[b]);
            }
        }
        else
        {
            for (unsigned b = 0; b < dim; b++)
                expected[b] = p2ExpectedAdd(inputA[b], inputX[b]);
        }

        Phase2CheckpointStore checkpoint;
        for (unsigned tile = 0; tile < nTiles; tile++)
        {
            const std::size_t begin = static_cast<std::size_t>(tile) * 128u;
            checkpoint.addTile(0, tile,
                               std::vector<BurstType>(inputA.begin() + begin,
                                                      inputA.begin() + begin + 128));
            checkpoint.addTile(1, tile,
                               std::vector<BurstType>(inputX.begin() + begin,
                                                      inputX.begin() + begin + 128));
            if (w2)
                checkpoint.addTile(4, tile,
                                   std::vector<BurstType>(inputY.begin() + begin,
                                                          inputY.begin() + begin + 128));
        }
        CheckpointView checkpointView;
        if (!checkpointMissing.empty())
        {
            const std::size_t colon = checkpointMissing.find(':');
            if (colon == std::string::npos)
                throw std::runtime_error("malformed checkpoint_missing");
            checkpointView.setAbsent(
                static_cast<uint16_t>(std::stoul(checkpointMissing.substr(0, colon))),
                static_cast<unsigned>(std::stoul(checkpointMissing.substr(colon + 1))));
        }

        auto feeder = std::make_shared<Phase1FeederMemory>(deviceIni, systemIni, ".",
                                                            "phase2_e2e", 256, 8);
        feeder->setCPUClockSpeed(0);
        const Phase1Geometry geometry = Phase1Address::fromConfiguration();
        Phase1Address codec(geometry);
        Phase1Reports reports(codec, geometry.num_channels, geometry.num_banks, granularity,
                              transportDelay, serviceInterval, capacity, true, true);
        for (MemorySystem* channel : feeder->channels)
            for (Rank* rank : *channel->ranks)
                for (Bank& bank : rank->banks)
                    bank.setPhase1(Phase1Profile::LOCAL_VISIBLE, &reports);
        Phase1PauseController pause(*feeder, 0, handlerDelay);

        Ledger ledger(maxLogEntries);
        for (const auto& entry : layout.base_rows)
            ledger.setBaseRow(entry.first, entry.second);

        auto observer = std::make_shared<P2ObserverState>();
        observer->ledger = accountingOn ? &ledger : nullptr;
        std::map<std::pair<int, int>, uint64_t> opIds;
        if (accountingOn)
        {
            for (unsigned kernelIdx = 0; kernelIdx < layout.kernels.size(); kernelIdx++)
            {
                for (unsigned tile = 0; tile < nTiles; tile++)
                {
                    std::vector<std::pair<uint16_t, uint32_t>> inputs;
                    std::vector<std::pair<uint16_t, uint32_t>> outputs;
                    if (workload == "W1")
                    {
                        inputs = {{0, tile}, {1, tile}};
                        outputs = {{2, tile}};
                    }
                    else if (kernelIdx == 0)
                    {
                        inputs = {{0, tile}, {1, tile}};
                        outputs = {{3, tile}};
                    }
                    else if (kernelIdx == 1)
                    {
                        inputs = {{3, tile}, {4, tile}};
                        outputs = {{5, tile}};
                    }
                    else
                    {
                        inputs = {{5, tile}};
                        outputs = {{6, tile}};
                    }
                    const uint64_t opId = ledger.planOp(1, static_cast<uint16_t>(kernelIdx),
                                                        tile, inputs, outputs);
                    opIds[std::make_pair(static_cast<int>(kernelIdx),
                                         static_cast<int>(tile))] = opId;
                }
            }
            observer->op_ids = opIds;
            for (const auto& entry : opIds)
                observer->expected[entry.first] =
                    (entry.first.first == 2) ? 32u : 48u;
            if (observerMode == "ON" || observerMode == "on")
                p2InstallObserver(*feeder, observer);
        }

        auto kernel = std::make_shared<PIMKernel>(feeder, 1, 1);
        NumpyBurstType a;
        a.bShape.push_back(dim);
        a.bData = inputA;
        NumpyBurstType x;
        x.bShape.push_back(dim);
        x.bData = inputX;
        NumpyBurstType y;
        y.bShape.push_back(dim);
        y.bData = inputY;
        kernel->preloadNoReplacement(&a, 0, 0);
        kernel->preloadNoReplacement(&x, 128, 0);
        if (w2)
            kernel->preloadNoReplacement(&y, 384, 0);
        std::size_t consumed = 0;
        P2LoopStats preload = p2RunFeederLoop(*feeder, reports, nullptr, consumed, watchdog,
                                              false);
        if (preload.timeout)
            throw std::runtime_error("Phase 2 preload timed out");
        const uint64_t jobStart = feeder->currentClockCycle;

        const unsigned faultRow = p2InputRow(layout, faultOperand) + (8u * faultTile) / 32u;
        const unsigned faultCol = (8u * faultTile) % 32u;
        auto injectFault = [&]() {
            if (faultKind == "F0")
                return;
            if (faultBank >= (*feeder->channels[0]->ranks)[0]->banks.size())
                throw std::runtime_error("fault bank out of range");
            Bank& bank = (*feeder->channels[0]->ranks)[0]->banks[faultBank];
            if (faultKind == "F1")
                bank.injectRawDataBit(faultRow, faultCol, 15);
            else if (faultKind == "F2")
            {
                bank.injectRawDataBit(faultRow, faultCol, 0);
                bank.injectRawDataBit(faultRow, faultCol, 1);
            }
            else
            {
                throw std::runtime_error("unknown fault kind: " + faultKind);
            }
        };

        const bool inputFault = (faultOperand != 3);
        if (inputFault)
            injectFault();

        uint64_t producerCycles = 0;
        bool timeout = false;
        std::size_t consumedBeforeRecovery = consumed;
        std::vector<P2KernelTiming> kernelTimings;
        auto executeProducer = [&](int kernelIdx, int tileBegin, int tileCount,
                                   const std::string& mode, bool waitReports) {
            const uint64_t start = feeder->currentClockCycle;
            P2ExecuteResult run = p2ExecuteKernel(
                kernel, layout.kernels[kernelIdx], tileBegin, tileCount, mode, kernelIdx,
                observer, *feeder, reports, &pause, consumed, watchdog, waitReports);
            const uint64_t end = feeder->currentClockCycle;
            kernelTimings.push_back(P2KernelTiming{
                kernelIdx, p2KernelTypeName(layout.kernels[kernelIdx].ktype),
                start - jobStart, end - jobStart, static_cast<unsigned>(tileCount)});
            producerCycles += run.cycles;
            return run;
        };
        if (workload == "W1")
        {
            const P2ExecuteResult run =
                executeProducer(0, 0, static_cast<int>(nTiles), kernelMode, true);
            timeout = run.timeout;
        }
        else
        {
            P2ExecuteResult addRun =
                executeProducer(0, 0, static_cast<int>(nTiles), kernelMode, false);
            timeout = addRun.timeout;
            if (!pause.acked() && !timeout)
            {
                if (!inputFault)
                    injectFault();
                P2ExecuteResult mulRun =
                    executeProducer(1, 0, static_cast<int>(nTiles), kernelMode, false);
                timeout = timeout || mulRun.timeout;
                if (!pause.acked() && !timeout)
                {
                    P2ExecuteResult reluRun =
                        executeProducer(2, 0, static_cast<int>(nTiles), kernelMode, false);
                    timeout = timeout || reluRun.timeout;
                }
            }
            if (faultKind != "F0" && !pause.acked() && !timeout)
            {
                const P2LoopStats wait =
                    p2RunFeederLoop(*feeder, reports, &pause, consumed, watchdog, true,
                                    true);
                timeout = wait.timeout;
            }
        }
        if (timeout)
            throw std::runtime_error("Phase 2 producer timed out");

        const uint64_t jobEnd = feeder->currentClockCycle;
        const bool haveReport = !reports.reports().empty();
        const bool acked = pause.acked();
        if (haveReport && !acked)
            throw std::runtime_error("Phase 2 report arrived without ACK");
        const uint64_t ackCycle = acked ? pause.ackCycle() : 0;
        const uint64_t requestCycle = pause.requestCycle();
        const uint64_t reportCycle = haveReport ? reports.reports()[0].report_cycle : 0;

        Decision decision;
        bool haveDecision = false;
        bool fallbackFull = false;
        if (haveReport && acked)
        {
            const Phase1PublicReport& report = reports.reports()[0];
            EngineConfig config;
            config.radius_rows = radiusRows;
            config.delta_cycles = deltaCycles;
            config.max_log_entries = maxLogEntries;
            if (windowMode == "DELTA")
                config.window_mode = WindowMode::DELTA;
            else if (windowMode == "BIRTH")
                config.window_mode = WindowMode::BIRTH;
            else
                config.window_mode = WindowMode::AUTO;
            config.c_fix_by_kernel = kernelCFix;
            config.c_tile_by_kernel = kernelCTile;
            RecoveryEngine engine;
            Gate emptyGate;
            if (policy == "B0")
            {
                decision = engine.planWholeJob(ledger, checkpointView);
            }
            else
            {
                if (policy == "B1")
                    config.policy = Policy::B1_SELECTIVE;
                else if (policy == "B2")
                    config.policy = Policy::B2_CLASS_AWARE;
                else if (policy == "B2R")
                    config.policy = Policy::B2R_RADIUS;
                else
                    throw std::runtime_error("unknown policy: " + policy);
                decision = engine.analyze(report, ackCycle, config, ledger, emptyGate,
                                          checkpointView, reports.overflow(), codec);
            }
            haveDecision = true;
            fallbackFull = (decision.status == P2Status::WHOLE_JOB_FALLBACK &&
                            std::find(decision.fallback_reasons.begin(),
                                      decision.fallback_reasons.end(),
                                      "LEDGER_TRUNCATED") !=
                                decision.fallback_reasons.end());
        }

        const std::vector<OpEntry> snapshot(ledger.ops().begin(), ledger.ops().end());
        const uint64_t ledgerBytes = ledger.bytesEstimate();

        uint64_t restoreCycles = 0;
        uint64_t replayCycles = 0;
        uint64_t resumeCycles = 0;
        uint64_t replayTiles = 0;
        uint64_t replayRuns = 0;
        uint64_t coalescedExtra = 0;
        uint64_t publishCycle = 0;
        uint64_t recoveryCycles = 0;
        uint64_t recoveryCyclesExclResume = 0;
        uint64_t readerPartialAtAck = 0;
        uint64_t endToEnd = jobEnd - jobStart;
        uint64_t isolationBytes = 0;
        uint64_t reportsDuringRecovery = 0;
        uint64_t peakHeldItems = 0;
        bool published = false;
        bool hostRead = false;
        bool hostReadDenied = false;
        bool bitExact = false;
        bool recoveryFailed = false;
        unsigned mismatchedTiles = 0;
        std::vector<uint32_t> mismatchIds;
        std::vector<uint32_t> poisonedTiles;
        uint64_t postRecoveryCe = 0;
        std::vector<HostRead> hostReads;

        Gate gate;
        EngineConfig planConfig;
        planConfig.radius_rows = radiusRows;
        planConfig.delta_cycles = deltaCycles;
        planConfig.max_log_entries = maxLogEntries;
        if (windowMode == "DELTA")
            planConfig.window_mode = WindowMode::DELTA;
        else if (windowMode == "BIRTH")
            planConfig.window_mode = WindowMode::BIRTH;
        else
            planConfig.window_mode = WindowMode::AUTO;
        planConfig.c_fix_by_kernel = kernelCFix;
        planConfig.c_tile_by_kernel = kernelCTile;
        for (unsigned tile = 0; tile < nTiles; tile++)
        {
            const uint64_t opId = w2 ? (2ULL * nTiles + tile + 1) : (tile + 1ULL);
            gate.hold(DataRef{static_cast<uint16_t>(w2 ? 6 : 2), tile, opId});
        }
        auto compareTiles = [&]() {
            bitExact = true;
            for (unsigned tile = 0; tile < nTiles; tile++)
            {
                bool tileOk = true;
                for (unsigned b = 0; b < 128; b++)
                {
                    const unsigned index = tile * 128 + b;
                    if (std::memcmp(result[index].u8Data_, expected[index].u8Data_,
                                    sizeof(result[index].u8Data_)) != 0)
                    {
                        tileOk = false;
                        break;
                    }
                }
                if (!tileOk)
                {
                    mismatchedTiles++;
                    mismatchIds.push_back(tile);
                    bitExact = false;
                }
            }
        };

        if (haveDecision)
        {
            RecoveryEngine engine;
            const ExecutionPlan plan = engine.planExecution(decision, ledger, &planConfig, &gate);
            replayTiles = plan.replay_tiles;
            replayRuns = plan.replay_runs.size();
            coalescedExtra = plan.coalesced_extra_tiles;
            for (const DataRef& ref : decision.suspect_items)
            {
                for (uint64_t reader : ledger.readersOf(ref))
                {
                    const OpEntry* op = ledger.find(reader);
                    if (op != nullptr && op->t_issue <= ackCycle && ackCycle < op->t_complete)
                    {
                        readerPartialAtAck = 1;
                        break;
                    }
                }
                if (readerPartialAtAck)
                    break;
            }
            if (decision.status == P2Status::OUTSIDE_WINDOW_NO_TRUSTED_INPUT)
            {
                gate.poison(plan.poison_outputs);
                for (const DataRef& ref : plan.poison_outputs)
                    poisonedTiles.push_back(ref.tile);
                hostReadDenied = true;
            }
            else
            {
                const std::size_t discarded = feeder->clearPending();
                (void)discarded;
                feeder->unfreezeChannel(0);
                PIMRank* pim = (*feeder->channels[0]->ranks)[0]->pimRank.get();
                pim->resetState();

                SimpleRegionIsolationPolicy isolation;
                isolationBytes = isolation.isolate(buildReportFootprint(reports.reports()[0],
                                                                        codec),
                                                   codec);

                const bool skipRestore = (negativeControl == "NO_RESTORE");
                if (!skipRestore)
                {
                    for (const DataRef& ref : decision.restore_list)
                    {
                        if (ref.version != 0)
                            continue;
                        const std::vector<BurstType>* tile =
                            checkpoint.tile(ref.operand, ref.tile);
                        if (tile == nullptr || tile->size() != 128)
                            continue;
                        const uint64_t page = pageRegionId(
                            p2InputRow(layout, ref.operand), ref.tile);
                        for (unsigned i = 0; i < 128; i++)
                            feeder->MultiChannelMemorySystem::addTransaction(
                                true, page * 4096ULL + i * 32ULL,
                                const_cast<BurstType*>(&(*tile)[i]));
                        feeder->MultiChannelMemorySystem::addBarrier(0);
                    }
                    P2LoopStats restore = p2RunDirectWrites(*feeder, watchdog);
                    if (restore.timeout)
                        throw std::runtime_error("Phase 2 restore timed out");
                    restoreCycles += restore.cycles;
                }

                auto recoveryKernel = std::make_shared<PIMKernel>(feeder, 1, 1);
                const bool skipReplay = (negativeControl == "NO_REPLAY");
                const bool wholeJob = (policy == "B0" ||
                                       decision.status == P2Status::WHOLE_JOB_FALLBACK ||
                                       decision.status == P2Status::BASELINE_B0 ||
                                       fallbackFull);
                if (wholeJob)
                {
                    replayTiles = layout.kernels.size() * nTiles;
                    replayRuns = layout.kernels.size();
                    coalescedExtra = 0;
                }
                const std::size_t reportsAtStart = reports.reports().size();
                for (unsigned kernelIdx = 0; kernelIdx < layout.kernels.size(); kernelIdx++)
                {
                    std::set<int> replayTiles;
                    std::set<int> resumeTiles;
                    if (wholeJob)
                    {
                        for (unsigned tile = 0; tile < nTiles; tile++)
                            replayTiles.insert(static_cast<int>(tile));
                    }
                    else
                    {
                        for (const DRAMSim::phase2::Run& run : plan.replay_runs)
                        {
                            if (static_cast<int>(run.kernel_idx) != static_cast<int>(kernelIdx))
                                continue;
                            for (uint32_t tile = run.tile_begin;
                                 tile < run.tile_begin + run.tile_count; tile++)
                                replayTiles.insert(static_cast<int>(tile));
                        }
                        for (uint64_t opId : decision.remaining)
                        {
                            const OpEntry* op = ledger.find(opId);
                            if (op != nullptr && op->kernel_idx == kernelIdx)
                                resumeTiles.insert(static_cast<int>(op->tile));
                        }
                        if (skipReplay)
                            replayTiles.clear();
                    }
                    auto executeTileSet = [&](const std::set<int>& tiles, bool isReplay) {
                        std::vector<int> ordered(tiles.begin(), tiles.end());
                        std::size_t index = 0;
                        while (index < ordered.size())
                        {
                            int count = 1;
                            while (index + count < ordered.size() &&
                                   ordered[index + count] == ordered[index] + count)
                                count++;
                            P2ExecuteResult run = p2ExecuteKernel(
                                recoveryKernel, layout.kernels[kernelIdx], ordered[index],
                                count, "tiles", static_cast<int>(kernelIdx), observer,
                                *feeder, reports, nullptr, consumed, watchdog);
                            if (run.timeout)
                                throw std::runtime_error("Phase 2 recovery timed out");
                            if (isReplay)
                                replayCycles += run.cycles;
                            else
                                resumeCycles += run.cycles;
                            index += count;
                        }
                    };
                    executeTileSet(replayTiles, true);
                    if (!wholeJob)
                        executeTileSet(resumeTiles, false);
                }
                for (const OpEntry& op : snapshot)
                {
                    if (p2Contains(decision.affected, op.op_id))
                    {
                        OpEntry* mutableOp = ledger.findMutable(op.op_id);
                        if (mutableOp != nullptr)
                            mutableOp->is_replay = true;
                    }
                }
                reportsDuringRecovery = reports.reports().size() - reportsAtStart;
                if (reportsDuringRecovery > 0)
                    recoveryFailed = true;

                if (!recoveryFailed)
                {
                    const uint64_t delay = (release == "DEP_RELEASE") ? deltaCycles
                                                                      : holdCycles;
                    publishCycle = feeder->currentClockCycle + delay;
                    for (unsigned tile = 0; tile < nTiles; tile++)
                    {
                        const uint64_t opId = w2 ? (2ULL * nTiles + tile + 1)
                                                 : (tile + 1ULL);
                        const DataRef ref{static_cast<uint16_t>(w2 ? 6 : 2), tile, opId};
                        gate.publish(ref, publishCycle);
                    }
                    published = true;
                    const uint64_t outputReadStart = feeder->currentClockCycle;
                    recoveryKernel->readData(result.data(), dim, layout.result_row, 0);
                    P2LoopStats read = p2RunFeederLoop(*feeder, reports, nullptr, consumed,
                                                       watchdog, false);
                    if (read.timeout)
                        throw std::runtime_error("Phase 2 output read timed out");
                    const uint64_t outputReadEnd = feeder->currentClockCycle;
                    for (unsigned tile_idx = 0; tile_idx < nTiles; tile_idx++)
                    {
                        const uint16_t operand = static_cast<uint16_t>(w2 ? 6 : 2);
                        hostReads.push_back(HostRead{
                            operand, tile_idx,
                            ledger.currentVersion(operand, tile_idx),
                            outputReadStart, outputReadEnd});
                    }
                    hostRead = true;
                    compareTiles();
                    if (inputFault && faultKind != "F0")
                    {
                        std::vector<BurstType> probe(128);
                        const std::size_t beforeProbe = reports.reports().size();
                        const unsigned probeRow =
                            p2InputRow(layout, faultOperand) + tileRowOffset(faultTile);
                        recoveryKernel->readData(probe.data(), 128, probeRow, 0);
                        P2LoopStats probeStats = p2RunFeederLoop(*feeder, reports, nullptr,
                                                                 consumed, watchdog, false);
                        if (probeStats.timeout)
                            throw std::runtime_error("Phase 2 post-recovery read timed out");
                        for (std::size_t i = beforeProbe; i < reports.reports().size(); i++)
                            if (reports.reports()[i].error_class == "CE")
                                postRecoveryCe++;
                    }
                }
                recoveryCycles = publishCycle >= ackCycle ? publishCycle - ackCycle : 0;
                recoveryCyclesExclResume =
                    recoveryCycles >= resumeCycles ? recoveryCycles - resumeCycles : 0;
                endToEnd = publishCycle >= jobStart ? publishCycle - jobStart : 0;
            }
        }
        else
        {
            std::map<DataRef, uint64_t> releaseSchedule;
            if (releaseTest)
            {
                RecoveryEngine releaseEngine;
                if (release == "DEP_RELEASE")
                    releaseSchedule = releaseEngine.depReleaseSchedule(ledger, deltaCycles);
                else
                    releaseSchedule = releaseEngine.globalHoldSchedule(ledger, holdCycles);
                uint64_t latest = jobEnd;
                for (unsigned tile = 0; tile < nTiles; tile++)
                {
                    const uint64_t opId = w2 ? (2ULL * nTiles + tile + 1) : (tile + 1ULL);
                    const DataRef ref{static_cast<uint16_t>(w2 ? 6 : 2), tile, opId};
                    auto found = releaseSchedule.find(ref);
                    const uint64_t releaseCycle =
                        found == releaseSchedule.end() ? jobEnd : found->second;
                    const uint64_t readable = std::max(releaseCycle, jobEnd) + 1;
                    latest = std::max(latest, readable);
                    gate.publish(ref, readable);
                    if (readable > jobEnd)
                        peakHeldItems++;
                }
                publishCycle = latest;
            }
            else
            {
                publishCycle = feeder->currentClockCycle + holdCycles;
                for (unsigned tile = 0; tile < nTiles; tile++)
                {
                    const uint64_t opId = w2 ? (2ULL * nTiles + tile + 1) : (tile + 1ULL);
                    gate.publish(DataRef{static_cast<uint16_t>(w2 ? 6 : 2), tile, opId},
                                 publishCycle);
                }
            }
            published = true;
            auto readKernel = std::make_shared<PIMKernel>(feeder, 1, 1);
            const uint64_t outputReadStart = feeder->currentClockCycle;
            readKernel->readData(result.data(), dim, layout.result_row, 0);
            P2LoopStats read = p2RunFeederLoop(*feeder, reports, nullptr, consumed,
                                               watchdog, false);
            if (read.timeout)
                throw std::runtime_error("Phase 2 no-fault output read timed out");
            const uint64_t outputReadEnd = feeder->currentClockCycle;
            for (unsigned tile_idx = 0; tile_idx < nTiles; tile_idx++)
            {
                const uint16_t operand = static_cast<uint16_t>(w2 ? 6 : 2);
                hostReads.push_back(HostRead{
                    operand, tile_idx,
                    ledger.currentVersion(operand, tile_idx),
                    outputReadStart, outputReadEnd});
            }
            hostRead = true;
            compareTiles();
            endToEnd = publishCycle >= jobStart ? publishCycle - jobStart : 0;
        }

        std::ofstream output( p2Join(outputDir, "output.bin"), std::ios::binary);
        if (hostRead)
            for (const BurstType& burst : result)
                output.write(reinterpret_cast<const char*>(burst.u8Data_),
                             sizeof(burst.u8Data_));
        output.close();
        std::ofstream reference(p2Join(outputDir, "reference.bin"), std::ios::binary);
        for (const BurstType& burst : expected)
            reference.write(reinterpret_cast<const char*>(burst.u8Data_),
                            sizeof(burst.u8Data_));
        reference.close();
        reports.writeCsv(p2Join(outputDir, "reports.csv"));

        std::ofstream layoutOut(p2Join(outputDir, "layout.csv"));
        layoutOut << "operand,base_row\n";
        for (const auto& entry : layout.base_rows)
            layoutOut << entry.first << ',' << entry.second << '\n';
        layoutOut.close();

        std::ofstream timingOut(p2Join(outputDir, "kernel_timing.csv"));
        timingOut << "kernel_idx,kernel_type,t_start,t_end,n_tiles\n";
        for (const P2KernelTiming& timing : kernelTimings)
            timingOut << timing.kernel_idx << ',' << timing.kernel_type << ','
                      << timing.t_start << ',' << timing.t_end << ','
                      << timing.n_tiles << '\n';
        timingOut.close();

        std::ofstream ledgerOut(p2Join(outputDir, "ledger.csv"));
        ledgerOut << "op_id,job_id,kernel_idx,tile,t_issue,t_complete,completed,"
                     "dep_complete,is_replay,inputs,outputs\n";
        for (const OpEntry& op : snapshot)
        {
            const bool replay = haveDecision && p2Contains(decision.affected, op.op_id);
            ledgerOut << op.op_id << ',' << op.job_id << ',' << op.kernel_idx << ','
                      << op.tile << ',' << op.t_issue << ',' << op.t_complete << ','
                      << (op.completed ? "true" : "false") << ','
                      << (op.dep_complete ? "true" : "false") << ','
                      << (replay ? "true" : "false") << ','
                      << p2Refs(op.inputs) << ',' << p2Refs(op.outputs) << '\n';
        }
        ledgerOut.close();

        if (haveDecision)
        {
            const std::string decisionJson = decisionToJson(decision);
            std::ofstream decisionOut(p2Join(outputDir, "decision.json"));
            decisionOut << decisionJson << '\n';
            decisionOut.close();
            std::ofstream inputsOut(p2Join(outputDir, "decision_inputs.json"));
            inputsOut << "{\n"
                      << "  \"t_ack\": " << ackCycle << ",\n"
                      << "  \"delta_cycles\": " << deltaCycles << ",\n"
                      << "  \"report_overflow\": "
                      << p2JsonBool(reports.overflow()) << ",\n"
                      << "  \"ledger_truncated\": " << p2JsonBool(ledger.truncated())
                      << ",\n"
                      << "  \"checkpoint_missing\": ";
            if (checkpointMissing.empty())
                inputsOut << "[]";
            else
            {
                const std::size_t colon = checkpointMissing.find(':');
                inputsOut << "[[" << checkpointMissing.substr(0, colon) << ','
                          << checkpointMissing.substr(colon + 1) << "]]";
            }
            inputsOut << ",\n  \"window_mode\": \"" << windowMode << "\",\n"
                      << "  \"kernel_c_fix\": [";
            for (std::size_t i = 0; i < kernelCFix.size(); i++)
                inputsOut << (i ? "," : "") << kernelCFix[i];
            inputsOut << "],\n  \"kernel_c_tile\": [";
            for (std::size_t i = 0; i < kernelCTile.size(); i++)
                inputsOut << (i ? "," : "") << kernelCTile[i];
            inputsOut << "],\n  \"host_reads\": [";
            for (std::size_t i = 0; i < hostReads.size(); i++)
            {
                if (i != 0) inputsOut << ',';
                inputsOut << "{\"operand\":" << hostReads[i].operand
                          << ",\"tile\":" << hostReads[i].tile
                          << ",\"version\":" << hostReads[i].version
                          << ",\"t_issue\":" << hostReads[i].t_issue
                          << ",\"t_complete\":" << hostReads[i].t_complete << '}';
            }
            inputsOut << "],\n"
                      << "  \"gate_consumed\": [],\n"
                      << "  \"dep_complete_false_ops\": []\n}\n";
            inputsOut.close();
        }

        uint64_t publishWait = 0;
        uint64_t recoveryIncl = 0;
        if (haveDecision && published)
        {
            const uint64_t done = ackCycle + restoreCycles + replayCycles + resumeCycles;
            publishWait = publishCycle > done ? publishCycle - done : 0;
        }
        if (requestCycle > 0 && publishCycle >= requestCycle)
            recoveryIncl = publishCycle - requestCycle;
        const uint64_t faultOverhead =
            (baselineEndToEnd > 0 && endToEnd >= baselineEndToEnd)
                ? endToEnd - baselineEndToEnd : 0;

        std::ofstream metrics(p2Join(outputDir, "metrics.json"));
        metrics << "{\n"
                << "  \"workload\": \"" << workload << "\",\n"
                << "  \"n_tiles\": " << nTiles << ",\n"
                << "  \"seed\": " << seed << ",\n"
                << "  \"granularity\": \"" << granularity << "\",\n"
                << "  \"error_class\": \""
                << (faultKind == "F1" ? "CE" : (faultKind == "F2" ? "UE" : "NONE"))
                << "\",\n"
                << "  \"policy\": \"" << policy << "\",\n"
                << "  \"radius_rows\": " << radiusRows << ",\n"
                << "  \"transport_delay\": " << transportDelay << ",\n"
                << "  \"service_interval\": " << serviceInterval << ",\n"
                << "  \"capacity\": " << capacity << ",\n"
                << "  \"handler_delay\": " << handlerDelay << ",\n"
                << "  \"fault_kind\": \"" << faultKind << "\",\n"
                << "  \"fault_operand\": " << faultOperand << ",\n"
                << "  \"fault_tile\": " << faultTile << ",\n"
                << "  \"fault_bank\": " << faultBank << ",\n"
                << "  \"negative_control\": \"" << negativeControl << "\",\n"
                << "  \"delta_cycles\": " << deltaCycles << ",\n"
                << "  \"release\": \"" << release << "\",\n"
                << "  \"hold_cycles\": " << holdCycles << ",\n"
                << "  \"max_log_entries\": " << maxLogEntries << ",\n"
                << "  \"checkpoint_missing\": \"" << checkpointMissing << "\",\n"
                << "  \"observed_status\": \""
                << (haveDecision ? p2StatusName(decision.status) : "NO_REPORT") << "\",\n"
                << "  \"affected_ops\": " << (haveDecision ? decision.affected.size() : 0)
                << ",\n"
                << "  \"replay_tiles\": " << replayTiles << ",\n"
                << "  \"replay_runs\": " << replayRuns << ",\n"
                << "  \"coalesced_extra_tiles\": " << coalescedExtra << ",\n"
                << "  \"r_read_count\": "
                << (haveDecision ? decision.r_read.size() : 0) << ",\n"
                << "  \"reader_partial_at_ack\": " << readerPartialAtAck << ",\n"
                << "  \"window_mode\": \"" << windowMode << "\",\n"
                << "  \"restore_bytes\": "
                << (haveDecision ? 4096ULL * decision.restore_list.size() : 0) << ",\n"
                << "  \"restore_items\": "
                << (haveDecision ? decision.restore_list.size() : 0) << ",\n"
                << "  \"isolation_bytes\": " << isolationBytes << ",\n"
                << "  \"quiesce_cycles\": " << (acked ? pause.quiesceCycles() : 0) << ",\n"
                << "  \"restore_cycles\": " << restoreCycles << ",\n"
                << "  \"replay_cycles\": " << replayCycles << ",\n"
                << "  \"resume_cycles\": " << resumeCycles << ",\n"
                << "  \"publish_cycles\": " << publishWait << ",\n"
                << "  \"recovery_cycles\": " << recoveryCycles << ",\n"
                << "  \"recovery_cycles_incl_quiesce\": " << recoveryIncl << ",\n"
                << "  \"recovery_cycles_excl_resume\": " << recoveryCyclesExclResume << ",\n"
                << "  \"fault_overhead_cycles\": " << faultOverhead << ",\n"
                << "  \"baseline_end_to_end_cycles\": " << baselineEndToEnd << ",\n"
                << "  \"peak_held_items\": " << peakHeldItems << ",\n"
                << "  \"end_to_end_cycles\": " << endToEnd << ",\n"
                << "  \"producer_cycles\": " << (jobEnd - jobStart) << ",\n"
                << "  \"reports_during_recovery\": " << reportsDuringRecovery << ",\n"
                << "  \"escaped_outputs\": "
                << (haveDecision ? decision.escaped_outputs.size() : 0) << ",\n"
                << "  \"published\": " << p2JsonBool(published) << ",\n"
                << "  \"host_read\": " << p2JsonBool(hostRead) << ",\n"
                << "  \"host_read_denied\": " << p2JsonBool(hostReadDenied) << ",\n"
                << "  \"output_bit_exact\": " << p2JsonBool(bitExact) << ",\n"
                << "  \"mismatch_tiles\": " << mismatchedTiles << ",\n"
                << "  \"mismatch_tile_ids\": \"" << p2TileList(mismatchIds) << "\",\n"
                << "  \"poisoned_tiles\": \"" << p2TileList(poisonedTiles) << "\",\n"
                << "  \"post_recovery_ce_count\": " << postRecoveryCe << ",\n"
                << "  \"ledger_entries\": " << (accountingOn ? snapshot.size() : 0) << ",\n"
                << "  \"ledger_bytes_est\": " << (accountingOn ? ledgerBytes : 0) << ",\n"
                << "  \"ledger_truncated\": " << p2JsonBool(ledger.truncated()) << ",\n"
                << "  \"report_overflow\": " << p2JsonBool(reports.overflow()) << ",\n"
                << "  \"report_count\": " << reports.reports().size() << ",\n"
                << "  \"old_report_cycle\": " << reportCycle << ",\n"
                << "  \"request_cycle\": " << requestCycle << ",\n"
                << "  \"ack_cycle\": " << ackCycle << ",\n"
                << "  \"job_start\": " << jobStart << ",\n"
                << "  \"job_end\": " << jobEnd << ",\n"
                << "  \"publish_cycle\": " << publishCycle << ",\n"
                << "  \"recovery_failed\": " << p2JsonBool(recoveryFailed) << ",\n"
                << "  \"timeout\": false\n"
                << "}\n";
    }
    catch (const std::exception& error)
    {
        std::ofstream metrics(p2Join(outputDir, "metrics.json"));
        metrics << "{\n  \"timeout\": true,\n  \"error\": \"" << error.what() << "\"\n}\n";
        FAIL() << "Phase 2 E2E case failed: " << error.what();
    }
}

TEST(Phase2E2EFixture, SameCycleReleaseCase)
{
    const char* outputPath = std::getenv("PHASE2_OUT");
    if (outputPath == nullptr)
        GTEST_SKIP() << "PHASE2_OUT is not set";
    const std::string outputDir(outputPath);
    p2EnsureDir(outputDir);
    Gate gate;
    const DataRef ref{2, 0, 1};
    gate.hold(ref);
    std::map<DataRef, uint64_t> schedule;
    schedule[ref] = 100;
    gate.freeze();
    gate.releaseDue(100, schedule);
    const bool sameCycleHeld = gate.state(ref) == Lifecycle::HELD;
    gate.releaseDue(99, schedule);
    const bool beforeCycleHeld = gate.state(ref) == Lifecycle::HELD;
    gate.unfreeze();
    gate.releaseDue(101, schedule);
    const bool releasedAfter = gate.state(ref) == Lifecycle::PUBLISHED;
    std::ofstream metrics(p2Join(outputDir, "metrics.json"));
    metrics << "{\n"
            << "  \"timeout\": false,\n"
            << "  \"same_cycle_held\": " << p2JsonBool(sameCycleHeld) << ",\n"
            << "  \"before_cycle_held\": " << p2JsonBool(beforeCycleHeld) << ",\n"
            << "  \"released_after_freeze\": " << p2JsonBool(releasedAfter) << ",\n"
            << "  \"output_bit_exact\": true,\n"
            << "  \"published\": true\n"
            << "}\n";
}

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <utility>
#include <vector>

#include "gtest/gtest.h"

#include "ActivationObserver.h"
#include "Bank.h"
#include "Burst.h"
#include "Phase1Address.h"
#include "Phase1Driver.h"
#include "Phase1Ecc.h"
#include "Phase1Feeder.h"
#include "Phase1Reports.h"
#include "Phase1Storage.h"
#include "Phase2Checkpoint.h"
#include "Phase2Engine.h"
#include "Phase2Gate.h"
#include "Phase2Layout.h"
#include "Phase2Ledger.h"
#include "Phase3Bridge.h"
#include "Rank.h"
#include "ReadDisturbanceModel.h"
#include "RegionMap.h"
#include "tests/PIMKernel.h"

using namespace DRAMSim;

namespace
{

bool isRegularFile(const std::string& path)
{
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool isDirectory(const std::string& path)
{
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

void ensureDir(const std::string& path)
{
    if (path.empty() || path == ".")
        return;
    std::string prefix;
    size_t pos = 0;
    if (path[0] == '/')
    {
        prefix = "/";
        pos = 1;
    }
    while (pos <= path.size())
    {
        const size_t slash = path.find('/', pos);
        const std::string part =
            (slash == std::string::npos) ? path.substr(pos) : path.substr(pos, slash - pos);
        if (!part.empty() && part != ".")
        {
            if (!prefix.empty() && prefix.back() != '/')
                prefix += '/';
            prefix += part;
            if (!isDirectory(prefix))
            {
                if (::mkdir(prefix.c_str(), 0755) != 0 && errno != EEXIST)
                    throw std::runtime_error("cannot create directory: " + prefix);
                if (!isDirectory(prefix))
                    throw std::runtime_error("path exists and is not a directory: " + prefix);
            }
        }
        if (slash == std::string::npos)
            break;
        pos = slash + 1;
    }
}

std::string resolveConfigPath(const std::string& explicitPath,
                              const std::vector<std::string>& fallbacks,
                              const char* label)
{
    if (!explicitPath.empty())
    {
        if (isRegularFile(explicitPath))
            return explicitPath;
        throw std::runtime_error(std::string(label) + " not found: " + explicitPath);
    }
    for (const std::string& candidate : fallbacks)
    {
        if (isRegularFile(candidate))
            return candidate;
    }
    throw std::runtime_error(std::string("cannot resolve ") + label + " from relative fallbacks");
}

std::string resolveDeviceIni(const std::string& explicitPath)
{
    static const std::vector<std::string> kFallbacks = {
        "../../pim-phase1/simulator/ini/HBM2_samsung_2M_16B_x64.ini",
        "../pim-phase1/simulator/ini/HBM2_samsung_2M_16B_x64.ini",
        "pim-phase1/simulator/ini/HBM2_samsung_2M_16B_x64.ini",
    };
    return resolveConfigPath(explicitPath, kFallbacks, "device ini");
}

std::string resolveSystemIni(const std::string& explicitPath)
{
    static const std::vector<std::string> kFallbacks = {
        "../../configs/system_1ch_quiet.ini",
        "configs/system_1ch_quiet.ini",
        "phase3_exp/configs/system_1ch_quiet.ini",
    };
    return resolveConfigPath(explicitPath, kFallbacks, "system ini");
}

std::string envOr(const char* name, const std::string& fallback)
{
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0')
        return fallback;
    return std::string(value);
}

bool envFlag(const char* name)
{
    const std::string val = envOr(name, "");
    return val == "1" || val == "true" || val == "TRUE";
}

std::vector<std::string> readProcCmdline()
{
    std::vector<std::string> args;
    std::ifstream in("/proc/self/cmdline", std::ios::binary);
    if (!in)
        return args;
    std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    size_t start = 0;
    while (start < raw.size())
    {
        const size_t end = raw.find('\0', start);
        const size_t len = (end == std::string::npos) ? (raw.size() - start) : (end - start);
        if (len > 0)
            args.push_back(raw.substr(start, len));
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return args;
}

struct ClosedLoopOptions
{
    std::string deviceIni;
    std::string systemIni;
    std::string outDir = ".";
    std::string workload = "DEC";
    std::string scheme = "OURS";
    std::string attack = "A1";
    double scale = 0.0625;
    uint64_t seed = 20261122ull;
    int index = 0;
    double paraP = 0.01171875;
    std::string swapMode = "v1";
    std::string genMode = "HBM2";
    unsigned regionN = 64u;
    unsigned mgK = 15u;
    uint64_t reportDelay = 32ull;
    std::string reportGranularity = "page";
    std::string hcDist = "truncated_lognormal";
    double vrdMax = 3.5;
    uint64_t attackBudget = 0ull;
    bool disableCeFeedback = false;
    bool disableSwapScrub = false;
    bool corruptEpochMap = false;
    bool disableEpochCheck = false;
    bool hasMarginG = false;
    double marginG = 1.1;
    bool disableDetector = false;
    bool runCell = false;

    double effectiveMarginG() const
    {
        if (hasMarginG)
            return marginG;
        if (scheme == "OURS" || (scheme.compare(0, 5, "OURS-") == 0 && scheme != "OURS-G1.5"))
            return 1.1;
        return 1.5;
    }

    double effectiveParaP() const
    {
        if (scheme == "PARA" && scale < 1.0 && scale > 0.0)
            return std::min(1.0, paraP / scale);
        return paraP;
    }

    bool effectiveScrub() const
    {
        if (disableSwapScrub)
            return false;
        if (scheme == "NONE" || scheme == "REACT" || scheme == "PARA" || scheme == "OURS-SCRUB")
            return false;
        return true;
    }

    bool effectiveCeFeedback() const
    {
        if (disableCeFeedback)
            return false;
        const bool isOurs = (scheme == "OURS" || scheme.compare(0, 5, "OURS-") == 0);
        return isOurs && scheme != "OURS-CE";
    }

    bool effectiveDetector() const
    {
        if (disableDetector)
            return false;
        const bool isOurs = (scheme == "OURS" || scheme.compare(0, 5, "OURS-") == 0);
        return scheme == "ADA" || (isOurs && scheme != "OURS-DET");
    }

    bool effectiveEpochCheck() const
    {
        if (disableEpochCheck || scheme == "OURS-EPOCH")
            return false;
        return true;
    }
};

void validateOptions(const ClosedLoopOptions& opts)
{
    if (opts.workload != "W2" && opts.workload != "DEC")
        throw std::invalid_argument("workload must be W2 or DEC");
    const bool validScheme =
        opts.scheme == "NONE" || opts.scheme == "REACT" || opts.scheme == "PARA" ||
        opts.scheme == "FIX" || opts.scheme == "ADA" || opts.scheme == "OURS" ||
        opts.scheme == "OURS-CE" || opts.scheme == "OURS-SCRUB" || opts.scheme == "OURS-EPOCH" ||
        opts.scheme == "OURS-G1.5" || opts.scheme == "OURS-DET";
    if (!validScheme)
        throw std::invalid_argument("unsupported scheme: " + opts.scheme);
    if (opts.attack != "NO_ATK" && opts.attack != "A1" && opts.attack != "A2" &&
        opts.attack != "A3" && opts.attack != "A4")
        throw std::invalid_argument("unsupported attack: " + opts.attack);
    if (!(opts.scale > 0.0 && opts.scale <= 1.0))
        throw std::invalid_argument("scale must be in (0, 1]");
    if (opts.swapMode != "v1" && opts.swapMode != "v2")
        throw std::invalid_argument("swap-mode must be v1 or v2");
    if (opts.genMode != "HBM2" && opts.genMode != "HBM3_STYLE")
        throw std::invalid_argument("gen-mode must be HBM2 or HBM3_STYLE");
    if (opts.reportGranularity != "page" && opts.reportGranularity != "codeword" &&
        opts.reportGranularity != "bank")
        throw std::invalid_argument("report-granularity must be page, codeword, or bank");
    if (opts.hcDist != "truncated_lognormal" && opts.hcDist != "uniform" &&
        opts.hcDist != "shifted_exponential")
        throw std::invalid_argument("unsupported hc-dist: " + opts.hcDist);
    if (opts.regionN == 0u || opts.mgK == 0u)
        throw std::invalid_argument("region-n and mg-k must be positive");
    if (opts.vrdMax < 1.0)
        throw std::invalid_argument("vrd-max must be >= 1.0");
}

ClosedLoopOptions parseClosedLoopOptions()
{
    ClosedLoopOptions opts;
    opts.deviceIni = envOr("PHASE3_DEVICE_INI", "");
    opts.systemIni = envOr("PHASE3_SYSTEM_INI", "");
    opts.outDir = envOr("PHASE3_OUT", ".");
    opts.workload = envOr("PHASE3_WORKLOAD", "DEC");
    opts.scheme = envOr("PHASE3_SCHEME", "OURS");
    opts.attack = envOr("PHASE3_ATTACK", "A1");
    opts.scale = std::strtod(envOr("PHASE3_SCALE", "0.0625").c_str(), nullptr);
    opts.seed = std::strtoull(envOr("PHASE3_SEED", "20261122").c_str(), nullptr, 10);
    opts.index = std::atoi(envOr("PHASE3_INDEX", "0").c_str());
    opts.paraP = std::strtod(envOr("PHASE3_PARA_P", "0.01171875").c_str(), nullptr);
    opts.swapMode = envOr("PHASE3_SWAP_MODE", "v1");
    opts.genMode = envOr("PHASE3_GEN_MODE", "HBM2");
    opts.regionN = static_cast<unsigned>(std::strtoul(envOr("PHASE3_REGION_N", "64").c_str(), nullptr, 10));
    opts.mgK = static_cast<unsigned>(std::strtoul(envOr("PHASE3_MG_K", "15").c_str(), nullptr, 10));
    opts.reportDelay = std::strtoull(envOr("PHASE3_REPORT_DELAY", "32").c_str(), nullptr, 10);
    opts.reportGranularity = envOr("PHASE3_REPORT_GRANULARITY", "page");
    opts.hcDist = envOr("PHASE3_HC_DIST", "truncated_lognormal");
    opts.vrdMax = std::strtod(envOr("PHASE3_VRD_MAX", "3.5").c_str(), nullptr);
    opts.attackBudget = std::strtoull(envOr("PHASE3_ATTACK_BUDGET", "0").c_str(), nullptr, 10);
    opts.disableCeFeedback = envFlag("PHASE3_DISABLE_CE_FEEDBACK");
    opts.disableSwapScrub = envFlag("PHASE3_DISABLE_SWAP_SCRUB");
    opts.corruptEpochMap = envFlag("PHASE3_CORRUPT_EPOCH_MAP");
    opts.disableEpochCheck = envFlag("PHASE3_DISABLE_EPOCH_CHECK");
    opts.disableDetector = envFlag("PHASE3_DISABLE_DETECTOR");
    const std::string envMargin = envOr("PHASE3_MARGIN_G", "");
    if (!envMargin.empty())
    {
        opts.hasMarginG = true;
        opts.marginG = std::strtod(envMargin.c_str(), nullptr);
    }
    opts.runCell = envOr("PHASE3_CL_RUN", "") == "cell";

    const std::vector<std::string> args = readProcCmdline();
    for (size_t i = 1; i < args.size(); ++i)
    {
        const std::string& raw = args[i];
        if (raw.compare(0, 2, "--") != 0)
            continue;
        const size_t eq = raw.find('=');
        const std::string flag = (eq == std::string::npos) ? raw : raw.substr(0, eq);
        const bool hasEq = (eq != std::string::npos);
        const std::string inlineVal = hasEq ? raw.substr(eq + 1) : "";

        auto needValue = [&]() -> std::string {
            if (hasEq)
                return inlineVal;
            if (i + 1 >= args.size())
                throw std::invalid_argument("missing value for CLI flag: " + flag);
            return args[++i];
        };

        if (flag == "--device-ini")
            opts.deviceIni = needValue();
        else if (flag == "--system-ini")
            opts.systemIni = needValue();
        else if (flag == "--out")
            opts.outDir = needValue();
        else if (flag == "--workload")
            opts.workload = needValue();
        else if (flag == "--scheme")
            opts.scheme = needValue();
        else if (flag == "--attack")
            opts.attack = needValue();
        else if (flag == "--scale")
            opts.scale = std::strtod(needValue().c_str(), nullptr);
        else if (flag == "--seed")
            opts.seed = std::strtoull(needValue().c_str(), nullptr, 10);
        else if (flag == "--index")
            opts.index = std::atoi(needValue().c_str());
        else if (flag == "--para-p")
            opts.paraP = std::strtod(needValue().c_str(), nullptr);
        else if (flag == "--swap-mode")
            opts.swapMode = needValue();
        else if (flag == "--gen-mode")
            opts.genMode = needValue();
        else if (flag == "--region-n")
            opts.regionN = static_cast<unsigned>(std::strtoul(needValue().c_str(), nullptr, 10));
        else if (flag == "--mg-k")
            opts.mgK = static_cast<unsigned>(std::strtoul(needValue().c_str(), nullptr, 10));
        else if (flag == "--report-delay")
            opts.reportDelay = std::strtoull(needValue().c_str(), nullptr, 10);
        else if (flag == "--report-granularity")
            opts.reportGranularity = needValue();
        else if (flag == "--hc-dist")
            opts.hcDist = needValue();
        else if (flag == "--vrd-max")
            opts.vrdMax = std::strtod(needValue().c_str(), nullptr);
        else if (flag == "--attack-budget")
            opts.attackBudget = std::strtoull(needValue().c_str(), nullptr, 10);
        else if (flag == "--disable-ce-feedback")
            opts.disableCeFeedback = true;
        else if (flag == "--disable-swap-scrub")
            opts.disableSwapScrub = true;
        else if (flag == "--corrupt-epoch-map")
            opts.corruptEpochMap = true;
        else if (flag == "--disable-epoch-check")
            opts.disableEpochCheck = true;
        else if (flag == "--margin-g")
        {
            opts.hasMarginG = true;
            opts.marginG = std::strtod(needValue().c_str(), nullptr);
        }
        else if (flag == "--disable-detector")
            opts.disableDetector = true;
        else if (flag == "--cl-cell")
            opts.runCell = true;
    }

    validateOptions(opts);
    return opts;
}

uint64_t bridgeEpoch(unsigned bank, unsigned row, void* context)
{
    const RegionMap* map = static_cast<const RegionMap*>(context);
    (void)bank;
    if (row >= RegionMap::kRows)
    {
        uint64_t best = 0;
        const unsigned nRegions = RegionMap::kRows / map->regionRows();
        for (unsigned region = 0; region < nRegions; ++region)
            best = std::max(best, map->epoch(region));
        return best;
    }
    return map->epoch(map->logical(row) / map->regionRows());
}

unsigned bridgeLogical(unsigned bank, unsigned row, void* context)
{
    (void)bank;
    if (row >= RegionMap::kRows)
        return row;
    return static_cast<const RegionMap*>(context)->logical(row);
}

unsigned bridgePhysical(unsigned bank, unsigned row, void* context)
{
    (void)bank;
    if (row >= RegionMap::kRows)
        return row;
    return static_cast<const RegionMap*>(context)->physical(row);
}

void bridgeCe(unsigned bank, unsigned row, void* context)
{
    static_cast<RegionMap*>(context)->noteCe(bank, row);
}

bool injectCodewordRank(unsigned bank, unsigned row, unsigned burstColumn, unsigned codeword,
                        unsigned bit, bool parity, void* context)
{
    DRAMSim::Rank* rank = static_cast<DRAMSim::Rank*>(context);
    if (rank == nullptr || bank >= rank->banks.size() || codeword >= 4u)
        return false;
    const DRAMSim::Phase1StorageKey key{0, 0, bank, row, burstColumn, codeword};
    if (!rank->banks[bank].phase1Storage().contains(key))
        return false;
    try
    {
        if (parity)
            rank->banks[bank].injectRawParityBit(row, burstColumn, codeword, bit);
        else
            rank->banks[bank].injectRawDataBit(row, burstColumn, codeword * 64u + bit);
    }
    catch (const std::exception&)
    {
        return false;
    }
    return true;
}

bool injectCodewordStorage(unsigned bank, unsigned row, unsigned burstColumn, unsigned codeword,
                           unsigned bit, bool parity, void* context)
{
    DRAMSim::Phase1Storage* storage = static_cast<DRAMSim::Phase1Storage*>(context);
    if (storage == nullptr || codeword >= 4u)
        return false;
    const DRAMSim::Phase1StorageKey key{0, 0, bank, row, burstColumn, codeword};
    if (!storage->contains(key))
        return false;
    if (parity)
        storage->injectParityBit(key, bit);
    else
        storage->injectDataBit(key, bit);
    return true;
}

struct LiveSwapContext
{
    DRAMSim::Rank* rank = nullptr;
    ReadDisturbanceModel* model = nullptr;
    RegionMap* region = nullptr;
    std::vector<std::pair<unsigned, unsigned>>* moves = nullptr;
    bool swapScrub = true;
    unsigned numCols = 32u;
};

void releaseScrubbedCodewords(DRAMSim::Bank& bankObj, ReadDisturbanceModel* model,
                              unsigned bank, unsigned row, unsigned numCols)
{
    if (model == nullptr)
        return;
    const DRAMSim::Phase1Storage& st = bankObj.phase1Storage();
    for (unsigned col = 0; col < numCols; ++col)
    {
        for (unsigned cw = 0; cw < 4u; ++cw)
        {
            const DRAMSim::Phase1StorageKey key{0, 0, bank, row, col, cw};
            if (st.contains(key) && st.readProtected(key).status == DRAMSim::Phase1EccStatus::CORRECTED)
                model->releaseCodewordBits(bank, row, col, cw);
        }
    }
}

void scrubLive(unsigned bank, unsigned source, unsigned dest, void* context)
{
    LiveSwapContext* live = static_cast<LiveSwapContext*>(context);
    if (live == nullptr)
        return;
    if (live->rank != nullptr && bank < live->rank->banks.size())
    {
        if (live->swapScrub && live->model != nullptr)
        {
            releaseScrubbedCodewords(live->rank->banks[bank], live->model, bank, source, live->numCols);
            if (dest != source)
                releaseScrubbedCodewords(live->rank->banks[bank], live->model, bank, dest, live->numCols);
        }
        live->rank->banks[bank].exchangeRows(source, dest);
    }
    if (bank == 0 && live->moves != nullptr)
        live->moves->push_back(std::make_pair(source, dest));
}

bool refreshPending(const DRAMSim::Phase1FeederMemory& feeder)
{
    for (DRAMSim::MemorySystem* channel : feeder.channels)
    {
        if (channel == nullptr || channel->ranks == nullptr)
            continue;
        for (DRAMSim::Rank* rank : *channel->ranks)
        {
            if (rank != nullptr && rank->refreshWaiting)
                return true;
        }
    }
    return false;
}

uint64_t drain(DRAMSim::Phase1FeederMemory& feeder, uint64_t watchdog)
{
    uint64_t cycles = 0;
    while (feeder.pendingCount() > 0 || !DRAMSim::Phase1Driver::directIdle(feeder) ||
           refreshPending(feeder))
    {
        if (cycles >= watchdog)
            throw std::runtime_error("closed-loop drain watchdog exceeded");
        feeder.tick();
        feeder.update();
        cycles += 1;
    }
    return cycles;
}

uint64_t drainReports(DRAMSim::Phase1FeederMemory& feeder, DRAMSim::Phase1Reports& reports,
                      uint64_t watchdog)
{
    uint64_t cycles = 0;
    while (feeder.pendingCount() > 0 || !DRAMSim::Phase1Driver::directIdle(feeder) ||
           refreshPending(feeder) || reports.pendingCount() > 0)
    {
        if (cycles >= watchdog)
            throw std::runtime_error("closed-loop report watchdog exceeded");
        feeder.tick();
        feeder.update();
        reports.tick(feeder.currentClockCycle);
        cycles += 1;
    }
    return cycles;
}

uint64_t hbm2ScrubCycles(uint64_t codewordWrites)
{
    return ((codewordWrites + 3ull) / 4ull) * 4ull;
}

struct ObserverDetacher
{
    ActivationObserver* observer = nullptr;
    ReadDisturbanceModel* model = nullptr;
    RegionMap* region = nullptr;

    ObserverDetacher(ActivationObserver* obs, ReadDisturbanceModel* m, RegionMap* r)
        : observer(obs), model(m), region(r)
    {
        if (observer != nullptr)
        {
            observer->attachDisturbance(nullptr);
            observer->attachRegion(nullptr);
        }
    }

    ~ObserverDetacher()
    {
        if (observer != nullptr)
        {
            observer->attachDisturbance(model);
            observer->attachRegion(region);
        }
    }
};

struct ClosedLoopSession
{
    std::shared_ptr<DRAMSim::Phase1FeederMemory> feeder;
    ActivationObserver observer;
    ReadDisturbanceModel model;
    RegionMap region;
    DRAMSim::Phase1Address codec;
    std::shared_ptr<PIMKernel> kernel;
    DRAMSim::Rank* rank = nullptr;
    LiveSwapContext live;
    std::vector<std::pair<unsigned, unsigned>> moves;
    std::string swapMode = "v1";
    bool scrub = false;
    bool ceFeedback = false;
    bool detector = false;
    bool epochCheck = true;
    uint64_t swapCycles = 0;
    std::size_t billedMoves = 0;
    uint64_t paraCycles = 0;
    uint64_t billedParaSb = 0;
    uint64_t billedParaAb = 0;

    ClosedLoopSession(const std::string& deviceIni, const std::string& systemIni,
                      const std::string& outDir, const ClosedLoopOptions& opts)
        : feeder(std::make_shared<DRAMSim::Phase1FeederMemory>(deviceIni, systemIni, outDir,
                                                              "phase3_cl", 256, 256)),
          observer(0),
          codec(DRAMSim::Phase1Address::fromConfiguration()),
          swapMode(opts.swapMode),
          scrub(opts.effectiveScrub()),
          ceFeedback(opts.effectiveCeFeedback()),
          detector(opts.effectiveDetector()),
          epochCheck(opts.effectiveEpochCheck())
    {
        feeder->setCPUClockSpeed(0);
        for (DRAMSim::MemorySystem* channel : feeder->channels)
        {
            if (channel == nullptr || channel->ranks == nullptr)
                continue;
            for (DRAMSim::Rank* one : *channel->ranks)
            {
                rank = one;
                phase3Attach(one, &observer);
            }
        }
        if (rank == nullptr)
            throw std::runtime_error("closed-loop session did not attach a DRAMSim::Rank");

        const double g = opts.effectiveMarginG();
        region.configure(opts.scheme, g, 2.0, opts.scale);
        region.setSeed(opts.seed);
        if (opts.regionN != 64u || opts.mgK != 15u)
            region.setGeometry(opts.regionN, opts.mgK);
        if (opts.hasMarginG)
            region.setMarginG(g, 2.0, opts.scale);
        region.setScrubEnabled(scrub);
        region.setCeFeedback(ceFeedback);
        if (!detector)
        {
            region.setDetectorEnabled(false, false);
        }
        else
        {
            region.setDetectorEnabled(true, false);
        }

        if (!epochCheck)
        {
            region.setEpochCorrupt(opts.corruptEpochMap);
            region.setEpochCheck(false);
            phase3SetLateReportTranslation(true);
            phase3SetReportEpochRelaxed(true);
        }
        else
        {
            region.setEpochCorrupt(opts.corruptEpochMap);
            region.setEpochCheck(true);
            phase3SetLateReportTranslation(false);
            phase3SetReportEpochRelaxed(true);
        }

        region.setParaProbability(opts.effectiveParaP());
        region.setDeferSwaps(true);

        model.configure(opts.seed, opts.scale, opts.genMode);
        model.setDistribution(opts.hcDist, opts.vrdMax);
        model.setCodewordInjector(&injectCodewordRank, rank);

        observer.attachDisturbance(&model);
        observer.attachRegion(&region);

        live.rank = rank;
        live.model = &model;
        live.region = &region;
        live.moves = &moves;
        live.swapScrub = scrub;
        live.numCols = codec.geometry().burst_columns();
        region.setScrubber(&scrubLive, &live);

        phase3SetAddressHook(&bridgeEpoch, &bridgeLogical, &region);
        phase3SetPhysicalHook(&bridgePhysical, &region);
        phase3SetCeHook(&bridgeCe, &region);
        kernel = std::make_shared<PIMKernel>(feeder, 1, 1);
    }

    ~ClosedLoopSession()
    {
        phase3SetAddressHook(nullptr, nullptr, nullptr);
        phase3SetPhysicalHook(nullptr, nullptr);
        phase3SetCeHook(nullptr, nullptr);
        phase3SetLateReportTranslation(false);
        phase3SetReportEpochRelaxed(false);
        phase3ClearPublication();
        phase3DetachAll();
    }

    void billMoves(uint64_t& cycles)
    {
        if (moves.size() == billedMoves)
            return;
        const std::size_t freshCount = moves.size() - billedMoves;
        if (swapMode == "v1")
        {
            const uint64_t added = static_cast<uint64_t>(freshCount) * 4ull * 47ull;
            billedMoves = moves.size();
            cycles += added;
            swapCycles += added;
            return;
        }
        // v2: issue real controller-queued SB row-swap bursts across the 4 parallel bank groups
        // (banks 0, 4, 8, 12 interleaved per column so bank groups overlap within queue depth)
        // using unreserved rows (< 8192), draining per swap pair.
        ObserverDetacher guard(&observer, &model, &region);
        DRAMSim::BurstType scratchBurst;
        const unsigned cols = codec.geometry().burst_columns();
        uint64_t added = 0;
        for (std::size_t idx = billedMoves; idx < moves.size(); ++idx)
        {
            const unsigned sRow = 7000u + static_cast<unsigned>((idx * 2u) % 256u);
            const unsigned dRow = sRow + 1u;
            for (unsigned col = 0; col < cols; ++col)
            {
                for (unsigned b = 0; b < 4u; ++b)
                    feeder->addTransaction(false, codec.encode(DRAMSim::Phase1AddrTuple{0, 0, b * 4u, sRow, col}),
                                           "SWAP_MOV", &scratchBurst);
            }
            for (unsigned col = 0; col < cols; ++col)
            {
                for (unsigned b = 0; b < 4u; ++b)
                    feeder->addTransaction(false, codec.encode(DRAMSim::Phase1AddrTuple{0, 0, b * 4u, dRow, col}),
                                           "SWAP_MOV", &scratchBurst);
            }
            for (unsigned col = 0; col < cols; ++col)
            {
                for (unsigned b = 0; b < 4u; ++b)
                    feeder->addTransaction(true, codec.encode(DRAMSim::Phase1AddrTuple{0, 0, b * 4u, dRow, col}),
                                           "SWAP_MOV", &scratchBurst);
            }
            for (unsigned col = 0; col < cols; ++col)
            {
                for (unsigned b = 0; b < 4u; ++b)
                    feeder->addTransaction(true, codec.encode(DRAMSim::Phase1AddrTuple{0, 0, b * 4u, sRow, col}),
                                           "SWAP_MOV", &scratchBurst);
            }
            added += drain(*feeder, 16000000ull);
        }
        billedMoves = moves.size();
        cycles += added;
        swapCycles += added;
    }

    void billPara(uint64_t& cycles)
    {
        const uint64_t freshSb = region.paraSbCommands() - billedParaSb;
        const uint64_t freshAb = region.paraAbCommands() - billedParaAb;
        if (freshSb == 0 && freshAb == 0)
            return;
        billedParaSb = region.paraSbCommands();
        billedParaAb = region.paraAbCommands();
        if (swapMode == "v1")
        {
            const uint64_t added = freshSb * 2ull * 47ull + freshAb * 4ull * 47ull;
            cycles += added;
            paraCycles += added;
            return;
        }
        ObserverDetacher guard(&observer, &model, &region);
        DRAMSim::BurstType scratchBurst;
        uint64_t added = 0;
        uint64_t queuedTriggers = 0;
        for (uint64_t k = 0; k < freshSb; ++k)
        {
            const unsigned row = 7260u + static_cast<unsigned>((k * 2u) % 64u);
            feeder->addTransaction(false, codec.encode(DRAMSim::Phase1AddrTuple{0, 0, 0, row, 0}),
                                   "PARA_REF", &scratchBurst);
            feeder->addTransaction(false,
                                   codec.encode(DRAMSim::Phase1AddrTuple{0, 0, 0, row + 1u, 0}),
                                   "PARA_REF", &scratchBurst);
            queuedTriggers += 1;
            if (queuedTriggers % 16ull == 0ull)
                added += drain(*feeder, 16000000ull);
        }
        for (uint64_t k = 0; k < freshAb; ++k)
        {
            const unsigned row = 7260u + static_cast<unsigned>((k * 2u) % 64u);
            for (unsigned b = 0; b < 4u; ++b)
            {
                for (unsigned sub = 0; sub < 2u; ++sub)
                {
                    const unsigned bank = b * 4u + sub;
                    feeder->addTransaction(
                        false, codec.encode(DRAMSim::Phase1AddrTuple{0, 0, bank, row, 0}),
                        "PARA_REF", &scratchBurst);
                    feeder->addTransaction(
                        false, codec.encode(DRAMSim::Phase1AddrTuple{0, 0, bank, row + 1u, 0}),
                        "PARA_REF", &scratchBurst);
                }
            }
            queuedTriggers += 1;
            if (queuedTriggers % 16ull == 0ull)
                added += drain(*feeder, 16000000ull);
        }
        if (queuedTriggers % 16ull != 0ull)
            added += drain(*feeder, 16000000ull);
        cycles += added;
        paraCycles += added;
    }

    void flushSwaps(uint64_t& cycles)
    {
        region.flush(&model);
        billMoves(cycles);
        billPara(cycles);
    }
};

BurstType positiveBurst(unsigned index, unsigned tag)
{
    BurstType burst;
    for (int lane = 0; lane < 16; ++lane)
    {
        const float value = static_cast<float>(1 + ((index + tag + static_cast<unsigned>(lane)) % 5));
        burst.fp16Data_[lane] = convertF2H(value);
    }
    return burst;
}

BurstType applyRelu(BurstType value)
{
    for (int lane = 0; lane < 16; ++lane)
        value.u16Data_[lane] = (value.u16Data_[lane] & 0x8000u) ? 0 : value.u16Data_[lane];
    return value;
}

unsigned countBurstDiff(const std::vector<BurstType>& left, const std::vector<BurstType>& right)
{
    unsigned diff = 0;
    const size_t n = std::min(left.size(), right.size());
    for (size_t i = 0; i < n; ++i)
    {
        if (std::memcmp(left[i].u8Data_, right[i].u8Data_, 32) != 0)
            diff += 1;
    }
    diff += static_cast<unsigned>(left.size() > right.size() ? left.size() - right.size()
                                                             : right.size() - left.size());
    return diff;
}

unsigned countBurstBits(const std::vector<BurstType>& left, const std::vector<BurstType>& right)
{
    unsigned bits = 0;
    const size_t n = std::min(left.size(), right.size());
    for (size_t i = 0; i < n; ++i)
    {
        for (int byte = 0; byte < 32; ++byte)
        {
            bits += static_cast<unsigned>(__builtin_popcount(
                static_cast<unsigned>(left[i].u8Data_[byte] ^ right[i].u8Data_[byte])));
        }
    }
    return bits;
}

void preloadRow(PIMKernel& kernel, const std::vector<BurstType>& data, unsigned row,
                unsigned startingCol = 0)
{
    NumpyBurstType operand;
    operand.bShape.push_back(data.size());
    operand.bData = data;
    kernel.preloadNoReplacement(&operand, row, startingCol);
}

void enqueueReadCol(DRAMSim::Phase1FeederMemory& feeder, const DRAMSim::Phase1Address& codec,
                    unsigned bank, unsigned row, unsigned col, DRAMSim::BurstType& slot,
                    const char* tag)
{
    feeder.addTransaction(false, codec.encode(DRAMSim::Phase1AddrTuple{0, 0, bank, row, col}), tag,
                          &slot);
}

void enqueueWriteCol(DRAMSim::Phase1FeederMemory& feeder, const DRAMSim::Phase1Address& codec,
                     unsigned bank, unsigned row, unsigned col, DRAMSim::BurstType& data,
                     const char* tag)
{
    feeder.addTransaction(true, codec.encode(DRAMSim::Phase1AddrTuple{0, 0, bank, row, col}), tag,
                          &data);
}

std::vector<BurstType> readRow(ClosedLoopSession& session, DRAMSim::Phase1Reports& reports,
                               unsigned row, size_t bursts, uint64_t& cycles)
{
    std::vector<BurstType> out(bursts);
    session.kernel->readData(out.data(), bursts, row, 0);
    cycles += drainReports(*session.feeder, reports, 8000000ull);
    return out;
}

struct KernelDesc
{
    uint16_t index = 0;
    KernelType type = KernelType::ADD;
    int input0 = 0;
    int input1 = 0;
    int output = 0;
    uint16_t in0 = 0;
    uint16_t in1 = 0;
    bool hasSecond = false;
    uint16_t out = 0;
};

struct ClosedLoopResult
{
    std::string status = "SUCCESS";
    std::string error;
    std::string recovery = "NONE";
    std::string phase2Status;
    uint64_t cycles = 0;
    uint64_t pimItems = 0;
    uint64_t usefulPimItems = 0;
    uint64_t replayPimItems = 0;
    uint64_t hostTx = 0;
    uint64_t attackTx = 0;
    uint64_t attackBudget = 0;
    uint64_t flips = 0;
    uint64_t latent = 0;
    uint64_t ce = 0;
    uint64_t ue = 0;
    uint64_t onDieMaskedCe = 0;
    uint64_t scrubWrites = 0;
    uint64_t scrubCycles = 0;
    uint64_t swapCycles = 0;
    uint64_t paraCycles = 0;
    uint64_t restoredTiles = 0;
    uint64_t modelScrubs = 0;
    unsigned computeErrors = 0;
    unsigned committedMismatch = 0;
    unsigned misrecovered = 0;
    unsigned level = 0;
    unsigned scoreRow = 0;
    unsigned attackRow = 0;
    unsigned physicalVictim = 0;
    unsigned physicalVictimEnd = 0;
    unsigned visibleBits = 0;
    uint64_t crossings = 0;
    uint64_t unplaced = 0;
    uint64_t restoredMask = 0;
    bool bitExact = false;
    bool replayed = false;
    bool withheld = false;
    bool recoveryFailed = false;
    bool sawL2 = false;
    bool erroneousCommit = false;
    bool refused = false;
    bool directInject = false;
};

uint64_t computeAttackBudget(const ClosedLoopOptions& opts)
{
    if (opts.attack == "NO_ATK")
        return 0ull;
    if (opts.attackBudget > 0ull)
        return opts.attackBudget;
    const double baseHc = 14531.0 * opts.scale;
    if (opts.attack == "A1" || opts.attack == "A4")
        return static_cast<uint64_t>(baseHc * 14.4) + 128ull;
    if (opts.attack == "A2")
        return static_cast<uint64_t>(baseHc * 80.0) + 128ull;
    if (opts.scale > 0.5)
        return static_cast<uint64_t>(baseHc * 48.0) + 136ull;
    return static_cast<uint64_t>(baseHc * (2.40 + 3.05 + 3.75 + 4.45) * 17.0) + 136ull;
}

unsigned selectAggressor(const ClosedLoopOptions& opts, ClosedLoopSession& session,
                         unsigned logicalVictim, unsigned physicalVictim, uint64_t step)
{
    (void)logicalVictim;
    unsigned aggressor = physicalVictim > 0 ? physicalVictim - 1u : physicalVictim + 1u;
    if (opts.attack == "A1")
    {
        aggressor = (step % 2ull == 0ull) ? (physicalVictim > 0 ? physicalVictim - 1u : physicalVictim + 1u)
                                          : (physicalVictim + 1u < RegionMap::kRows ? physicalVictim + 1u
                                                                                    : physicalVictim - 1u);
    }
    else if (opts.attack == "A2")
    {
        const unsigned pair = static_cast<unsigned>(step % 8ull);
        const unsigned side = static_cast<unsigned>((step / 8ull) % 2ull);
        unsigned base = physicalVictim + pair * 4u;
        if (base + 1u >= RegionMap::kRows)
            base = physicalVictim - pair * 4u;
        aggressor = (side == 0u) ? (base > 0 ? base - 1u : base + 1u) : (base + 1u);
    }
    else if (opts.attack == "A3")
    {
        const unsigned slot = static_cast<unsigned>(step % 17ull);
        const unsigned vicRegion = physicalVictim / opts.regionN;
        if (slot == 0u)
        {
            if (physicalVictim > 0u && (physicalVictim - 1u) / opts.regionN != vicRegion)
                aggressor = physicalVictim - 1u;
            else if (physicalVictim + 1u < RegionMap::kRows &&
                     (physicalVictim + 1u) / opts.regionN != vicRegion)
                aggressor = physicalVictim + 1u;
            else
                aggressor = ((step / 17ull) % 2ull == 0ull)
                                ? (physicalVictim > 0u ? physicalVictim - 1u : physicalVictim + 1u)
                                : (physicalVictim + 1u < RegionMap::kRows ? physicalVictim + 1u
                                                                          : physicalVictim - 1u);
        }
        else
        {
            const unsigned decoyBase = 6144u;
            aggressor = decoyBase + ((slot - 1u) % opts.regionN);
        }
    }
    else if (opts.attack == "A4")
    {
        unsigned pointed = session.region.migrationSource(logicalVictim / opts.regionN);
        if (pointed == 0u)
            pointed = 1u;
        if (pointed + 1u >= RegionMap::kRows)
            pointed = RegionMap::kRows - 2u;
        aggressor = (step % 2ull == 0ull) ? (pointed - 1u) : (pointed + 1u);
    }
    if (aggressor == 0u || aggressor >= RegionMap::kRows)
        aggressor = physicalVictim > 0 ? physicalVictim - 1u : physicalVictim + 1u;
    return aggressor;
}

uint64_t countLatentFaults(const DRAMSim::Rank& rank, const RegionMap& region,
                           const std::vector<unsigned>& logicalRows, unsigned numCols)
{
    std::set<unsigned> physRows;
    for (unsigned lrow : logicalRows)
    {
        physRows.insert(lrow);
        physRows.insert(region.physical(lrow));
    }
    uint64_t latent = 0;
    for (unsigned bank = 0; bank < rank.banks.size(); ++bank)
    {
        const DRAMSim::Phase1Storage& st = rank.banks[bank].phase1Storage();
        for (unsigned row : physRows)
        {
            for (unsigned col = 0; col < numCols; ++col)
            {
                for (unsigned cw = 0; cw < 4u; ++cw)
                {
                    const DRAMSim::Phase1StorageKey key{0, 0, bank, row, col, cw};
                    if (!st.contains(key))
                        continue;
                    const DRAMSim::Phase1EccStatus status = st.readProtected(key).status;
                    if (status == DRAMSim::Phase1EccStatus::CORRECTED ||
                        status == DRAMSim::Phase1EccStatus::UNCORRECTABLE)
                    {
                        latent += 1;
                    }
                }
            }
        }
    }
    return latent;
}

void releaseTileCodewords(ClosedLoopSession& session, unsigned logicalRow, unsigned firstCol)
{
    const unsigned physRow = session.region.physical(logicalRow);
    for (unsigned bank = 0; bank < session.rank->banks.size(); ++bank)
    {
        for (unsigned col = firstCol; col < firstCol + 8u; ++col)
        {
            for (unsigned cw = 0; cw < 4u; ++cw)
                session.model.releaseCodewordBits(bank, physRow, col, cw);
        }
    }
}

struct LiveLedgerState
{
    phase2::Ledger* ledger = nullptr;
    const KernelDesc* activeKernel = nullptr;
    std::vector<uint64_t> activeOps;
    std::vector<uint64_t> maxCompleteCycle;
};

void installLedgerObserver(ClosedLoopSession& session, LiveLedgerState& state)
{
    session.feeder->setCompletionObserver([&state](const DRAMSim::P1CompletionEvent& event) {
        if (state.ledger == nullptr || state.activeKernel == nullptr)
            return;
        const KernelDesc& k = *state.activeKernel;
        int base = -1;
        if (!event.is_write)
        {
            if (event.tag == "BANK_TO_GRF_" || event.tag == "FILL&ReLU")
                base = k.input0;
            else if (event.tag == "ADD" && k.hasSecond)
                base = k.input1;
        }
        else
        {
            if (event.tag == "GRF_TO_BANK" || event.tag == "GRF_A_TO_EVEN_BANK" ||
                event.tag == "GRF_B_TO_ODD_BANK")
                base = k.output;
        }
        if (base < 0)
            return;
        const unsigned baseU = static_cast<unsigned>(base);
        if (event.row < baseU)
            return;
        const unsigned rowOff = event.row - baseU;
        const unsigned tile = rowOff * 4u + (event.column / 8u);
        if (tile >= state.activeOps.size())
            return;
        const uint64_t opId = state.activeOps[tile];
        const phase2::OpEntry* entry = state.ledger->find(opId);
        if (entry == nullptr)
            return;
        if (entry->t_issue == UINT64_MAX)
            state.ledger->recordIssue(opId, event.submit_cycle);
        state.ledger->observeAccess(opId, event.row, event.is_write);
        if (event.complete_cycle > state.maxCompleteCycle[tile])
            state.maxCompleteCycle[tile] = event.complete_cycle;
    });
}

void runAttackWave(const ClosedLoopOptions& opts, ClosedLoopSession& session,
                   DRAMSim::Phase1Reports& reports, uint64_t steps, uint64_t& stepCounter,
                   ClosedLoopResult& result)
{
    if (steps == 0ull)
        return;
    DRAMSim::BurstType background;
    DRAMSim::BurstType activated;
    const bool batchDistinct = (opts.attack == "A3");
    uint64_t remaining = steps;
    while (remaining > 0ull)
    {
        const uint64_t batch = batchDistinct ? std::min<uint64_t>(8ull, remaining) : 1ull;
        for (uint64_t b = 0; b < batch; ++b)
        {
            const unsigned victim = session.region.physical(result.attackRow);
            const unsigned aggressor =
                selectAggressor(opts, session, result.attackRow, victim, stepCounter);
            enqueueReadCol(*session.feeder, session.codec, 0, aggressor, 0, activated, "HOST_ATK");
            result.attackTx += 1;
            result.hostTx += 1;
            if (stepCounter % 5ull == 0ull)
            {
                const unsigned bgStep = static_cast<unsigned>(stepCounter / 5ull);
                const unsigned bgBank = (bgStep % 2u == 0u) ? 1u : 3u;
                if (bgStep % 10u == 0u)
                {
                    const unsigned nAccRegions = 8192u / opts.regionN;
                    const unsigned regIdx = (bgStep / 10u) % nAccRegions;
                    const unsigned bgRow = regIdx * opts.regionN + std::min<unsigned>(14u, opts.regionN - 1u);
                    enqueueReadCol(*session.feeder, session.codec, bgBank, bgRow, 0, background,
                                   "HOST_BG");
                }
                else
                {
                    enqueueWriteCol(*session.feeder, session.codec, bgBank,
                                    static_cast<unsigned>(5000u + (bgStep % 32u)), 0, background,
                                    "HOST_BG");
                }
                result.hostTx += 1;
            }
            stepCounter += 1;
            remaining -= 1;
        }
        result.cycles += drainReports(*session.feeder, reports, 8000000ull);
        if (batchDistinct || (stepCounter % 16ull == 0ull) || remaining == 0ull)
            session.flushSwaps(result.cycles);
    }
}

struct DirectInjectSpec
{
    bool enabled = false;
    unsigned logicalRow = 0;
    unsigned burstColumn = 0;
    unsigned codeword = 0;
    unsigned bit0 = 14;
    bool secondBit = false;
    unsigned bit1 = 15;
    bool triggerPreAddSwap = false;
    bool dropCheckpoint = false;
};

ClosedLoopResult runClosedLoopInternal(const ClosedLoopOptions& opts,
                                       const DirectInjectSpec* injectSpec = nullptr)
{
    ClosedLoopResult result;
    try
    {
        const std::string deviceIni = resolveDeviceIni(opts.deviceIni);
        const std::string systemIni = resolveSystemIni(opts.systemIni);
        ensureDir(opts.outDir);

        phase3ResetHostLatency();
        ClosedLoopSession session(deviceIni, systemIni, opts.outDir, opts);
        DRAMSim::Phase1Reports reports(session.codec, 1,
                                       static_cast<unsigned>(session.rank->banks.size()),
                                       opts.reportGranularity, opts.reportDelay, 1, 256, true, true);
        reports.setDeferCeHookToTick(true);
        reports.setOnDieEccMode(opts.genMode == "HBM3_STYLE");

        for (DRAMSim::Bank& bank : session.rank->banks)
        {
            bank.setPhase1(DRAMSim::Phase1Profile::LOCAL_VISIBLE, &reports);
            bank.setPhase1Scrub(session.scrub);
            bank.setSwapScrub(session.scrub);
        }

        const unsigned bursts = (opts.workload == "DEC") ? 128u : 48u * 128u;
        const unsigned tiles = bursts / 128u;
        const unsigned rowsPerOperand = (opts.workload == "DEC") ? 1u : 12u;

        std::vector<BurstType> left(bursts), right(bursts), scaleData(bursts), expected(bursts);
        std::vector<KernelDesc> chain;
        std::vector<std::pair<uint16_t, unsigned>> inputs;
        uint16_t resultOperand = 0;
        std::vector<unsigned> trackedRows;

        if (opts.workload == "W2")
        {
            for (unsigned i = 0; i < bursts; ++i)
            {
                left[i] = positiveBurst(i, 1);
                right[i] = positiveBurst(i, 2);
                scaleData[i] = positiveBurst(i, 3);
                expected[i] = applyRelu((left[i] + right[i]) * scaleData[i]);
            }
            result.attackRow = 64u;
            result.scoreRow = 704u;
            inputs = {{0, 64u}, {1, 192u}, {3, 448u}};
            chain.push_back(KernelDesc{0, KernelType::ADD, 64, 192, 320, 0, 1, true, 2});
            chain.push_back(KernelDesc{1, KernelType::MUL, 320, 448, 576, 2, 3, true, 4});
            chain.push_back(KernelDesc{2, KernelType::RELU, 576, 0, 704, 4, 0, false, 5});
            resultOperand = 5;
            for (const unsigned base : {64u, 192u, 320u, 448u, 576u, 704u})
                for (unsigned off = 0; off < rowsPerOperand; ++off)
                    trackedRows.push_back(base + off);
        }
        else
        {
            for (unsigned i = 0; i < bursts; ++i)
            {
                left[i] = positiveBurst(i, 4);
                right[i] = positiveBurst(i, 5);
                expected[i] = applyRelu(left[i] + right[i]);
            }
            result.attackRow = 2048u;
            result.scoreRow = 2432u;
            inputs = {{0, 2048u}, {1, 2176u}};
            chain.push_back(KernelDesc{0, KernelType::ADD, 2048, 2176, 2304, 0, 1, true, 2});
            chain.push_back(KernelDesc{1, KernelType::RELU, 2304, 0, 2432, 2, 0, false, 3});
            resultOperand = 3;
            trackedRows = {2048u, 2176u, 2304u, 2432u};
        }

        NumpyBurstType gemvWeight;
        NumpyBurstType gemvInput;
        if (opts.workload == "DEC")
        {
            const std::vector<BurstType> cleanGemvRow(32, positiveBurst(0, 7));
            preloadRow(*session.kernel, cleanGemvRow, 0u);
            result.hostTx += 1;
            gemvWeight.bShape.push_back(256);
            gemvWeight.bShape.push_back(128);
            gemvWeight.bData.assign(4096, positiveBurst(0, 7));
            gemvInput.bShape.push_back(1);
            gemvInput.bShape.push_back(128);
            gemvInput.bData.assign(4096, positiveBurst(0, 8));
        }

        phase2::Phase2CheckpointStore store;
        for (const auto& input : inputs)
        {
            const std::vector<BurstType>* source =
                (input.first == 0) ? &left : ((input.first == 1) ? &right : &scaleData);
            for (unsigned tile = 0; tile < tiles; ++tile)
            {
                std::vector<BurstType> tileBursts(
                    source->begin() + static_cast<std::ptrdiff_t>(tile * 128u),
                    source->begin() + static_cast<std::ptrdiff_t>((tile + 1u) * 128u));
                if (injectSpec == nullptr || !injectSpec->dropCheckpoint || input.first != 0 ||
                    tile != 0)
                    store.addTile(input.first, tile, tileBursts);
            }
            preloadRow(*session.kernel, *source, input.second);
            result.hostTx += 1;
        }
        result.cycles += drainReports(*session.feeder, reports, 8000000ull);

        const uint64_t flipsBefore = session.model.cumulativeFlips();
        result.physicalVictim = session.region.physical(result.attackRow);

        if (opts.attack != "NO_ATK")
        {
            result.attackBudget = computeAttackBudget(opts);
            uint64_t stepCounter = 0;
            const unsigned probeCols = std::min<unsigned>(32u, bursts / 16u);
            auto probeInputRow = [&]() {
                for (DRAMSim::Bank& bank : session.rank->banks)
                    bank.setPhase1Scrub(false);
                std::vector<BurstType> probeBuf(probeCols);
                for (unsigned col = 0; col < probeCols; ++col)
                {
                    enqueueReadCol(*session.feeder, session.codec, 0, result.attackRow, col,
                                   probeBuf[col], "HOST_BG");
                    result.hostTx += 1;
                }
                result.cycles += drainReports(*session.feeder, reports, 8000000ull);
                session.flushSwaps(result.cycles);
                for (DRAMSim::Bank& bank : session.rank->banks)
                    bank.setPhase1Scrub(session.scrub);
            };

            if (opts.attack == "A3")
            {
                const double weights[4] = {2.40, 3.05, 3.75, 4.45};
                const double wSum = 13.65;
                uint64_t doneSteps = 0;
                for (int w = 0; w < 4; ++w)
                {
                    const uint64_t subSteps =
                        (w < 3)
                            ? static_cast<uint64_t>(static_cast<double>(result.attackBudget) *
                                                    (weights[w] / wSum))
                            : (result.attackBudget - doneSteps);
                    doneSteps += subSteps;
                    runAttackWave(opts, session, reports, subSteps, stepCounter, result);
                    if (w < 3)
                        probeInputRow();
                }
            }
            else
            {
                const uint64_t wave1 = result.attackBudget / 2ull;
                const uint64_t wave2 = result.attackBudget - wave1;
                runAttackWave(opts, session, reports, wave1, stepCounter, result);
                probeInputRow();
                runAttackWave(opts, session, reports, wave2, stepCounter, result);
            }
        }

        if (injectSpec != nullptr && injectSpec->enabled)
        {
            result.directInject = true;
            const unsigned phys = session.region.physical(injectSpec->logicalRow);
            const unsigned bitBase = injectSpec->codeword * 64u;
            session.rank->banks[0].injectRawDataBit(phys, injectSpec->burstColumn,
                                                    bitBase + injectSpec->bit0);
            if (injectSpec->secondBit)
            {
                session.rank->banks[0].injectRawDataBit(phys, injectSpec->burstColumn,
                                                        bitBase + injectSpec->bit1);
            }
        }
        result.physicalVictimEnd = session.region.physical(result.attackRow);

        if (opts.workload == "DEC")
        {
            phase3GemvRowBias = 0;
            for (int r = 0; r < 3; ++r)
            {
                const size_t pendingBefore = session.feeder->pendingCount();
                session.kernel->executeGemv(&gemvWeight, &gemvInput, false);
                result.usefulPimItems += session.feeder->pendingCount() - pendingBefore;
            }
            result.cycles += drainReports(*session.feeder, reports, 8000000ull);
            session.flushSwaps(result.cycles);
        }

        if (injectSpec != nullptr && injectSpec->enabled && injectSpec->triggerPreAddSwap)
        {
            // Alternate between two non-victim rows in Region 32 (+6u and +7u) for 80 steps,
            // draining each step so open-page mode closes and forces 80 real ACTIVATEs without
            // flushing deferred swaps; Region 32 arms L1 (since >= r1) and swaps 2048 <-> 2049
            // immediately after ADD drains.
            DRAMSim::BurstType scratch;
            for (unsigned step = 0; step < 80u; ++step)
            {
                const unsigned row = injectSpec->logicalRow + (step % 2u == 0u ? 6u : 7u);
                enqueueReadCol(*session.feeder, session.codec, 0, row, 0, scratch, "HOST_ATK");
                result.attackTx += 1;
                result.hostTx += 1;
                result.cycles += drainReports(*session.feeder, reports, 8000000ull);
            }
        }

        if (opts.attack != "NO_ATK")
        {
            const unsigned vicReg = result.attackRow / opts.regionN;
            const unsigned preAddSteps = std::min<unsigned>(128u, session.region.period(vicReg));
            if (preAddSteps > 0u)
            {
                unsigned farOff =
                    ((result.physicalVictimEnd % opts.regionN) + (opts.regionN / 2u)) % opts.regionN;
                if (farOff + 1u >= opts.regionN)
                    farOff = (farOff >= 2u) ? (farOff - 2u) : 0u;
                const unsigned rowA = vicReg * opts.regionN + farOff;
                const unsigned rowB = rowA + 1u;
                DRAMSim::BurstType scratch;
                for (unsigned step = 0; step < preAddSteps; ++step)
                {
                    const unsigned row = (step % 2u == 0u) ? rowA : rowB;
                    enqueueReadCol(*session.feeder, session.codec, 0, row, 0, scratch, "HOST_ATK");
                    result.attackTx += 1;
                    result.hostTx += 1;
                    result.cycles += drainReports(*session.feeder, reports, 8000000ull);
                }
            }
        }

        phase2::Ledger ledger;
        for (const auto& input : inputs)
            ledger.setBaseRow(input.first, input.second);
        for (const KernelDesc& k : chain)
            ledger.setBaseRow(k.out, static_cast<unsigned>(k.output));

        LiveLedgerState ledgerState;
        ledgerState.ledger = &ledger;
        installLedgerObserver(session, ledgerState);

        for (const KernelDesc& k : chain)
        {
            ledgerState.activeKernel = &k;
            ledgerState.activeOps.assign(tiles, 0ull);
            ledgerState.maxCompleteCycle.assign(tiles, 0ull);
            for (unsigned tile = 0; tile < tiles; ++tile)
            {
                std::vector<std::pair<uint16_t, uint32_t>> reads = {{k.in0, tile}};
                if (k.hasSecond)
                    reads.push_back(std::make_pair(k.in1, tile));
                ledgerState.activeOps[tile] =
                    ledger.planOp(1, k.index, tile, reads, {{k.out, tile}});
            }
            const size_t pendingBefore = session.feeder->pendingCount();
            session.kernel->executeEltwise(static_cast<int>(bursts), pimBankType::ALL_BANK, k.type,
                                           k.input0, k.output, k.input1);
            result.usefulPimItems += session.feeder->pendingCount() - pendingBefore;

            if (k.index == 0)
            {
                // Drain DRAM commands of ADD first, then flush pending region swaps before
                // reports.tick() delivers delayed UE reports.
                result.cycles += drain(*session.feeder, 8000000ull);
                session.flushSwaps(result.cycles);
            }
            else
            {
                result.cycles += drainReports(*session.feeder, reports, 8000000ull);
                session.flushSwaps(result.cycles);
            }
            for (unsigned tile = 0; tile < tiles; ++tile)
            {
                const uint64_t opId = ledgerState.activeOps[tile];
                const uint64_t compCycle = ledgerState.maxCompleteCycle[tile] > 0
                                               ? ledgerState.maxCompleteCycle[tile]
                                               : session.feeder->currentClockCycle;
                if (!ledger.completeObserved(opId, compCycle))
                    throw std::runtime_error("closed-loop ledger did not complete observed op");
            }
        }
        ledgerState.activeKernel = nullptr;
        session.feeder->setCompletionObserver({});
        result.cycles += drainReports(*session.feeder, reports, 8000000ull);
        const uint64_t completedAt = session.feeder->currentClockCycle;

        // Internal diagnostic pre-gate audit read prior to binding Gate::hold for consumer publication control.
        const std::vector<BurstType> beforeRecovery =
            readRow(session, reports, result.scoreRow, bursts, result.cycles);
        result.hostTx += bursts;
        result.computeErrors = countBurstDiff(beforeRecovery, expected);
        result.visibleBits = countBurstBits(beforeRecovery, expected);

        phase2::Gate gate;
        std::vector<phase2::DataRef> resultRefs;
        resultRefs.reserve(tiles);
        for (unsigned tile = 0; tile < tiles; ++tile)
        {
            const uint64_t ver = ledger.currentVersion(resultOperand, tile);
            const phase2::DataRef ref{resultOperand, tile, ver};
            gate.hold(ref);
            resultRefs.push_back(ref);
        }
        for (unsigned rowOff = 0; rowOff < rowsPerOperand; ++rowOff)
        {
            const unsigned repTile = (rowOff * 4u < tiles) ? (rowOff * 4u) : 0u;
            phase3BindPublication(result.scoreRow + rowOff, &gate, resultOperand, repTile,
                                  ledger.currentVersion(resultOperand, repTile));
        }

        phase2::EngineConfig config;
        config.policy = phase2::Policy::B2_CLASS_AWARE;
        config.window_mode = phase2::WindowMode::AUTO;
        config.delta_cycles = 1;
        phase2::CheckpointView view;
        if (injectSpec != nullptr && injectSpec->dropCheckpoint)
            view.setAbsent(0, 0);
        phase2::RecoveryEngine engine;

        std::set<std::pair<uint16_t, uint32_t>> restoreSet;
        std::set<std::pair<uint16_t, uint32_t>> replayTileSet;
        std::vector<phase2::DataRef> poisonRefs;
        bool poison = false;
        bool partialEscape = false;

        if (session.region.recoveryEnabled())
        {
            for (const Phase1PublicReport& report : reports.reports())
            {
                if (report.error_class != "UE")
                    continue;
                const phase2::Decision decision = engine.analyze(
                    report, completedAt, config, ledger, gate, view, reports.overflow(), session.codec);
                result.recovery = phase2::p2StatusName(decision.status);
                if (decision.status == phase2::P2Status::STALE_EPOCH ||
                    (!session.epochCheck && decision.status == phase2::P2Status::SOURCE_REPAIRED_ONLY))
                    result.misrecovered += 1;
                if (decision.status == phase2::P2Status::STALE_EPOCH)
                    continue;
                if (decision.status == phase2::P2Status::OUTSIDE_WINDOW_NO_TRUSTED_INPUT)
                {
                    const phase2::ExecutionPlan plan =
                        engine.planExecution(decision, ledger, &config, &gate);
                    poisonRefs.insert(poisonRefs.end(), plan.poison_outputs.begin(),
                                      plan.poison_outputs.end());
                    poison = true;
                    break;
                }
                if (decision.status == phase2::P2Status::PARTIAL_WITH_ESCAPE)
                    partialEscape = true;
                for (const phase2::DataRef& ref : decision.restore_list)
                {
                    if (ref.version == 0)
                        restoreSet.insert(std::make_pair(ref.operand, ref.tile));
                }
                for (const phase2::Run& run : decision.replay_runs)
                {
                    const uint32_t count = (run.tile_count == 0u) ? 1u : run.tile_count;
                    for (uint32_t t = 0; t < count; ++t)
                        replayTileSet.insert(std::make_pair(run.kernel_idx, run.tile_begin + t));
                }
            }
        }

        if (poison)
        {
            if (poisonRefs.empty())
                poisonRefs = resultRefs;
            gate.poison(poisonRefs);
            result.withheld = true;
            result.recoveryFailed = true;
            result.refused = true;
            result.bitExact = true;
        }
        else
        {
            for (const auto& item : restoreSet)
            {
                if (item.second < 64u)
                    result.restoredMask |= (1ull << item.second);
                const std::vector<BurstType>* tileVec = store.tile(item.first, item.second);
                if (tileVec == nullptr)
                    throw std::runtime_error("recovery requested absent checkpoint tile");
                const unsigned row = ledger.baseRow(item.first) + phase2::tileRowOffset(item.second);
                const unsigned col = phase2::tileFirstColumn(item.second);
                preloadRow(*session.kernel, *tileVec, row, col);
                releaseTileCodewords(session, row, col);
                result.restoredTiles += 1;
            }
            if (result.restoredTiles > 0)
                result.cycles += drainReports(*session.feeder, reports, 8000000ull);

            // Coalesce deduplicated (kernel_idx, tile) into contiguous runs.
            std::vector<phase2::Run> runs;
            for (const auto& kt : replayTileSet)
            {
                if (!runs.empty() && runs.back().kernel_idx == kt.first &&
                    runs.back().tile_begin + runs.back().tile_count == kt.second)
                {
                    runs.back().tile_count += 1u;
                }
                else
                {
                    phase2::Run run;
                    run.job_id = 1u;
                    run.kernel_idx = kt.first;
                    run.tile_begin = kt.second;
                    run.tile_count = 1u;
                    runs.push_back(run);
                }
            }
            for (const phase2::Run& run : runs)
            {
                const KernelDesc* kernel = nullptr;
                for (const KernelDesc& one : chain)
                {
                    if (one.index == run.kernel_idx)
                        kernel = &one;
                }
                if (kernel == nullptr)
                    throw std::runtime_error("recovery plan names unknown kernel");
                const int count = static_cast<int>(run.tile_count == 0u ? 1u : run.tile_count);
                const size_t pendingBefore = session.feeder->pendingCount();
                session.kernel->executeEltwiseTiles(kernel->type, static_cast<int>(run.tile_begin),
                                                    count, kernel->input0, kernel->output,
                                                    kernel->input1);
                result.replayPimItems += session.feeder->pendingCount() - pendingBefore;
                result.replayed = true;
            }
            if (result.replayed)
            {
                result.cycles += drainReports(*session.feeder, reports, 8000000ull);
                session.flushSwaps(result.cycles);
            }
            for (const phase2::DataRef& ref : resultRefs)
                gate.publish(ref, session.feeder->currentClockCycle);
            const std::vector<BurstType> committed =
                readRow(session, reports, result.scoreRow, bursts, result.cycles);
            result.hostTx += bursts;
            result.committedMismatch = countBurstDiff(committed, expected);
            result.bitExact = (result.committedMismatch == 0u);
            result.erroneousCommit = !result.bitExact || (partialEscape && !result.bitExact);
        }

        for (const DRAMSim::Bank& bank : session.rank->banks)
            result.scrubWrites += bank.phase1ScrubWrites();
        result.scrubCycles = hbm2ScrubCycles(result.scrubWrites);
        result.cycles += result.scrubCycles;
        session.billMoves(result.cycles);
        session.billPara(result.cycles);
        result.swapCycles = session.swapCycles;
        result.paraCycles = session.paraCycles;
        result.flips = session.model.cumulativeFlips() - flipsBefore;
        result.latent =
            countLatentFaults(*session.rank, session.region, trackedRows,
                              session.codec.geometry().burst_columns());
        result.crossings = session.model.crossings().size();
        for (const ReadDisturbanceModel::Crossing& crossing : session.model.crossings())
        {
            if (!crossing.placed)
                result.unplaced += 1;
        }
        result.modelScrubs = session.model.scrubbedCount();
        result.ce = reports.internalCe();
        result.ue = reports.internalUe();
        result.onDieMaskedCe = reports.onDieMaskedCe();
        result.level = session.region.level(result.attackRow / opts.regionN);
        result.sawL2 = session.region.sawL2();
        result.pimItems = result.usefulPimItems;
        if (result.recovery == "NONE" && result.ce > 0 && !result.replayed)
            result.recovery = "CE_OBSERVED";
        if (result.recovery == "SELECTIVE_RECOVERED" || result.recovery == "SOURCE_REPAIRED_ONLY" ||
            result.recovery == "WHOLE_JOB_FALLBACK" ||
            result.recovery == "OUTSIDE_WINDOW_NO_TRUSTED_INPUT")
        {
            result.phase2Status = result.recovery;
        }
    }
    catch (const std::exception& failure)
    {
        result.status = "FAILED";
        result.error = failure.what();
        for (char& ch : result.error)
        {
            if (ch == '"' || ch == '\n' || ch == '\r')
                ch = ' ';
        }
    }
    return result;
}

std::string resultToJson(const ClosedLoopOptions& opts, const ClosedLoopResult& result)
{
    const double effThroughput =
        (result.status == "SUCCESS" && result.bitExact && !result.withheld && result.cycles > 0)
            ? static_cast<double>(result.usefulPimItems) / static_cast<double>(result.cycles)
            : 0.0;
    std::ostringstream json;
    json << std::setprecision(10)
         << "{\"workload\":\"" << opts.workload << "\""
         << ",\"scheme\":\"" << opts.scheme << "\""
         << ",\"attack\":\"" << opts.attack << "\""
         << ",\"scale\":" << opts.scale
         << ",\"index\":" << opts.index
         << ",\"seed\":" << opts.seed
         << ",\"status\":\"" << result.status << "\""
         << ",\"error\":\"" << result.error << "\""
         << ",\"cycles\":" << result.cycles
         << ",\"pim_items\":" << result.pimItems
         << ",\"useful_pim_items\":" << result.usefulPimItems
         << ",\"replay_pim_items\":" << result.replayPimItems
         << ",\"effective_throughput\":" << effThroughput
         << ",\"host_transactions\":" << result.hostTx
         << ",\"attack_transactions\":" << result.attackTx
         << ",\"attack_budget\":" << result.attackBudget
         << ",\"flips\":" << result.flips
         << ",\"latent_flips\":" << result.latent
         << ",\"level\":" << result.level
         << ",\"saw_l2\":" << (result.sawL2 ? "true" : "false")
         << ",\"bit_exact\":" << (result.bitExact ? "true" : "false")
         << ",\"replayed\":" << (result.replayed ? "true" : "false")
         << ",\"withheld\":" << (result.withheld ? "true" : "false")
         << ",\"recovery_failed\":" << (result.recoveryFailed ? "true" : "false")
         << ",\"para_p\":" << opts.paraP
         << ",\"effective_para_p\":" << opts.effectiveParaP()
         << ",\"phase2_status\":\"" << result.phase2Status << "\""
         << ",\"recovery_status\":\"" << result.recovery << "\""
         << ",\"ce\":" << result.ce
         << ",\"ue\":" << result.ue
         << ",\"on_die_masked_ce\":" << result.onDieMaskedCe
         << ",\"scrub_writes\":" << result.scrubWrites
         << ",\"scrub_cycles\":" << result.scrubCycles
         << ",\"swap_cycles\":" << result.swapCycles
         << ",\"para_cycles\":" << result.paraCycles
         << ",\"restored_tiles\":" << result.restoredTiles
         << ",\"model_scrubs\":" << result.modelScrubs
         << ",\"compute_errors\":" << result.computeErrors
         << ",\"committed_mismatch\":" << result.committedMismatch
         << ",\"misrecovered\":" << result.misrecovered
         << ",\"erroneous_commit\":" << (result.erroneousCommit ? "true" : "false")
         << ",\"refused\":" << (result.refused ? "true" : "false")
         << ",\"score_row\":" << result.scoreRow
         << ",\"attack_row\":" << result.attackRow
         << ",\"physical_victim\":" << result.physicalVictim
         << ",\"physical_victim_end\":" << result.physicalVictimEnd
         << ",\"visible_bits\":" << result.visibleBits
         << ",\"crossings\":" << result.crossings
         << ",\"unplaced\":" << result.unplaced
         << ",\"direct_inject\":" << (result.directInject ? "true" : "false")
         << ",\"restored_mask\":" << result.restoredMask
         << ",\"swap_mode\":\"" << opts.swapMode << "\""
         << ",\"gen_mode\":\"" << opts.genMode << "\""
         << ",\"region_n\":" << opts.regionN
         << ",\"mg_k\":" << opts.mgK
         << ",\"report_delay\":" << opts.reportDelay
         << ",\"report_granularity\":\"" << opts.reportGranularity << "\""
         << ",\"hc_dist\":\"" << opts.hcDist << "\""
         << ",\"vrd_max\":" << opts.vrdMax
         << ",\"margin_g\":" << opts.effectiveMarginG()
         << ",\"ce_feedback\":" << (opts.effectiveCeFeedback() ? "true" : "false")
         << ",\"scrub_enabled\":" << (opts.effectiveScrub() ? "true" : "false")
         << ",\"epoch_check\":" << (opts.effectiveEpochCheck() ? "true" : "false")
         << ",\"detector_enabled\":" << (opts.effectiveDetector() ? "true" : "false")
         << "}\n";
    return json.str();
}

void writeTextFile(const std::string& path, const std::string& body)
{
    std::ofstream out(path.c_str());
    if (!out)
        throw std::runtime_error("cannot write file: " + path);
    out << body;
}

}  // namespace

TEST(Phase3ClosedLoop, UnitMechanisms)
{
    // 1. 64:8 CodewordInjectFn places both data and parity flips across columns & codewords 0..3.
    {
        DRAMSim::Phase1Storage storage;
        const unsigned testRows[] = {10u, 12u, 14u, 16u, 18u, 20u, 22u, 24u};
        for (unsigned row : testRows)
        {
            for (unsigned col = 0; col < 32u; ++col)
            {
                for (unsigned cw = 0; cw < 4u; ++cw)
                    storage.writeLegal(DRAMSim::Phase1StorageKey{0, 0, 0, row, col, cw},
                                       0x123456789abcdef0ull);
            }
        }
        ReadDisturbanceModel model;
        model.configure(42ull, 0.0625, "HBM2");
        model.setDistribution("truncated_lognormal", 3.5);
        model.setCodewordInjector(&injectCodewordStorage, &storage);
        for (unsigned row : testRows)
            model.forceRow(0, row, 4.0, 1.0);
        for (unsigned row : testRows)
        {
            for (int act = 0; act < 20; ++act)
                model.noteActivate(0, row + 1u);
        }
        EXPECT_EQ(80u, model.flips().size());
        bool sawData = false;
        bool sawParity = false;
        std::set<unsigned> cols;
        std::set<unsigned> codewords;
        for (const ReadDisturbanceModel::Flip& f : model.flips())
        {
            if (f.parity)
                sawParity = true;
            else
                sawData = true;
            cols.insert(f.burstColumn);
            for (unsigned cw = 0; cw < 4u; ++cw)
            {
                const DRAMSim::Phase1StorageKey probeKey{0, 0, f.bank, f.row, f.burstColumn, cw};
                if (storage.readProtected(probeKey).status != DRAMSim::Phase1EccStatus::CLEAN)
                    codewords.insert(cw);
            }
        }
        EXPECT_TRUE(sawData);
        EXPECT_TRUE(sawParity);
        EXPECT_GT(cols.size(), 1u);
        EXPECT_EQ(4u, codewords.size());
    }

    // 2. setSeed rotates keyNew pseudorandomly while preserving logical(physical(r)) == r.
    {
        RegionMap map;
        map.configure("FIX", 1.5, 2.0, 1.0);
        map.setSeed(20261122ull);
        std::vector<unsigned> keyAtEpoch;
        for (unsigned swap = 0; swap < 32u; ++swap)
            map.forceSwap(nullptr);
        EXPECT_EQ(0ull, map.epoch(0));
        for (unsigned ep = 1u; ep <= 6u; ++ep)
        {
            map.forceSwap(nullptr);
            EXPECT_EQ(static_cast<uint64_t>(ep), map.epoch(0));
            for (unsigned swap = 0; swap < 31u; ++swap)
                map.forceSwap(nullptr);
            for (unsigned r = 0; r < 64u; ++r)
                EXPECT_EQ(r, map.logical(map.physical(r)));
            keyAtEpoch.push_back(map.physical(0));
        }
        bool nonLinear = false;
        for (size_t i = 1; i < keyAtEpoch.size(); ++i)
        {
            if (keyAtEpoch[i] != ((keyAtEpoch[i - 1] + 1u) & 63u))
                nonLinear = true;
        }
        EXPECT_TRUE(nonLinear);
    }

    // Boot one session to initialize DRAMSim Configuration for Rank/Bank/ActivationObserver.
    ClosedLoopOptions baseOpts;
    baseOpts.deviceIni = resolveDeviceIni("");
    baseOpts.systemIni = resolveSystemIni("");
    baseOpts.outDir = ".";
    baseOpts.workload = "DEC";
    baseOpts.scheme = "OURS";
    baseOpts.attack = "NO_ATK";
    ClosedLoopSession session(baseOpts.deviceIni, baseOpts.systemIni, baseOpts.outDir, baseOpts);

    // 3. ActivationObserver triggers noteRefreshWindow after 8,192 REFs; noteCe resets quiet.
    {
        RegionMap reg;
        reg.configure("OURS", 1.1, 2.0, 1.0);
        reg.noteCe(0, 0);
        EXPECT_EQ(1u, reg.level(0));
        reg.noteRefreshWindow();  // close alarmed window
        EXPECT_EQ(1u, reg.level(0));
        reg.noteRefreshWindow();  // 1st quiet window
        EXPECT_EQ(1u, reg.level(0));
        reg.noteCe(0, 0);         // re-arms alarmed & resets quiet -> 0 without L2
        reg.noteRefreshWindow();  // close alarmed window
        EXPECT_EQ(1u, reg.level(0));
        reg.noteRefreshWindow();  // 1st quiet window
        EXPECT_EQ(1u, reg.level(0));
        reg.noteRefreshWindow();  // 2nd quiet window -> demote to L0
        EXPECT_EQ(0u, reg.level(0));

        reg.noteCe(0, 0);
        EXPECT_EQ(1u, reg.level(0));
        reg.noteRefreshWindow();  // close alarmed window
        EXPECT_EQ(1u, reg.level(0));
        reg.noteRefreshWindow();  // 1st quiet window
        EXPECT_EQ(1u, reg.level(0));

        session.observer.attachRegion(&reg);
        DRAMSim::BusPacket refPacket(DRAMSim::REF, 0, 0, 0, 0, 0, nullptr, std::cout);
        for (unsigned i = 0; i < 8192u; ++i)
            session.observer.onCommand(session.rank, &refPacket);
        EXPECT_EQ(8192ull, session.observer.refreshCommands());
        EXPECT_EQ(0u, reg.level(0));
        session.observer.attachRegion(&session.region);
    }

    // 4. HBM3_STYLE onDieEccMode masks 1-bit CEs while surfacing 2-bit UEs.
    {
        DRAMSim::Phase1Reports onDieReports(session.codec, 1, 16, "page", 1, 1, 64, true, true);
        onDieReports.setOnDieEccMode(true);
        EXPECT_TRUE(onDieReports.onDieEccMode());

        DRAMSim::Bank bank(std::cout);
        bank.setIdentity(0, 0, 0);
        bank.setPhase1(DRAMSim::Phase1Profile::LOCAL_VISIBLE, &onDieReports);

        DRAMSim::BurstType payload = positiveBurst(0, 1);
        DRAMSim::BurstType readSlot;
        DRAMSim::BusPacket wr0(DRAMSim::WRITE, 0, 0, 5, 0, 0, &payload, std::cout);
        DRAMSim::BusPacket wr1(DRAMSim::WRITE, 0, 1, 5, 0, 0, &payload, std::cout);
        bank.write(&wr0, DRAMSim::Phase1AccessType::HOST, 10);
        bank.write(&wr1, DRAMSim::Phase1AccessType::HOST, 11);

        bank.injectRawDataBit(5, 0, 3);
        DRAMSim::BusPacket rd0(DRAMSim::READ, 0, 0, 5, 0, 0, &readSlot, std::cout);
        bank.read(&rd0, DRAMSim::Phase1AccessType::PIM_OPERAND, 20);
        EXPECT_EQ(0ull, onDieReports.internalCe());
        EXPECT_EQ(1ull, onDieReports.onDieMaskedCe());
        EXPECT_EQ(0ull, onDieReports.internalUe());

        bank.injectRawDataBit(5, 1, 3);
        bank.injectRawDataBit(5, 1, 4);
        DRAMSim::BusPacket rd1(DRAMSim::READ, 0, 1, 5, 0, 0, &readSlot, std::cout);
        bank.read(&rd1, DRAMSim::Phase1AccessType::PIM_OPERAND, 30);
        EXPECT_EQ(1ull, onDieReports.internalUe());
    }

    // 5. Swap scrubbing: setSwapScrub(true) scrubs 1-bit CEs and preserves 2-bit UEs;
    //    setSwapScrub(false) preserves both 1-bit CEs and 2-bit UEs.
    {
        DRAMSim::Bank scrubBank(std::cout);
        DRAMSim::Bank noScrubBank(std::cout);
        scrubBank.setIdentity(0, 0, 0);
        noScrubBank.setIdentity(0, 0, 0);
        scrubBank.setPhase1(DRAMSim::Phase1Profile::LOCAL_VISIBLE, nullptr);
        noScrubBank.setPhase1(DRAMSim::Phase1Profile::LOCAL_VISIBLE, nullptr);
        scrubBank.setSwapScrub(true);
        noScrubBank.setSwapScrub(false);
        EXPECT_TRUE(scrubBank.swapScrub());
        EXPECT_FALSE(noScrubBank.swapScrub());

        DRAMSim::BurstType payload = positiveBurst(1, 2);
        DRAMSim::BusPacket wr0(DRAMSim::WRITE, 0, 0, 0, 0, 0, &payload, std::cout);
        DRAMSim::BusPacket wr1(DRAMSim::WRITE, 0, 1, 0, 0, 0, &payload, std::cout);
        scrubBank.write(&wr0, DRAMSim::Phase1AccessType::HOST, 1);
        scrubBank.write(&wr1, DRAMSim::Phase1AccessType::HOST, 2);
        noScrubBank.write(&wr0, DRAMSim::Phase1AccessType::HOST, 1);
        noScrubBank.write(&wr1, DRAMSim::Phase1AccessType::HOST, 2);

        scrubBank.injectRawDataBit(0, 0, 0);
        scrubBank.injectRawDataBit(0, 1, 0);
        scrubBank.injectRawDataBit(0, 1, 1);

        noScrubBank.injectRawDataBit(0, 0, 0);
        noScrubBank.injectRawDataBit(0, 1, 0);
        noScrubBank.injectRawDataBit(0, 1, 1);

        scrubBank.exchangeRows(0, 1);
        noScrubBank.exchangeRows(0, 1);

        const DRAMSim::Phase1StorageKey ceKey{0, 0, 0, 1, 0, 0};
        const DRAMSim::Phase1StorageKey ueKey{0, 0, 0, 1, 1, 0};
        EXPECT_EQ(DRAMSim::Phase1EccStatus::CLEAN, scrubBank.phase1Storage().readProtected(ceKey).status);
        EXPECT_EQ(DRAMSim::Phase1EccStatus::UNCORRECTABLE,
                  scrubBank.phase1Storage().readProtected(ueKey).status);
        EXPECT_EQ(DRAMSim::Phase1EccStatus::CORRECTED,
                  noScrubBank.phase1Storage().readProtected(ceKey).status);
        EXPECT_EQ(DRAMSim::Phase1EccStatus::UNCORRECTABLE,
                  noScrubBank.phase1Storage().readProtected(ueKey).status);
    }

    // 6. PARA trigger accounting and OURS-EPOCH clean-run exactness.
    {
        ReadDisturbanceModel paraModel;
        paraModel.configure(1ull, 1.0, "HBM2");
        RegionMap paraMap;
        paraMap.configure("PARA", 1.5, 2.0, 1.0);
        paraMap.setParaProbability(1.0);
        paraMap.noteActivate(0, 100, &paraModel);
        const unsigned abBanks[8] = {0, 2, 4, 6, 8, 10, 12, 14};
        paraMap.noteCommand(200, abBanks, 8, &paraModel);
        EXPECT_EQ(1ull, paraMap.paraSbCommands());
        EXPECT_EQ(1ull, paraMap.paraAbCommands());
        EXPECT_EQ(2ull, paraMap.paraCommands());

        RegionMap oursMap;
        oursMap.configure("OURS", 1.1, 2.0, 1.0);
        oursMap.setSeed(20261122ull);
        RegionMap badEpochMap;
        badEpochMap.configure("OURS-EPOCH", 1.1, 2.0, 1.0);
        badEpochMap.setSeed(20261122ull);
        EXPECT_TRUE(badEpochMap.epochCorrupt());
        bool sawCorruptLogical = false;
        for (unsigned r = 0; r < 64u; ++r)
        {
            const unsigned p = badEpochMap.physical(r);
            EXPECT_EQ(oursMap.physical(r), p);
            if (badEpochMap.logical(p) != r)
                sawCorruptLogical = true;
        }
        EXPECT_TRUE(sawCorruptLogical);
    }
}

TEST(Phase3ClosedLoop, BasicClosedLoopSmoke)
{
    ClosedLoopOptions base = parseClosedLoopOptions();
    base.workload = "DEC";
    base.scale = 0.0625;
    base.deviceIni = resolveDeviceIni(base.deviceIni);
    base.systemIni = resolveSystemIni(base.systemIni);
    ensureDir(base.outDir);

    // 1. Fault-free NO_ATK on NONE, OURS, and OURS-EPOCH.
    ClosedLoopOptions cleanOpts = base;
    cleanOpts.attack = "NO_ATK";
    cleanOpts.scheme = "NONE";
    const ClosedLoopResult cleanNone = runClosedLoopInternal(cleanOpts);
    cleanOpts.scheme = "OURS";
    const ClosedLoopResult cleanOurs = runClosedLoopInternal(cleanOpts);
    cleanOpts.scheme = "OURS-EPOCH";
    const ClosedLoopResult cleanBadEpoch = runClosedLoopInternal(cleanOpts);

    EXPECT_EQ(std::string("SUCCESS"), cleanNone.status) << cleanNone.error;
    EXPECT_TRUE(cleanNone.bitExact);
    EXPECT_EQ(0ull, cleanNone.flips);
    EXPECT_EQ(std::string("SUCCESS"), cleanOurs.status) << cleanOurs.error;
    EXPECT_TRUE(cleanOurs.bitExact);
    EXPECT_EQ(0ull, cleanOurs.flips);
    EXPECT_EQ(std::string("SUCCESS"), cleanBadEpoch.status) << cleanBadEpoch.error;
    EXPECT_TRUE(cleanBadEpoch.bitExact);
    EXPECT_EQ(0ull, cleanBadEpoch.flips);

    // 2. Equal-budget A1 across 5 seeds (index 0..4) for NONE, REACT, OURS, plus FIX baseline.
    unsigned noneEscapes = 0;
    uint64_t oursSwapTotal = 0;
    uint64_t fixSwapTotal = 0;
    for (int idx = 0; idx < 5; ++idx)
    {
        ClosedLoopOptions atkOpts = base;
        atkOpts.attack = "A1";
        atkOpts.index = idx;
        atkOpts.seed = 20261122ull + static_cast<uint64_t>(idx) * 101ull;

        atkOpts.scheme = "NONE";
        const ClosedLoopResult noneRes = runClosedLoopInternal(atkOpts);
        EXPECT_EQ(std::string("SUCCESS"), noneRes.status) << noneRes.error;
        if (!noneRes.bitExact)
            noneEscapes += 1;

        atkOpts.scheme = "REACT";
        const ClosedLoopResult reactRes = runClosedLoopInternal(atkOpts);
        EXPECT_EQ(std::string("SUCCESS"), reactRes.status) << reactRes.error;
        EXPECT_TRUE(reactRes.bitExact);
        EXPECT_EQ(cleanNone.usefulPimItems, reactRes.usefulPimItems);
        if (reactRes.ue > 0)
            EXPECT_TRUE(reactRes.replayed);

        atkOpts.scheme = "OURS";
        const ClosedLoopResult oursRes = runClosedLoopInternal(atkOpts);
        EXPECT_EQ(std::string("SUCCESS"), oursRes.status) << oursRes.error;
        EXPECT_TRUE(oursRes.bitExact);
        EXPECT_EQ(cleanOurs.usefulPimItems, oursRes.usefulPimItems);
        oursSwapTotal += oursRes.swapCycles;

        atkOpts.scheme = "FIX";
        const ClosedLoopResult fixRes = runClosedLoopInternal(atkOpts);
        EXPECT_EQ(std::string("SUCCESS"), fixRes.status) << fixRes.error;
        EXPECT_LT(oursRes.swapCycles, fixRes.swapCycles);
        fixSwapTotal += fixRes.swapCycles;
    }
    EXPECT_GE(noneEscapes, 1u);
    EXPECT_LT(oursSwapTotal, fixSwapTotal);

    // 3. Deterministic epoch-race check (OURS vs OURS-EPOCH).
    DirectInjectSpec raceSpec;
    raceSpec.enabled = true;
    raceSpec.logicalRow = 2048u;
    raceSpec.burstColumn = 0u;
    raceSpec.codeword = 0u;
    raceSpec.bit0 = 14u;
    raceSpec.secondBit = true;
    raceSpec.bit1 = 15u;
    raceSpec.triggerPreAddSwap = true;

    ClosedLoopOptions raceOpts = base;
    raceOpts.attack = "NO_ATK";
    raceOpts.reportDelay = 32ull;
    raceOpts.scheme = "OURS";
    const ClosedLoopResult raceOurs = runClosedLoopInternal(raceOpts, &raceSpec);
    raceOpts.scheme = "OURS-EPOCH";
    const ClosedLoopResult raceBadEpoch = runClosedLoopInternal(raceOpts, &raceSpec);

    EXPECT_EQ(std::string("SUCCESS"), raceOurs.status) << raceOurs.error;
    EXPECT_GT(raceOurs.ue, 0ull);
    EXPECT_EQ(std::string("SELECTIVE_RECOVERED"), raceOurs.recovery);
    EXPECT_TRUE(raceOurs.bitExact);

    EXPECT_EQ(std::string("SUCCESS"), raceBadEpoch.status) << raceBadEpoch.error;
    EXPECT_GT(raceBadEpoch.ue, 0ull);
    EXPECT_FALSE(raceBadEpoch.bitExact);

    std::ostringstream smoke;
    smoke << "{\"clean_none_exact\":" << (cleanNone.bitExact ? "true" : "false")
          << ",\"clean_ours_exact\":" << (cleanOurs.bitExact ? "true" : "false")
          << ",\"clean_bad_epoch_exact\":" << (cleanBadEpoch.bitExact ? "true" : "false")
          << ",\"none_a1_escapes\":" << noneEscapes
          << ",\"ours_swap_cycles_5seeds\":" << oursSwapTotal
          << ",\"fix_swap_cycles_5seeds\":" << fixSwapTotal
          << ",\"epoch_race_ours_recovery\":\"" << raceOurs.recovery << "\""
          << ",\"epoch_race_ours_exact\":" << (raceOurs.bitExact ? "true" : "false")
          << ",\"epoch_race_bad_recovery\":\"" << raceBadEpoch.recovery << "\""
          << ",\"epoch_race_bad_exact\":" << (raceBadEpoch.bitExact ? "true" : "false")
          << "}\n";
    writeTextFile(base.outDir + "/closed_loop_smoke.json", smoke.str());
}

TEST(Phase3ClosedLoop, Cell)
{
    const ClosedLoopOptions opts = parseClosedLoopOptions();
    if (!opts.runCell)
        return;
    ensureDir(opts.outDir);
    const ClosedLoopResult result = runClosedLoopInternal(opts);
    writeTextFile(opts.outDir + "/result.json", resultToJson(opts, result));
    EXPECT_EQ(std::string("SUCCESS"), result.status) << result.error;
}

#include "Phase3Bridge.h"

#include <algorithm>
#include <cmath>
#include <map>

#include "Phase2Gate.h"

namespace
{
uint64_t (*epochFn)(unsigned, unsigned, void*) = nullptr;
unsigned (*logicalFn)(unsigned, unsigned, void*) = nullptr;
unsigned (*physicalFn)(unsigned, unsigned, void*) = nullptr;
void (*ceFn)(unsigned, unsigned, void*) = nullptr;
void* hookContext = nullptr;
void* physicalContext = nullptr;
void* ceContext = nullptr;
std::vector<uint64_t> latencies;
std::map<unsigned, std::pair<DRAMSim::phase2::Gate*, DRAMSim::phase2::DataRef>> publication;
uint64_t blockedReads = 0;
uint64_t staleRequests = 0;
bool issueEpochArmed = false;
uint64_t issueEpoch = 0;
}  // namespace

bool phase3IsLogicalDataTag(const std::string& tag)
{
    return tag.empty() || tag == "output" || tag == "HOST_BG" || tag == "BANK_TO_GRF_" ||
           tag == "ADD" || tag == "MUL" || tag == "GRF_TO_BANK" || tag == "FILL&ReLU" ||
           tag == "GRF_A_TO_EVEN_BANK" || tag == "GRF_B_TO_ODD_BANK" || tag == "MAC_" ||
           tag == "GRF_B_TO_BANK_";
}

uint64_t phase3LogicalEpoch(unsigned bank, unsigned logicalRow)
{
    if (physicalFn == nullptr)
        return 0;
    return phase3MappingEpoch(bank, phase3PhysicalRow(bank, logicalRow));
}

uint64_t phase3MappingEpoch(unsigned bank, unsigned row)
{
    if (epochFn == nullptr)
        return 0;
    return epochFn(bank, row, hookContext);
}

unsigned phase3LogicalRow(unsigned bank, unsigned row)
{
    if (logicalFn == nullptr)
        return row;
    return logicalFn(bank, row, hookContext);
}

unsigned phase3PhysicalRow(unsigned bank, unsigned row)
{
    if (physicalFn == nullptr)
        return row;
    return physicalFn(bank, row, physicalContext);
}

void phase3SetPhysicalHook(unsigned (*physical)(unsigned, unsigned, void*), void* context)
{
    physicalFn = physical;
    physicalContext = context;
}

bool phase3MappingAccepted(unsigned bank, unsigned logicalRow, uint64_t stampedEpoch)
{
    if (physicalFn == nullptr)
        return true;
    const unsigned physical = phase3PhysicalRow(bank, logicalRow);
    const unsigned inverse = phase3LogicalRow(bank, physical);
    const unsigned forward = phase3PhysicalRow(bank, inverse);
    if (inverse != logicalRow || forward != physical)
        return false;
    const uint64_t current = phase3MappingEpoch(bank, physical);
    return current == stampedEpoch;
}

void phase3SetAddressHook(uint64_t (*epoch)(unsigned, unsigned, void*),
                          unsigned (*logical)(unsigned, unsigned, void*), void* context)
{
    epochFn = epoch;
    logicalFn = logical;
    hookContext = context;
}

void phase3SetCeHook(void (*note)(unsigned, unsigned, void*), void* context)
{
    ceFn = note;
    ceContext = context;
}

void phase3NoteCe(unsigned bank, unsigned row)
{
    if (ceFn != nullptr)
        ceFn(bank, row, ceContext);
}

void phase3BindPublication(unsigned logicalRow, void* gate, unsigned operand, unsigned tile,
                           uint64_t version)
{
    DRAMSim::phase2::DataRef ref;
    ref.operand = static_cast<uint16_t>(operand);
    ref.tile = static_cast<uint32_t>(tile);
    ref.version = version;
    publication[logicalRow] = std::make_pair(static_cast<DRAMSim::phase2::Gate*>(gate), ref);
}

void phase3ClearPublication()
{
    publication.clear();
}

bool phase3ConsumptionBlocked(unsigned logicalRow)
{
    auto found = publication.find(logicalRow);
    if (found == publication.end() || found->second.first == nullptr)
        return false;
    const DRAMSim::phase2::Lifecycle state = found->second.first->state(found->second.second);
    return state == DRAMSim::phase2::Lifecycle::HELD || state == DRAMSim::phase2::Lifecycle::POISONED;
}

void phase3NoteBlockedRead()
{
    blockedReads += 1;
}

uint64_t phase3BlockedReads()
{
    return blockedReads;
}

void phase3NoteStaleRequest()
{
    staleRequests += 1;
}

uint64_t phase3StaleRequests()
{
    return staleRequests;
}

void phase3ArmIssueEpoch(uint64_t epoch)
{
    issueEpochArmed = true;
    issueEpoch = epoch;
}

bool phase3TakeIssueEpoch(uint64_t& epoch)
{
    if (!issueEpochArmed)
        return false;
    issueEpochArmed = false;
    epoch = issueEpoch;
    return true;
}

void phase3ResetHostLatency()
{
    latencies.clear();
}

void phase3NoteHostLatency(uint64_t cycles)
{
    latencies.push_back(cycles);
}

uint64_t phase3HostLatencySamples()
{
    return latencies.size();
}

double phase3HostLatencyMean()
{
    if (latencies.empty())
        return 0;
    double sum = 0;
    for (uint64_t sample : latencies)
        sum += static_cast<double>(sample);
    return sum / static_cast<double>(latencies.size());
}

double phase3HostLatencyP99()
{
    if (latencies.empty())
        return 0;
    std::vector<uint64_t> ordered = latencies;
    std::sort(ordered.begin(), ordered.end());
    const size_t index = static_cast<size_t>(std::ceil(0.99 * static_cast<double>(ordered.size()))) - 1;
    return static_cast<double>(ordered[std::min(index, ordered.size() - 1)]);
}

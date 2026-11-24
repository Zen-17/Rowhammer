#include "Phase3Bridge.h"

#include <algorithm>
#include <cmath>

namespace
{
uint64_t (*epochFn)(unsigned, unsigned, void*) = nullptr;
unsigned (*logicalFn)(unsigned, unsigned, void*) = nullptr;
void* hookContext = nullptr;
std::vector<uint64_t> latencies;
}  // namespace

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

void phase3SetAddressHook(uint64_t (*epoch)(unsigned, unsigned, void*),
                          unsigned (*logical)(unsigned, unsigned, void*), void* context)
{
    epochFn = epoch;
    logicalFn = logical;
    hookContext = context;
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

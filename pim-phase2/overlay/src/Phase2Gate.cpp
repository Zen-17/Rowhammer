#include "Phase2Gate.h"

namespace DRAMSim
{
namespace phase2
{
Lifecycle Gate::state(const DataRef& ref) const
{
    auto found = states_.find(ref);
    return found == states_.end() ? Lifecycle::HELD : found->second;
}

void Gate::hold(const DataRef& ref)
{
    auto found = states_.find(ref);
    if (found == states_.end())
    {
        states_[ref] = Lifecycle::HELD;
        return;
    }
    if (found->second != Lifecycle::CONSUMED && found->second != Lifecycle::POISONED)
        found->second = Lifecycle::HELD;
}

void Gate::publish(const DataRef& ref, uint64_t cycle)
{
    auto found = states_.find(ref);
    if (found == states_.end())
    {
        states_[ref] = Lifecycle::PUBLISHED;
        publish_cycles_[ref] = cycle;
        return;
    }
    if (found->second == Lifecycle::HELD)
    {
        found->second = Lifecycle::PUBLISHED;
        publish_cycles_[ref] = cycle;
    }
}

bool Gate::consume(const DataRef& ref, uint64_t cycle)
{
    auto found = states_.find(ref);
    if (found == states_.end() || found->second != Lifecycle::PUBLISHED)
        return false;
    found->second = Lifecycle::CONSUMED;
    publish_cycles_.insert(std::make_pair(ref, cycle));
    return true;
}

void Gate::markSuspect(const std::vector<DataRef>& refs)
{
    for (const DataRef& ref : refs)
    {
        auto found = states_.find(ref);
        if (found != states_.end() && found->second == Lifecycle::PUBLISHED)
            found->second = Lifecycle::HELD;
    }
}

void Gate::poison(const std::vector<DataRef>& refs)
{
    for (const DataRef& ref : refs)
        states_[ref] = Lifecycle::POISONED;
}

std::vector<DataRef> Gate::consumedRefs() const
{
    std::vector<DataRef> result;
    for (const auto& entry : states_)
        if (entry.second == Lifecycle::CONSUMED)
            result.push_back(entry.first);
    return result;
}

void Gate::releaseDue(uint64_t cycle, const std::map<DataRef, uint64_t>& schedule)
{
    if (frozen_)
        return;
    for (const auto& entry : schedule)
        if (cycle > entry.second)
            publish(entry.first, cycle);
}

}  // namespace phase2
}  // namespace DRAMSim

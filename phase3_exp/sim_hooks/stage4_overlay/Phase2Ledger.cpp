#include "Phase2Ledger.h"
#include "Phase2Layout.h"
#include "Phase3Bridge.h"

#include <algorithm>
#include <stdexcept>

namespace DRAMSim
{
namespace phase2
{
Ledger::Ledger(uint64_t max_log_entries)
    : max_log_entries_(std::max<uint64_t>(1, max_log_entries))
{
}

uint64_t Ledger::planOp(uint32_t job_id, uint16_t kernel_idx, uint32_t tile,
                        const std::vector<std::pair<uint16_t, uint32_t>>& input_items,
                        const std::vector<std::pair<uint16_t, uint32_t>>& output_items,
                        bool dep_complete)
{
    OpEntry entry;
    entry.op_id = next_id_++;
    entry.job_id = job_id;
    entry.kernel_idx = kernel_idx;
    entry.tile = tile;
    entry.dep_complete = dep_complete;
    entry.mapping_epoch = phase3MappingEpoch(0, 16384);
    for (const auto& item : input_items)
    {
        auto found = current_.find(item);
        const uint64_t version = found == current_.end() ? 0 : found->second;
        entry.inputs.push_back(DataRef{item.first, item.second, version});
    }
    for (const auto& item : output_items)
        entry.outputs.push_back(DataRef{item.first, item.second, 0});
    ops_.push_back(entry);
    by_id_[entry.op_id] = &ops_.back();
    final_cache_.clear();
    evictIfNeeded();
    return entry.op_id;
}

void Ledger::recordIssue(uint64_t op_id, uint64_t t_issue)
{
    OpEntry* entry = findMutable(op_id);
    if (entry == nullptr || entry->t_issue != UINT64_MAX)
        return;
    entry->t_issue = t_issue;
    for (DataRef& ref : entry->inputs)
    {
        auto found = current_.find(ref.item());
        ref.version = found == current_.end() ? 0 : found->second;
    }
    total_logged_ops_++;
}

void Ledger::recordComplete(uint64_t op_id, uint64_t t_complete)
{
    OpEntry* entry = findMutable(op_id);
    if (entry == nullptr || entry->completed)
        return;
    entry->completed = true;
    entry->t_complete = t_complete;
    for (DataRef& ref : entry->outputs)
    {
        ref.version = op_id;
        current_[ref.item()] = op_id;
        producers_[ref] = op_id;
    }
    for (const DataRef& ref : entry->inputs)
        readers_[ref].push_back(op_id);
    final_cache_.clear();
}

void Ledger::observeAccess(uint64_t op_id, unsigned row, bool is_write)
{
    if (find(op_id) == nullptr)
        return;
    if (is_write)
        observed_writes_[op_id].insert(row);
    else
        observed_reads_[op_id].insert(row);
}

bool Ledger::completeObserved(uint64_t op_id, uint64_t t_complete)
{
    OpEntry* entry = findMutable(op_id);
    if (entry == nullptr)
        return false;
    if (entry->completed)
        return true;
    std::set<unsigned> expected_reads;
    std::set<unsigned> expected_writes;
    for (const DataRef& ref : entry->inputs)
    {
        if (!hasBaseRow(ref.operand))
            return false;
        expected_reads.insert(baseRow(ref.operand) + tileRowOffset(ref.tile));
    }
    for (const DataRef& ref : entry->outputs)
    {
        if (!hasBaseRow(ref.operand))
            return false;
        expected_writes.insert(baseRow(ref.operand) + tileRowOffset(ref.tile));
    }
    const std::set<unsigned>& seen_reads = observed_reads_[op_id];
    const std::set<unsigned>& seen_writes = observed_writes_[op_id];
    if (seen_reads != expected_reads || seen_writes != expected_writes)
        return false;
    recordComplete(op_id, t_complete);
    return entry->completed;
}

uint64_t Ledger::addCompleted(uint32_t job_id, uint16_t kernel_idx, uint32_t tile,
                              const std::vector<std::pair<uint16_t, uint32_t>>& input_items,
                              const std::vector<std::pair<uint16_t, uint32_t>>& output_items,
                              uint64_t t_issue, uint64_t t_complete, bool dep_complete)
{
    const uint64_t op_id = planOp(job_id, kernel_idx, tile, input_items, output_items,
                                  dep_complete);
    recordIssue(op_id, t_issue);
    recordComplete(op_id, t_complete);
    return op_id;
}

const OpEntry* Ledger::find(uint64_t op_id) const
{
    auto found = by_id_.find(op_id);
    return found == by_id_.end() ? nullptr : found->second;
}

OpEntry* Ledger::findMutable(uint64_t op_id)
{
    auto found = by_id_.find(op_id);
    return found == by_id_.end() ? nullptr : found->second;
}

uint64_t Ledger::currentVersion(uint16_t operand, uint32_t tile) const
{
    auto found = current_.find(std::make_pair(operand, tile));
    return found == current_.end() ? 0 : found->second;
}

unsigned Ledger::baseRow(uint16_t operand) const
{
    auto found = base_rows_.find(operand);
    if (found == base_rows_.end())
        throw std::out_of_range("Phase 2 operand has no base row");
    return found->second;
}

bool Ledger::producerOf(const DataRef& ref, uint64_t& op_id) const
{
    auto found = producers_.find(ref);
    if (found == producers_.end())
        return false;
    op_id = found->second;
    return true;
}

std::vector<uint64_t> Ledger::readersOf(const DataRef& ref) const
{
    auto found = readers_.find(ref);
    return found == readers_.end() ? std::vector<uint64_t>() : found->second;
}

std::vector<DataRef> Ledger::liveItems() const
{
    std::map<std::pair<uint16_t, uint32_t>, uint64_t> items;
    for (const OpEntry& op : ops_)
    {
        for (const DataRef& ref : op.inputs)
        {
            auto found = current_.find(ref.item());
            items[ref.item()] = found == current_.end() ? 0 : found->second;
        }
        if (op.completed)
        {
            for (const DataRef& ref : op.outputs)
            {
                auto found = current_.find(ref.item());
                items[ref.item()] = found == current_.end() ? ref.version : found->second;
            }
        }
    }
    std::vector<DataRef> result;
    result.reserve(items.size());
    for (const auto& entry : items)
        result.push_back(DataRef{entry.first.first, entry.first.second, entry.second});
    return result;
}

std::vector<uint64_t> Ledger::jobs() const
{
    std::set<uint64_t> ids;
    for (const OpEntry& op : ops_)
        ids.insert(op.job_id);
    return std::vector<uint64_t>(ids.begin(), ids.end());
}

std::vector<uint64_t> Ledger::completedOpIds() const
{
    std::vector<uint64_t> result;
    for (const OpEntry& op : ops_)
        if (op.completed)
            result.push_back(op.op_id);
    return result;
}

std::vector<uint64_t> Ledger::remainingOpIds() const
{
    std::vector<uint64_t> result;
    for (const OpEntry& op : ops_)
        if (!op.completed)
            result.push_back(op.op_id);
    return result;
}

std::vector<uint64_t> Ledger::jobCompletedOpIds(uint32_t job_id) const
{
    std::vector<uint64_t> result;
    for (const OpEntry& op : ops_)
        if (op.job_id == job_id && op.completed)
            result.push_back(op.op_id);
    return result;
}

std::vector<DataRef> Ledger::jobInitialInputs(uint32_t job_id) const
{
    std::map<std::pair<uint16_t, uint32_t>, DataRef> items;
    for (const OpEntry& op : ops_)
    {
        if (op.job_id != job_id)
            continue;
        for (const DataRef& ref : op.inputs)
        {
            const uint64_t version = currentVersion(ref.operand, ref.tile);
            if (version == 0)
                items[ref.item()] = DataRef{ref.operand, ref.tile, 0};
        }
    }
    std::vector<DataRef> result;
    for (const auto& entry : items)
        result.push_back(entry.second);
    return result;
}

const std::set<std::pair<uint16_t, uint32_t>>& Ledger::finalItemsFor(uint32_t job_id) const
{
    auto found = final_cache_.find(job_id);
    if (found != final_cache_.end())
        return found->second;
    std::set<std::pair<uint16_t, uint32_t>> produced;
    std::set<std::pair<uint16_t, uint32_t>> consumed;
    for (const OpEntry& op : ops_)
    {
        if (op.job_id != job_id)
            continue;
        for (const DataRef& ref : op.inputs)
            consumed.insert(ref.item());
        if (op.completed)
            for (const DataRef& ref : op.outputs)
                produced.insert(ref.item());
    }
    std::set<std::pair<uint16_t, uint32_t>> result;
    for (const auto& item : produced)
        if (consumed.find(item) == consumed.end())
            result.insert(item);
    final_cache_[job_id] = result;
    return final_cache_[job_id];
}

bool Ledger::isFinalItem(uint32_t job_id, uint16_t operand, uint32_t tile) const
{
    return finalItemsFor(job_id).find(std::make_pair(operand, tile)) !=
           finalItemsFor(job_id).end();
}

std::vector<std::pair<uint16_t, uint32_t>> Ledger::finalItems(uint32_t job_id) const
{
    const auto& items = finalItemsFor(job_id);
    return std::vector<std::pair<uint16_t, uint32_t>>(items.begin(), items.end());
}

std::vector<std::string> Ledger::writeDisciplineReasons() const
{
    std::vector<std::string> reasons;
    bool in_place = false;
    std::set<std::pair<uint32_t, std::pair<uint16_t, uint32_t>>> produced;
    for (const OpEntry& op : ops_)
    {
        std::set<std::pair<uint16_t, uint32_t>> input_items;
        for (const DataRef& ref : op.inputs)
            input_items.insert(ref.item());
        for (const DataRef& ref : op.outputs)
        {
            if (input_items.find(ref.item()) != input_items.end())
                in_place = true;
            const auto key = std::make_pair(op.job_id, ref.item());
            if (!produced.insert(key).second)
                in_place = true;
        }
    }
    if (in_place)
        reasons.push_back("IN_PLACE_UNSUPPORTED");
    return reasons;
}

uint64_t Ledger::bytesEstimate() const
{
    uint64_t total = 0;
    for (const OpEntry& op : ops_)
        total += 8 + 4 + 2 + 4 + 16 + 1 +
                 8 * (op.inputs.size() + op.outputs.size());
    return total;
}

void Ledger::evictIfNeeded()
{
    while (ops_.size() > max_log_entries_)
    {
        const uint64_t op_id = ops_.front().op_id;
        by_id_.erase(op_id);
        ops_.pop_front();
        truncated_ = true;
    }
}

}  // namespace phase2
}  // namespace DRAMSim

#include "Phase1Feeder.h"

#include <algorithm>
#include <stdexcept>

#include "Phase1Trace.h"

namespace DRAMSim
{
namespace
{
bool isPimBroadcastTag(const std::string& tag)
{
    return tag == "BANK_TO_GRF_" || tag == "ADD" || tag == "GRF_TO_BANK" ||
           tag == "FILL&ReLU" || tag == "GRF_A_TO_EVEN_BANK" || tag == "GRF_B_TO_ODD_BANK" ||
           tag == "MAC_" || tag == "GRF_B_TO_BANK_" || tag == "RESET_GRF_B";
}
}  // namespace

Phase1FeederMemory::Phase1FeederMemory(const std::string& deviceIniFilename,
                                       const std::string& systemIniFilename,
                                       const std::string& pwd, const std::string& traceFilename,
                                       unsigned megsOfMemory, unsigned outstandingLimit)
    : MultiChannelMemorySystem(deviceIniFilename, systemIniFilename, pwd, traceFilename,
                               megsOfMemory),
      outstanding_limit_(outstandingLimit)
{
    if (outstanding_limit_ == 0)
        throw std::invalid_argument("feeder outstanding limit must be non-zero");
    outstanding_.assign(channels.size(), 0);
    frozen_.assign(channels.size(), false);
    read_callback_.reset(
        new Callback<Phase1FeederMemory, void, unsigned, uint64_t, uint64_t>(
            this, &Phase1FeederMemory::onReadComplete));
    write_callback_.reset(
        new Callback<Phase1FeederMemory, void, unsigned, uint64_t, uint64_t>(
            this, &Phase1FeederMemory::onWriteComplete));
    RegisterCallbacks(read_callback_.get(), write_callback_.get(), nullptr);
}

bool Phase1FeederMemory::addTransaction(Transaction* trans)
{
    if (trans == nullptr)
        return false;
    const bool isWrite = (trans->transactionType == DATA_WRITE);
    if (trans->transactionType != DATA_WRITE && trans->transactionType != DATA_READ)
    {
        delete trans;
        return false;
    }
    const bool queued = enqueue(isWrite, trans->address, trans->tag, trans->data);
    delete trans;
    return queued;
}

bool Phase1FeederMemory::addTransaction(bool isWrite, uint64_t addr, BurstType* data)
{
    return enqueue(isWrite, addr, "", data);
}

bool Phase1FeederMemory::addTransaction(bool isWrite, uint64_t addr, const std::string& tag,
                                        BurstType* data)
{
    return enqueue(isWrite, addr, tag, data);
}

bool Phase1FeederMemory::addBarrier(int chanId)
{
    if (chanId < 0 || static_cast<size_t>(chanId) >= channels.size())
        throw std::out_of_range("feeder barrier channel out of range");
    auto item = std::unique_ptr<Item>(new Item());
    item->kind = Item::Kind::BARRIER;
    item->channel = static_cast<unsigned>(chanId);
    item->sequence = next_sequence_++;
    const uint64_t sequence = item->sequence;
    pending_.push_back(std::move(item));
    max_pending_ = std::max(max_pending_, pending_.size());
    if (trace_ != nullptr)
        trace_->event(currentClockCycle, "ENQUEUE", "BARRIER_ENQUEUE", "1", "1",
                      std::to_string(sequence), -1, -1, -1, -1, -1, "", -1, "queued");
    return true;
}

void Phase1FeederMemory::decodeAddress(uint64_t address, unsigned& channel, unsigned& rank,
                                       unsigned& bank, unsigned& row, unsigned& column) const
{
    addrMapping->addressMapping(address, channel, rank, bank, row, column);
    if (channel >= channels.size())
        throw std::out_of_range("feeder address maps outside channel range");
}

bool Phase1FeederMemory::enqueue(bool isWrite, uint64_t address, const std::string& tag,
                                 BurstType* data)
{
    if (data == nullptr)
        throw std::invalid_argument("feeder transaction has null payload/buffer");
    auto item = std::unique_ptr<Item>(new Item());
    item->kind = Item::Kind::TRANSACTION;
    item->is_write = isWrite;
    item->address = address;
    decodeAddress(address, item->channel, item->rank, item->bank, item->row, item->column);
    item->tag = tag;
    item->access_path = tag;
    if (tag == "PROGRAM_CRF" || tag == "PIM")
    {
        item->location = Item::LocationKind::PIM_CONTROL;
        item->access_path = tag + ":PIM_CONTROL";
    }
    else if (isPimBroadcastTag(tag))
    {
        item->location = Item::LocationKind::PIM_BROADCAST;
        item->access_path = tag + ":PIM_BROADCAST";
    }
    if (isWrite)
        item->payload = *data;  // stable ownership of the write payload
    else
        item->read_buffer = data;  // caller-owned receive buffer, kept alive by caller
    item->sequence = next_sequence_++;
    traceItem(*item, "ENQUEUE", "DESCRIPTOR_ENQUEUE", "queued");
    pending_.push_back(std::move(item));
    max_pending_ = std::max(max_pending_, pending_.size());
    return true;
}

void Phase1FeederMemory::traceItem(const Item& item, const std::string& phase,
                                   const std::string& eventType, const std::string& status,
                                   int cycleOffset) const
{
    if (trace_ == nullptr)
        return;
    const int64_t cycle = static_cast<int64_t>(currentClockCycle) + cycleOffset;
    const bool hasBank = item.location == Item::LocationKind::NORMAL;
    const bool hasRowCol = item.location != Item::LocationKind::PIM_CONTROL;
    trace_->event(cycle < 0 ? 0 : static_cast<uint64_t>(cycle), phase, eventType, "1", "1",
                  std::to_string(item.sequence), static_cast<int>(item.channel),
                  static_cast<int>(item.rank), hasBank ? static_cast<int>(item.bank) : -1,
                  hasRowCol ? static_cast<int>(item.row) : -1,
                  hasRowCol ? static_cast<int>(item.column) : -1, item.access_path, 32, status);
}

void Phase1FeederMemory::traceNoItem(const std::string& phase, const std::string& eventType,
                                     const std::string& status, int cycleOffset) const
{
    if (trace_ == nullptr)
        return;
    const int64_t cycle = static_cast<int64_t>(currentClockCycle) + cycleOffset;
    trace_->event(cycle < 0 ? 0 : static_cast<uint64_t>(cycle), phase, eventType, "1", "1", "", -1,
                  -1, -1, -1, -1, "", -1, status);
}

bool Phase1FeederMemory::barrierReady(unsigned channel) const
{
    if (channel >= outstanding_.size() || outstanding_[channel] != 0)
        return false;
    if (MultiChannelMemorySystem::hasPendingTransactions() != 0)
        return false;
    return rankAndControllerIdle();
}

bool Phase1FeederMemory::rankAndControllerIdle() const
{
    for (size_t c = 0; c < channels.size(); c++)
    {
        const MemorySystem* memory = channels[c];
        if (memory == nullptr)
            return false;
        if (!memory->pendingTransactions.empty())
            return false;
        if (memory->memoryController == nullptr ||
            !memory->memoryController->transactionQueue.empty())
            return false;
        if (memory->ranks == nullptr)
            return false;
        for (Rank* rank : *memory->ranks)
        {
            if (rank == nullptr || rank->outgoingDataPacket != nullptr ||
                !rank->readReturnPacket.empty() || !rank->readReturnCountdown.empty())
                return false;
        }
    }
    return true;
}

bool Phase1FeederMemory::tick()
{
    if (pending_.empty())
        return false;

    Item& front = *pending_.front();
    if (front.channel < frozen_.size() && frozen_[front.channel])
        return false;
    if (front.kind == Item::Kind::BARRIER)
    {
        if (!barrierReady(front.channel))
            return false;
        // With no accepted transaction left, the upstream barrier has nothing
        // to attach to; the conservative feeder order already guarantees the
        // prior stage is quiesced.
        MultiChannelMemorySystem::addBarrier(static_cast<int>(front.channel));
        traceNoItem("FEEDER", "BARRIER_RETIRED", "retired");
        pending_.pop_front();
        barriers_retired_++;
        return true;
    }

    const unsigned channel = front.channel;
    if (channel >= outstanding_.size())
        throw std::out_of_range("feeder channel out of range");
    if (outstanding_[channel] >= outstanding_limit_)
        return false;
    if (!MultiChannelMemorySystem::willAcceptTransaction(front.address))
    {
        controller_rejections_++;
        traceItem(front, "FEEDER", "CONTROLLER_REJECT", "rejected");
        return false;
    }

    BurstType* data = front.is_write ? &front.payload : front.read_buffer;
    if (data == nullptr)
        throw std::logic_error("feeder transaction has no data pointer");
    if (!MultiChannelMemorySystem::addTransaction(front.is_write, front.address, front.tag, data))
    {
        controller_rejections_++;
        traceItem(front, "FEEDER", "CONTROLLER_REJECT", "rejected");
        return false;
    }

    outstanding_[channel]++;
    max_outstanding_ = std::max(max_outstanding_, outstandingCount());
    traceItem(front, "FEEDER", "CONTROLLER_ACCEPT", "accepted");
    if (completion_observer_)
    {
        ObserverRecord record;
        record.sequence = front.sequence;
        record.is_write = front.is_write;
        record.row = front.row;
        record.column = front.column;
        record.tag = front.tag;
        record.submit_cycle = currentClockCycle;
        observer_ids_.insert(std::make_pair(front.address, record));
    }
    if (trace_ != nullptr && trace_->enabled())
    {
        CompletionRecord record;
        record.sequence = front.sequence;
        record.location = front.location;
        record.access_path = front.access_path;
        completion_ids_.insert(std::make_pair(front.address, record));
    }
    accepted_.push_back(std::move(pending_.front()));
    pending_.pop_front();
    return true;
}

bool Phase1FeederMemory::tickChannel(unsigned channel)
{
    if (channel >= outstanding_.size() || frozen_[channel])
        return false;
    for (auto it = pending_.begin(); it != pending_.end(); ++it)
    {
        Item& item = **it;
        if (item.channel != channel)
            continue;
        if (item.kind == Item::Kind::BARRIER)
            return false;  // barrier handling is only through tick()
        if (outstanding_[channel] >= outstanding_limit_)
            return false;
        if (!MultiChannelMemorySystem::willAcceptTransaction(item.address))
        {
            controller_rejections_++;
            traceItem(item, "FEEDER", "CONTROLLER_REJECT", "rejected");
            return false;
        }
        BurstType* data = item.is_write ? &item.payload : item.read_buffer;
        if (data == nullptr)
            throw std::logic_error("feeder transaction has no data pointer");
        if (!MultiChannelMemorySystem::addTransaction(item.is_write, item.address, item.tag, data))
        {
            controller_rejections_++;
            traceItem(item, "FEEDER", "CONTROLLER_REJECT", "rejected");
            return false;
        }
        outstanding_[channel]++;
        max_outstanding_ = std::max(max_outstanding_, outstandingCount());
        traceItem(item, "FEEDER", "CONTROLLER_ACCEPT", "accepted");
        if (completion_observer_)
        {
            ObserverRecord record;
            record.sequence = item.sequence;
            record.is_write = item.is_write;
            record.row = item.row;
            record.column = item.column;
            record.tag = item.tag;
            record.submit_cycle = currentClockCycle;
            observer_ids_.insert(std::make_pair(item.address, record));
        }
        if (trace_ != nullptr && trace_->enabled())
        {
            CompletionRecord record;
            record.sequence = item.sequence;
            record.location = item.location;
            record.access_path = item.access_path;
            completion_ids_.insert(std::make_pair(item.address, record));
        }
        accepted_.push_back(std::move(*it));
        pending_.erase(it);
        return true;
    }
    return false;
}

void Phase1FeederMemory::freezeChannel(unsigned channel)
{
    if (channel >= frozen_.size())
        throw std::out_of_range("feeder freeze channel out of range");
    frozen_[channel] = true;
}

void Phase1FeederMemory::unfreezeChannel(unsigned channel)
{
    if (channel >= frozen_.size())
        throw std::out_of_range("feeder unfreeze channel out of range");
    frozen_[channel] = false;
}

std::size_t Phase1FeederMemory::clearPending()
{
    const std::size_t count = pending_.size();
    pending_.clear();
    return count;
}

bool Phase1FeederMemory::isFrozen(unsigned channel) const
{
    return channel < frozen_.size() && frozen_[channel];
}

bool Phase1FeederMemory::channelIdle(unsigned channel) const
{
    if (channel >= outstanding_.size() || outstanding_[channel] != 0)
        return false;
    const MemorySystem* memory = channels[channel];
    if (memory == nullptr || !memory->pendingTransactions.empty())
        return false;
    if (memory->memoryController == nullptr ||
        !memory->memoryController->transactionQueue.empty())
        return false;
    if (memory->ranks == nullptr)
        return false;
    for (const Rank* rank : *memory->ranks)
        if (rank == nullptr || rank->outgoingDataPacket != nullptr ||
            !rank->readReturnPacket.empty() || !rank->readReturnCountdown.empty())
            return false;
    return true;
}

bool Phase1FeederMemory::isIdle() const
{
    if (!pending_.empty() || outstandingCount() != 0)
        return false;
    if (MultiChannelMemorySystem::hasPendingTransactions() != 0)
        return false;
    return rankAndControllerIdle();
}

std::size_t Phase1FeederMemory::outstandingCount() const
{
    std::size_t total = 0;
    for (unsigned value : outstanding_)
        total += value;
    return total;
}

void Phase1FeederMemory::onReadComplete(unsigned systemID, uint64_t address, uint64_t cycle)
{
    (void)cycle;
    if (systemID < outstanding_.size() && outstanding_[systemID] > 0)
        outstanding_[systemID]--;
    if (completion_observer_)
    {
        auto found = observer_ids_.find(address);
        if (found != observer_ids_.end())
        {
            P1CompletionEvent event;
            event.sequence = found->second.sequence;
            event.is_write = false;
            event.row = found->second.row;
            event.column = found->second.column;
            event.tag = found->second.tag;
            event.submit_cycle = found->second.submit_cycle;
            event.complete_cycle = currentClockCycle;
            completion_observer_(event);
            observer_ids_.erase(found);
        }
    }
    if (trace_ != nullptr && trace_->enabled())
    {
        auto found = completion_ids_.find(address);
        if (found != completion_ids_.end())
        {
            Item item;
            item.address = address;
            item.sequence = found->second.sequence;
            item.location = found->second.location;
            item.access_path = found->second.access_path;
            decodeAddress(address, item.channel, item.rank, item.bank, item.row, item.column);
            traceItem(item, "COMPLETE", "READ_COMPLETE", "completed");
            completion_ids_.erase(found);
        }
    }
}

void Phase1FeederMemory::onWriteComplete(unsigned systemID, uint64_t address, uint64_t cycle)
{
    (void)cycle;
    if (systemID < outstanding_.size() && outstanding_[systemID] > 0)
        outstanding_[systemID]--;
    if (completion_observer_)
    {
        auto found = observer_ids_.find(address);
        if (found != observer_ids_.end())
        {
            P1CompletionEvent event;
            event.sequence = found->second.sequence;
            event.is_write = true;
            event.row = found->second.row;
            event.column = found->second.column;
            event.tag = found->second.tag;
            event.submit_cycle = found->second.submit_cycle;
            event.complete_cycle = currentClockCycle;
            completion_observer_(event);
            observer_ids_.erase(found);
        }
    }
    if (trace_ != nullptr && trace_->enabled())
    {
        auto found = completion_ids_.find(address);
        if (found != completion_ids_.end())
        {
            Item item;
            item.address = address;
            item.sequence = found->second.sequence;
            item.location = found->second.location;
            item.access_path = found->second.access_path;
            decodeAddress(address, item.channel, item.rank, item.bank, item.row, item.column);
            traceItem(item, "COMPLETE", "WRITE_DATA_DONE", "data_bus_done");
            completion_ids_.erase(found);
        }
    }
}

}  // namespace DRAMSim

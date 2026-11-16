#ifndef PHASE1_FEEDER_H
#define PHASE1_FEEDER_H

#include <cstddef>
#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "Callback.h"
#include "MultiChannelMemorySystem.h"

namespace DRAMSim
{
class Phase1Trace;

// Experimental bounded transaction feeder.
//
// This is a Phase1 interface model: it does not claim that a real HBM-PIM
// controller exposes a corresponding external submission queue.  The class
// keeps the external pending queue separate from the accepted/in-flight
// transactions counted by the upstream simulator.
class Phase1FeederMemory : public MultiChannelMemorySystem
{
  public:
    Phase1FeederMemory(const std::string& deviceIniFilename,
                       const std::string& systemIniFilename, const std::string& pwd,
                       const std::string& traceFilename, unsigned megsOfMemory,
                       unsigned outstandingLimit = 8);

    bool addTransaction(Transaction* trans) override;
    bool addTransaction(bool isWrite, uint64_t addr, BurstType* data) override;
    bool addTransaction(bool isWrite, uint64_t addr, const std::string& tag,
                        BurstType* data) override;
    bool addBarrier(int chanId) override;

    // Submit at most one external item.  Returns true iff an item was
    // submitted/consumed or a barrier was retired.
    bool tick();
    // Submit at most one external item for a specific channel.  Used by the
    // two-channel pause smoke; per-channel order is retained.
    bool tickChannel(unsigned channel);

    void freezeChannel(unsigned channel);
    void unfreezeChannel(unsigned channel);
    // G7 discard of old uncommitted external descriptors.  Accepted/in-flight
    // transactions must already be drained by quiesce.
    std::size_t clearPending();
    bool isFrozen(unsigned channel) const;
    bool channelIdle(unsigned channel) const;

    bool isIdle() const;
    std::size_t pendingCount() const { return pending_.size(); }
    std::size_t acceptedCount() const { return accepted_.size(); }
    std::size_t outstandingCount() const;
    std::size_t maxPending() const { return max_pending_; }
    std::size_t maxOutstanding() const { return max_outstanding_; }
    uint64_t controllerRejections() const { return controller_rejections_; }
    uint64_t barriersRetired() const { return barriers_retired_; }

    void setTrace(Phase1Trace* trace) { trace_ = trace; }

    // Callbacks are installed on each upstream MemorySystem.  They are used
    // only for the feeder's own accounting; they do not alter runtime state.
    void onReadComplete(unsigned systemID, uint64_t address, uint64_t cycle);
    void onWriteComplete(unsigned systemID, uint64_t address, uint64_t cycle);

  private:
    struct Item
    {
        enum class Kind
        {
            TRANSACTION,
            BARRIER
        };
        enum class LocationKind
        {
            NORMAL,
            PIM_BROADCAST,
            PIM_CONTROL
        };
        Kind kind = Kind::TRANSACTION;
        bool is_write = false;
        uint64_t address = 0;
        unsigned channel = 0;
        unsigned rank = 0;
        unsigned bank = 0;
        unsigned row = 0;
        unsigned column = 0;
        LocationKind location = LocationKind::NORMAL;
        std::string tag;
        std::string access_path;
        BurstType payload;           // stable copy for writes
        BurstType* read_buffer = nullptr;  // caller-owned receive buffer
        uint64_t sequence = 0;
    };

    struct CompletionRecord
    {
        uint64_t sequence = 0;
        Item::LocationKind location = Item::LocationKind::NORMAL;
        std::string access_path;
    };

    bool enqueue(bool isWrite, uint64_t address, const std::string& tag, BurstType* data);
    void decodeAddress(uint64_t address, unsigned& channel, unsigned& rank, unsigned& bank,
                       unsigned& row, unsigned& column) const;
    bool barrierReady(unsigned channel) const;
    bool rankAndControllerIdle() const;
    void traceItem(const Item& item, const std::string& phase, const std::string& eventType,
                   const std::string& status, int cycleOffset = 0) const;
    void traceNoItem(const std::string& phase, const std::string& eventType,
                     const std::string& status, int cycleOffset = 0) const;

    std::list<std::unique_ptr<Item>> pending_;
    std::list<std::unique_ptr<Item>> accepted_;
    std::vector<unsigned> outstanding_;
    std::vector<bool> frozen_;
    unsigned outstanding_limit_ = 8;
    uint64_t next_sequence_ = 0;
    uint64_t controller_rejections_ = 0;
    uint64_t barriers_retired_ = 0;
    std::size_t max_pending_ = 0;
    std::size_t max_outstanding_ = 0;
    Phase1Trace* trace_ = nullptr;
    std::multimap<uint64_t, CompletionRecord> completion_ids_;
    std::unique_ptr<TransactionCompleteCB> read_callback_;
    std::unique_ptr<TransactionCompleteCB> write_callback_;
};
}  // namespace DRAMSim

#endif

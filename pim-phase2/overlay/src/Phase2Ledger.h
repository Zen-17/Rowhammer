#ifndef PHASE2_LEDGER_H
#define PHASE2_LEDGER_H

#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "Phase2Types.h"

namespace DRAMSim
{
namespace phase2
{
class Ledger
{
  public:
    explicit Ledger(uint64_t max_log_entries = 100000);

    uint64_t planOp(uint32_t job_id, uint16_t kernel_idx, uint32_t tile,
                    const std::vector<std::pair<uint16_t, uint32_t>>& input_items,
                    const std::vector<std::pair<uint16_t, uint32_t>>& output_items,
                    bool dep_complete = true);
    void recordIssue(uint64_t op_id, uint64_t t_issue);
    void recordComplete(uint64_t op_id, uint64_t t_complete);
    void setBaseRow(uint16_t operand, unsigned base_row) { base_rows_[operand] = base_row; }
    bool hasBaseRow(uint16_t operand) const
    {
        return base_rows_.find(operand) != base_rows_.end();
    }
    unsigned baseRow(uint16_t operand) const;
    uint64_t addCompleted(uint32_t job_id, uint16_t kernel_idx, uint32_t tile,
                          const std::vector<std::pair<uint16_t, uint32_t>>& input_items,
                          const std::vector<std::pair<uint16_t, uint32_t>>& output_items,
                          uint64_t t_issue, uint64_t t_complete,
                          bool dep_complete = true);

    const std::deque<OpEntry>& ops() const { return ops_; }
    const OpEntry* find(uint64_t op_id) const;
    OpEntry* findMutable(uint64_t op_id);
    uint64_t currentVersion(uint16_t operand, uint32_t tile) const;
    bool producerOf(const DataRef& ref, uint64_t& op_id) const;
    std::vector<uint64_t> readersOf(const DataRef& ref) const;

    std::vector<DataRef> liveItems() const;
    void recordHostRead(const HostRead& read) { host_reads_.push_back(read); }
    const std::vector<HostRead>& hostReads() const { return host_reads_; }
    std::vector<uint64_t> jobs() const;
    std::vector<uint64_t> completedOpIds() const;
    std::vector<uint64_t> remainingOpIds() const;
    std::vector<uint64_t> jobCompletedOpIds(uint32_t job_id) const;
    std::vector<DataRef> jobInitialInputs(uint32_t job_id) const;
    bool isFinalItem(uint32_t job_id, uint16_t operand, uint32_t tile) const;
    std::vector<std::pair<uint16_t, uint32_t>> finalItems(uint32_t job_id) const;
    std::vector<std::string> writeDisciplineReasons() const;

    uint64_t bytesEstimate() const;
    uint64_t totalLoggedOps() const { return total_logged_ops_; }
    uint64_t peakHeldItems() const { return current_.size(); }
    bool truncated() const { return truncated_; }
    void truncateForTest() { truncated_ = true; }

  private:
    void evictIfNeeded();
    const std::set<std::pair<uint16_t, uint32_t>>& finalItemsFor(uint32_t job_id) const;

    uint64_t max_log_entries_ = 100000;
    std::deque<OpEntry> ops_;
    std::map<uint64_t, OpEntry*> by_id_;
    std::map<std::pair<uint16_t, uint32_t>, uint64_t> current_;
    std::vector<HostRead> host_reads_;
    std::map<uint16_t, unsigned> base_rows_;
    std::map<DataRef, std::vector<uint64_t>> readers_;
    std::map<DataRef, uint64_t> producers_;
    mutable std::map<uint32_t, std::set<std::pair<uint16_t, uint32_t>>> final_cache_;
    uint64_t next_id_ = 1;
    uint64_t total_logged_ops_ = 0;
    bool truncated_ = false;
};

}  // namespace phase2
}  // namespace DRAMSim

#endif

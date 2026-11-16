#ifndef PHASE1_REPORTS_H
#define PHASE1_REPORTS_H

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "Phase1Address.h"
#include "Phase1Ecc.h"
#include "Phase1Profile.h"

namespace DRAMSim
{
struct Phase1Detection
{
    uint64_t detect_cycle = 0;
    unsigned channel = 0;
    unsigned rank = 0;
    unsigned actual_bank = 0;
    unsigned row = 0;
    unsigned burst_column = 0;
    unsigned codeword_index = 0;
    Phase1EccStatus status = Phase1EccStatus::CLEAN;
    Phase1AccessType access = Phase1AccessType::HOST;
};

struct Phase1FormatEvent
{
    uint64_t detect_cycle = 0;
    unsigned channel = 0;
    unsigned rank = 0;
    unsigned actual_bank = 0;
    unsigned row = 0;
    unsigned burst_column = 0;
    unsigned codeword_index = 0;
    Phase1AccessType access = Phase1AccessType::HOST;
};

struct Phase1PublicReport
{
    uint64_t report_seq = 0;
    uint64_t report_cycle = 0;
    std::string error_class;
    std::string region_type;
    uint64_t region_id = 0;
};

class Phase1ReportSink
{
  public:
    virtual ~Phase1ReportSink() {}
    virtual void onDetected(const Phase1Detection& detection) = 0;
    virtual void onUnprotectedFormat(const Phase1FormatEvent& event) = 0;
};

// Coarse report queue.  It has no reference to runtime, golden output or fault
// schedules.  The CSV contains only the public fields; detect_cycle remains
// internal and is used only for latency accounting in the test harness.
class Phase1Reports : public Phase1ReportSink
{
  public:
    Phase1Reports(const Phase1Address& address, unsigned num_channels, unsigned num_banks,
                  const std::string& granularity, uint64_t transport_delay,
                  uint64_t service_interval, unsigned capacity, bool deliver_ce,
                  bool deliver_ue);

    void onDetected(const Phase1Detection& detection) override;
    void onUnprotectedFormat(const Phase1FormatEvent& event) override;

    // Deliver at most one due report at this cycle.  Call once per DRAM cycle.
    void tick(uint64_t cycle);
    void writeCsv(const std::string& path) const;

    const std::vector<Phase1PublicReport>& reports() const { return reports_; }
    const std::vector<uint64_t>& reportLatencies() const { return latencies_; }
    std::size_t pendingCount() const { return pending_.size(); }
    uint64_t internalDetections() const { return internal_detections_; }
    uint64_t internalCe() const { return internal_ce_; }
    uint64_t internalUe() const { return internal_ue_; }
    uint64_t delivered() const { return delivered_; }
    uint64_t dropped() const { return dropped_; }
    uint64_t formatEvents() const { return format_events_; }
    bool overflow() const { return overflow_; }
    std::size_t maxQueueDepth() const { return max_queue_depth_; }
    std::size_t droppedReports() const { return dropped_; }

  private:
    struct Pending
    {
        uint64_t detect_cycle = 0;
        uint64_t ready_cycle = 0;
        uint64_t arrival_seq = 0;
        std::string error_class;
        std::string region_type;
        uint64_t region_id = 0;
    };

    Phase1Address address_;
    unsigned num_channels_ = 0;
    unsigned num_banks_ = 0;
    std::string granularity_;
    uint64_t transport_delay_ = 0;
    uint64_t service_interval_ = 1;
    unsigned capacity_ = 64;
    bool deliver_ce_ = true;
    bool deliver_ue_ = true;

    std::deque<Pending> pending_;
    std::vector<Phase1PublicReport> reports_;
    std::vector<uint64_t> latencies_;
    uint64_t next_service_cycle_ = 0;
    uint64_t next_report_seq_ = 1;
    uint64_t next_arrival_seq_ = 1;
    uint64_t internal_detections_ = 0;
    uint64_t internal_ce_ = 0;
    uint64_t internal_ue_ = 0;
    uint64_t delivered_ = 0;
    uint64_t dropped_ = 0;
    uint64_t format_events_ = 0;
    bool overflow_ = false;
    std::size_t max_queue_depth_ = 0;
};
}  // namespace DRAMSim

#endif

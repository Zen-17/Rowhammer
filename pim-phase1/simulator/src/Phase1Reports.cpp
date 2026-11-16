#include "Phase1Reports.h"

#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace DRAMSim
{
Phase1Reports::Phase1Reports(const Phase1Address& address, unsigned num_channels,
                             unsigned num_banks, const std::string& granularity,
                             uint64_t transport_delay, uint64_t service_interval, unsigned capacity,
                             bool deliver_ce, bool deliver_ue)
    : address_(address),
      num_channels_(num_channels),
      num_banks_(num_banks),
      granularity_(granularity),
      transport_delay_(transport_delay),
      service_interval_(std::max<uint64_t>(1, service_interval)),
      capacity_(capacity),
      deliver_ce_(deliver_ce),
      deliver_ue_(deliver_ue)
{
    if (granularity_ != "bank" && granularity_ != "page")
        throw std::invalid_argument("report granularity must be bank or page");
    if (capacity_ == 0)
        throw std::invalid_argument("report queue capacity must be non-zero");
}

void Phase1Reports::onDetected(const Phase1Detection& detection)
{
    if (detection.status != Phase1EccStatus::CORRECTED &&
        detection.status != Phase1EccStatus::UNCORRECTABLE)
        return;

    internal_detections_++;
    const bool is_ce = (detection.status == Phase1EccStatus::CORRECTED);
    if (is_ce)
        internal_ce_++;
    else
        internal_ue_++;
    if ((is_ce && !deliver_ce_) || (!is_ce && !deliver_ue_))
        return;

    Pending item;
    item.detect_cycle = detection.detect_cycle;
    item.ready_cycle = detection.detect_cycle + std::max<uint64_t>(1, transport_delay_);
    item.arrival_seq = next_arrival_seq_++;
    item.error_class = is_ce ? "CE" : "UE";

    Phase1AddrTuple tuple;
    tuple.channel = detection.channel;
    tuple.rank = detection.rank;
    tuple.bank = detection.actual_bank;
    tuple.row = detection.row;
    tuple.burst_column = detection.burst_column;
    if (granularity_ == "bank")
    {
        item.region_type = "bank";
        item.region_id = static_cast<uint64_t>(detection.channel) * num_banks_ +
                         detection.actual_bank;
    }
    else
    {
        item.region_type = "page";
        const uint64_t hostAddress = address_.encode(tuple);
        item.region_id = hostAddress / 4096ULL;
    }

    if (pending_.size() >= capacity_)
    {
        dropped_++;
        overflow_ = true;
        return;
    }
    pending_.push_back(item);
    max_queue_depth_ = std::max(max_queue_depth_, pending_.size());
}

void Phase1Reports::onUnprotectedFormat(const Phase1FormatEvent& event)
{
    (void)event;
    format_events_++;
}

void Phase1Reports::tick(uint64_t cycle)
{
    if (pending_.empty())
        return;
    const Pending& front = pending_.front();
    if (cycle < front.ready_cycle || cycle < next_service_cycle_)
        return;

    Phase1PublicReport report;
    report.report_seq = next_report_seq_++;
    report.report_cycle = cycle;
    report.error_class = front.error_class;
    report.region_type = front.region_type;
    report.region_id = front.region_id;
    reports_.push_back(report);
    latencies_.push_back(cycle - front.detect_cycle);
    delivered_++;
    next_service_cycle_ = cycle + service_interval_;
    pending_.pop_front();
}

void Phase1Reports::writeCsv(const std::string& path) const
{
    std::ofstream output(path, std::ios::out | std::ios::trunc);
    if (!output)
        throw std::runtime_error("cannot open reports.csv: " + path);
    output << "report_seq,report_cycle,error_class,region_type,region_id\n";
    for (const Phase1PublicReport& report : reports_)
        output << report.report_seq << ',' << report.report_cycle << ',' << report.error_class << ','
               << report.region_type << ',' << report.region_id << '\n';
}

}  // namespace DRAMSim

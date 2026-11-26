#include "Phase1Reports.h"
#include "Phase3Bridge.h"

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
    if (granularity_ != "bank" && granularity_ != "page" && granularity_ != "codeword")
        throw std::invalid_argument("report granularity must be bank, page or codeword");
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
    else if (granularity_ == "page")
    {
        item.region_type = "page";
        const uint64_t hostAddress = address_.encode(tuple);
        item.region_id = hostAddress / 4096ULL;
    }
    else
    {
        // ASSUMED codeword-granularity interface.  Deterministic linear
        // encoding of (channel, actual_bank, row, burst_column, codeword):
        //   (((channel * num_banks + bank) * num_rows + row) * burst_columns
        //     + burst_column) * 4 + codeword_index
        const Phase1Geometry& geometry = address_.geometry();
        const unsigned logicalRow = phase3LogicalRow(detection.actual_bank, detection.row);
        item.region_type = "codeword";
        item.region_id =
            (((static_cast<uint64_t>(detection.channel) * num_banks_ +
               detection.actual_bank) *
                  geometry.num_rows +
              logicalRow) *
                 geometry.burst_columns() +
             detection.burst_column) *
                4ULL +
            detection.codeword_index;
    }

    item.mapping_epoch = phase3MappingEpoch(detection.actual_bank, detection.row);
    item.logical_row = phase3LogicalRow(detection.actual_bank, detection.row);
    item.actual_bank = detection.actual_bank;
    item.mapping_consistent =
        phase3MappingAccepted(detection.actual_bank, item.logical_row, item.mapping_epoch);
    if (is_ce)
        phase3NoteCe(detection.actual_bank, detection.row);
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
    report.mapping_epoch = front.mapping_epoch;
    report.logical_row = front.logical_row;
    report.actual_bank = front.actual_bank;
    report.mapping_consistent = front.mapping_consistent;
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

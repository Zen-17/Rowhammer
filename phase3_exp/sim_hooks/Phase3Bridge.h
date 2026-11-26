#ifndef PHASE3_BRIDGE_H
#define PHASE3_BRIDGE_H

#include <cstdint>
#include <string>
#include <vector>

// Address and publication hooks shared by the bank, the report queue, and recovery.
uint64_t phase3MappingEpoch(unsigned bank, unsigned row);
unsigned phase3LogicalRow(unsigned bank, unsigned row);
unsigned phase3PhysicalRow(unsigned bank, unsigned row);
uint64_t phase3LogicalEpoch(unsigned bank, unsigned logicalRow);
bool phase3IsLogicalDataTag(const std::string& tag);
bool phase3MappingAccepted(unsigned bank, unsigned logicalRow, uint64_t stampedEpoch);
void phase3SetAddressHook(uint64_t (*epoch)(unsigned bank, unsigned row, void*),
                          unsigned (*logical)(unsigned bank, unsigned row, void*),
                          void* context);
void phase3SetPhysicalHook(unsigned (*physical)(unsigned bank, unsigned row, void*), void* context);
void phase3SetCeHook(void (*note)(unsigned bank, unsigned row, void*), void* context);
void phase3NoteCe(unsigned bank, unsigned row);
void phase3BindPublication(unsigned logicalRow, void* gate, unsigned operand, unsigned tile,
                           uint64_t version);
void phase3ClearPublication();
bool phase3ConsumptionBlocked(unsigned logicalRow);
void phase3NoteBlockedRead();
uint64_t phase3BlockedReads();
void phase3NoteStaleRequest();
uint64_t phase3StaleRequests();
// Stamp taken when a logical request enters the feeder, consumed by the next
// controller accept. A swap that lands between those two points stays visible.
void phase3ArmIssueEpoch(uint64_t epoch);
bool phase3TakeIssueEpoch(uint64_t& epoch);

void phase3ResetHostLatency();
void phase3NoteHostLatency(uint64_t cycles);
uint64_t phase3HostLatencySamples();
double phase3HostLatencyMean();
double phase3HostLatencyP99();
void phase3SetLateReportTranslation(bool enabled);
bool phase3LateReportTranslation();
void phase3SetReportEpochRelaxed(bool enabled);
bool phase3ReportEpochRelaxed();

#endif

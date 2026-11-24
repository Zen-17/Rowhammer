#ifndef PHASE3_BRIDGE_H
#define PHASE3_BRIDGE_H

#include <cstdint>
#include <vector>

// Side channel for the two M2 compute-path fields.  The Phase 1 report queue
// and the Phase 2 ledger store the values; recovery decisions do not read them.
uint64_t phase3MappingEpoch(unsigned bank, unsigned row);
unsigned phase3LogicalRow(unsigned bank, unsigned row);
void phase3SetAddressHook(uint64_t (*epoch)(unsigned bank, unsigned row, void*),
                          unsigned (*logical)(unsigned bank, unsigned row, void*),
                          void* context);

void phase3ResetHostLatency();
void phase3NoteHostLatency(uint64_t cycles);
uint64_t phase3HostLatencySamples();
double phase3HostLatencyMean();
double phase3HostLatencyP99();

#endif

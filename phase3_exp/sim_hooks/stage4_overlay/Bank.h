/*********************************************************************************
 *  Copyright (c) 2010-2011, Elliott Cooper-Balis
 *                             Paul Rosenfeld
 *                             Bruce Jacob
 *                             University of Maryland
 *                             dramninjas [at] gmail [dot] com
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *
 *     * Redistributions of source code must retain the above copyright notice,
 *        this list of conditions and the following disclaimer.
 *
 *     * Redistributions in binary form must reproduce the above copyright notice,
 *        this list of conditions and the following disclaimer in the documentation
 *        and/or other materials provided with the distribution.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 *  ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 *  WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 *  DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 *  FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 *  DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 *  SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 *  OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *********************************************************************************/

#ifndef BANK_H
#define BANK_H

#include <iostream>
#include <memory>
#include <vector>

#include "BankState.h"
#include "Burst.h"
#include "BusPacket.h"
#include "Phase1Profile.h"
#include "Phase1Reports.h"
#include "Phase1Storage.h"
#include "SimulatorObject.h"
#include "SystemConfiguration.h"

namespace DRAMSim
{
class Bank
{
    typedef struct _DataStruct
    {
        unsigned row;
        BurstType data;
        std::shared_ptr<struct _DataStruct> next;
    } DataStruct;

  public:
    // functions
    Bank(ostream& simLog);

    void read(BusPacket* busPacket, Phase1AccessType access = Phase1AccessType::HOST,
              uint64_t cycle = 0);
    void write(const BusPacket* busPacket, Phase1AccessType access = Phase1AccessType::HOST,
               uint64_t cycle = 0);

    void setIdentity(unsigned channel, unsigned rank, unsigned actual_bank);
    unsigned identityBank() const { return actualBank_; }
    void setPhase1(Phase1Profile profile, Phase1ReportSink* reportSink);
    // ASSUMED, idealized zero-cost scrub: when enabled, a corrected read
    // rewrites the corrected codeword and increments this counter.  Default
    // false preserves the pre-G13 read path byte-for-byte.
    void setPhase1Scrub(bool enabled) { phase1Scrub_ = enabled; }
    uint64_t phase1ScrubWrites() const { return phase1ScrubWrites_; }
    void injectRawDataBit(unsigned row, unsigned burstColumn, unsigned bitInBurst);
    void injectRawParityBit(unsigned row, unsigned burstColumn, unsigned codeword, unsigned bit);
    uint64_t dataAccessCount() const { return dataAccessCount_; }

    // v1 swap: move the two physical rows and scrub a single-bit ECC error.
    void exchangeRows(unsigned source, unsigned dest)
    {
        if (source == dest)
            return;
        for (unsigned col = 0; col < numCols; ++col)
        {
            std::shared_ptr<DataStruct> left = searchForRow(source, rowEntries[col]);
            std::shared_ptr<DataStruct> right = searchForRow(dest, rowEntries[col]);
            if (left != NULL && right != NULL)
            {
                BurstType held = left->data;
                left->data = right->data;
                right->data = held;
            }
            else if (left != NULL)
                left->row = dest;
            else if (right != NULL)
                right->row = source;
            for (unsigned word = 0; word < 4; ++word)
            {
                Phase1StorageKey from;
                from.channel = channel_;
                from.rank = rank_;
                from.actual_bank = actualBank_;
                from.row = source;
                from.burst_column = col;
                from.codeword_index = word;
                Phase1StorageKey to = from;
                to.row = dest;
                // Keep the stored word and its parity.  Re-encoding here would
                // turn a correctable fault into a clean word and would drop an
                // uncorrectable word off the destination row.
                phase1Storage_.moveCodeword(from, to);
            }
        }
    }
    // Read-only view for G7 raw-storage evidence.  Test/evaluator code only;
    // normal reads continue through read()/write().
    const Phase1Storage& phase1Storage() const { return phase1Storage_; }
    BankState currentState;

  private:
    // private member
    std::vector<std::shared_ptr<DataStruct>> rowEntries;
    ostream& dramsimLog;
    static std::shared_ptr<DataStruct> searchForRow(unsigned row, std::shared_ptr<DataStruct> head);
    bool eccEnabled(Phase1AccessType access) const;
    unsigned mappedRow(const BusPacket* packet) const;
    void reportDetection(const Phase1Detection& detection) const;
    void reportFormat(const Phase1FormatEvent& event) const;
    unsigned numCols;
    unsigned channel_ = 0;
    unsigned rank_ = 0;
    unsigned actualBank_ = 0;
    Phase1Profile profile_ = Phase1Profile::NATIVE;
    Phase1ReportSink* reportSink_ = nullptr;
    Phase1Storage phase1Storage_;
    bool phase1Scrub_ = false;
    uint64_t phase1ScrubWrites_ = 0;
    uint64_t dataAccessCount_ = 0;
};
}  // namespace DRAMSim

#endif

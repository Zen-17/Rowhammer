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

#include "Bank.h"

#include <cstring>
#include <memory>
#include <stdexcept>

#include "BusPacket.h"

using namespace std;
using namespace DRAMSim;

Bank::Bank(ostream& simLog)
    : currentState(simLog), rowEntries(getConfigParam(UINT, "NUM_COLS")), dramsimLog(simLog)
{
    numCols = getConfigParam(UINT, "NUM_COLS");
}

/* The bank class is just a glorified sparse storage data structure
 * that keeps track of written data in case the simulator wants a
 * function DRAM model
 *
 * A vector of size NUM_COLS keeps a linked list of rows and their
 * associated values.
 *
 * write() adds an entry to the proper linked list or replaces the
 *     value in a row that was already written
 *
 * read() searches for a node with the right row value, if not found
 *     returns the tracer value 0xDEADBEEF
 *
 *    TODO: if anyone wants to actually store data, see the 'data_storage'
 *branch and perhaps try to merge that into master
 */

shared_ptr<Bank::DataStruct> Bank::searchForRow(unsigned row, shared_ptr<DataStruct> head)
{
    while (head != NULL)
    {
        if (head->row == row)
        {
            // found it
            return head;
        }
        // keep looking
        head = head->next;
    }
    // if we get here, didn't find it
    return NULL;
}

void Bank::read(BusPacket* busPacket, Phase1AccessType access, uint64_t cycle)
{
    dataAccessCount_++;
    shared_ptr<DataStruct> rowHeadNode = rowEntries[busPacket->column];
    shared_ptr<DataStruct> foundNode = NULL;
    if ((foundNode = Bank::searchForRow(busPacket->row, rowHeadNode)) == NULL)
    {
        return;
    }

    if (profile_ == Phase1Profile::NATIVE || !eccEnabled(access))
    {
        if (profile_ == Phase1Profile::HOST_ONLY && access != Phase1AccessType::HOST)
        {
            // PIM/HAB read path in HOST_ONLY: raw storage, no decoder.
            for (unsigned cw = 0; cw < 4; cw++)
            {
                Phase1StorageKey key = {channel_, rank_, actualBank_, busPacket->row,
                                        busPacket->column, cw};
                if (!phase1Storage_.contains(key))
                    return;
                const uint64_t word = phase1Storage_.readRawData(key);
                std::memcpy(&busPacket->data->u8Data_[cw * 8], &word, sizeof(word));
            }
        }
        else
        {
            *(busPacket->data) = foundNode->data;
        }
        return;
    }

    for (unsigned cw = 0; cw < 4; cw++)
    {
        Phase1StorageKey key = {channel_, rank_, actualBank_, busPacket->row, busPacket->column,
                                cw};
        if (!phase1Storage_.contains(key))
            return;
        const Phase1EccResult result = phase1Storage_.readProtected(key);
        if (result.status == Phase1EccStatus::CLEAN ||
            result.status == Phase1EccStatus::CORRECTED ||
            result.status == Phase1EccStatus::UNCORRECTABLE)
        {
            std::memcpy(&busPacket->data->u8Data_[cw * 8], &result.data, sizeof(result.data));
        }
        if (result.status == Phase1EccStatus::CORRECTED ||
            result.status == Phase1EccStatus::UNCORRECTABLE)
        {
            Phase1Detection detection;
            detection.detect_cycle = cycle;
            detection.channel = channel_;
            detection.rank = rank_;
            detection.actual_bank = actualBank_;
            detection.row = busPacket->row;
            detection.burst_column = busPacket->column;
            detection.codeword_index = cw;
            detection.status = result.status;
            detection.access = access;
            reportDetection(detection);
        }
        if (result.status == Phase1EccStatus::CORRECTED &&
            busPacket->phase1_status == Phase1PacketStatus::OK)
            busPacket->phase1_status = Phase1PacketStatus::CORRECTED;
        if (result.status == Phase1EccStatus::CORRECTED && phase1Scrub_)
        {
            // ASSUMED idealized scrub: direct codeword rewrite, no bus
            // transaction and no modeled cycle cost.
            phase1Storage_.writeLegal(key, result.data);
            phase1ScrubWrites_++;
        }
        if (result.status == Phase1EccStatus::UNPROTECTED_FORMAT)
        {
            Phase1FormatEvent event;
            event.detect_cycle = cycle;
            event.channel = channel_;
            event.rank = rank_;
            event.actual_bank = actualBank_;
            event.row = busPacket->row;
            event.burst_column = busPacket->column;
            event.codeword_index = cw;
            event.access = access;
            reportFormat(event);
            if (static_cast<int>(busPacket->phase1_status) <
                static_cast<int>(Phase1PacketStatus::UNPROTECTED_FORMAT))
                busPacket->phase1_status = Phase1PacketStatus::UNPROTECTED_FORMAT;
        }
        if (result.status == Phase1EccStatus::UNCORRECTABLE)
            busPacket->phase1_status = Phase1PacketStatus::UNCORRECTABLE;
    }
}

void Bank::write(const BusPacket* busPacket, Phase1AccessType access, uint64_t cycle)
{
    (void)cycle;
    dataAccessCount_++;
    if (busPacket->column >= numCols)
    {
        ERROR("== Error - Bus Packet column " << busPacket->column << " out of bounds");
        exit(-1);
    }

    shared_ptr<DataStruct> rowHeadNode = rowEntries[busPacket->column];
    shared_ptr<DataStruct> foundNode = NULL;
    if ((foundNode = Bank::searchForRow(busPacket->row, rowHeadNode)) == NULL)
    {
        shared_ptr<DataStruct> newRowNode = make_shared<DataStruct>();
        newRowNode->row = busPacket->row;
        if (busPacket->data)
            newRowNode->data = *(busPacket->data);
        newRowNode->next = rowHeadNode;
        rowEntries[busPacket->column] = newRowNode;
        foundNode = newRowNode;
    }
    else
    {
        foundNode->data = *(busPacket->data);
    }

    if (profile_ == Phase1Profile::NATIVE)
        return;

    const bool protect = eccEnabled(access);
    for (unsigned cw = 0; cw < 4; cw++)
    {
        uint64_t word = 0;
        std::memcpy(&word, &busPacket->data->u8Data_[cw * 8], sizeof(word));
        Phase1StorageKey key = {channel_, rank_, actualBank_, busPacket->row, busPacket->column,
                                cw};
        if (protect)
            phase1Storage_.writeLegal(key, word);
        else
            phase1Storage_.writeUnprotected(key, word);
    }
}

void Bank::setIdentity(unsigned channel, unsigned rank, unsigned actual_bank)
{
    channel_ = channel;
    rank_ = rank;
    actualBank_ = actual_bank;
}

void Bank::setPhase1(Phase1Profile profile, Phase1ReportSink* reportSink)
{
    profile_ = profile;
    reportSink_ = reportSink;
}

bool Bank::eccEnabled(Phase1AccessType access) const
{
    switch (profile_)
    {
        case Phase1Profile::NATIVE:
            return false;
        case Phase1Profile::LOCAL_VISIBLE:
        case Phase1Profile::LOCAL_HIDDEN:
            return true;
        case Phase1Profile::HOST_ONLY:
            return access == Phase1AccessType::HOST;
    }
    return false;
}

void Bank::reportDetection(const Phase1Detection& detection) const
{
    if (reportSink_ != nullptr && profile_ != Phase1Profile::NATIVE)
        reportSink_->onDetected(detection);
}

void Bank::reportFormat(const Phase1FormatEvent& event) const
{
    if (reportSink_ != nullptr && profile_ != Phase1Profile::NATIVE)
        reportSink_->onUnprotectedFormat(event);
}

void Bank::injectRawDataBit(unsigned row, unsigned burstColumn, unsigned bitInBurst)
{
    if (bitInBurst >= 256)
        throw std::out_of_range("Bank raw data bit must be in [0,255]");
    if (profile_ == Phase1Profile::NATIVE)
    {
        shared_ptr<DataStruct> head = rowEntries[burstColumn];
        shared_ptr<DataStruct> node = searchForRow(row, head);
        if (node == NULL)
            throw std::out_of_range("Bank raw row/column not initialized");
        node->data.u8Data_[bitInBurst / 8] ^= static_cast<uint8_t>(1u << (bitInBurst % 8));
        return;
    }
    Phase1StorageKey key = {channel_, rank_, actualBank_, row, burstColumn, bitInBurst / 64};
    phase1Storage_.injectDataBit(key, bitInBurst % 64);
}

void Bank::injectRawParityBit(unsigned row, unsigned burstColumn, unsigned codeword, unsigned bit)
{
    if (profile_ == Phase1Profile::NATIVE)
        throw std::logic_error("NATIVE profile has no ECC parity metadata");
    Phase1StorageKey key = {channel_, rank_, actualBank_, row, burstColumn, codeword};
    phase1Storage_.injectParityBit(key, bit);
}

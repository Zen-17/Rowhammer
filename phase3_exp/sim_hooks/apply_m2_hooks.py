#!/usr/bin/env python3
"""Patch the Phase 3 build tree with the two allowed compute-side fields.

The patches add mapping_epoch and the physical-to-logical translation result.
They do not change B2 recovery decisions or the public report CSV columns.

Stage 4–5 behavior (moveCodeword, STALE_EPOCH, observeAccess,
phase3_operand_denied, epoch-bound requests) is applied afterwards by
apply_stage4_hooks.py from sim_hooks/stage4_overlay/.  Do not re-edit the
generated build tree for those symbols.
"""
from __future__ import annotations

import sys
from pathlib import Path

EXCHANGE = r'''
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
                // Stage 4 overlay replaces this Bank.h after apply_m2_hooks.
                phase1Storage_.moveCodeword(from, to);
            }
        }
    }
'''


def patch_bank(text: str) -> str:
    if "exchangeRows" in text:
        return text
    anchor = "    uint64_t dataAccessCount() const { return dataAccessCount_; }\n"
    if anchor not in text:
        raise SystemExit("Bank.h anchor moved")
    return text.replace(anchor, anchor + EXCHANGE, 1)


def patch_reports_header(text: str) -> str:
    if "mapping_epoch" in text:
        return text
    text = text.replace(
        "    Phase1AccessType access = Phase1AccessType::HOST;\n};\n\nstruct Phase1FormatEvent",
        "    Phase1AccessType access = Phase1AccessType::HOST;\n"
        "    uint64_t mapping_epoch = 0;\n"
        "    unsigned logical_row = 0;\n};\n\nstruct Phase1FormatEvent",
        1,
    )
    text = text.replace(
        "    uint64_t region_id = 0;\n};\n\nclass Phase1ReportSink",
        "    uint64_t region_id = 0;\n"
        "    uint64_t mapping_epoch = 0;\n"
        "    unsigned logical_row = 0;\n};\n\nclass Phase1ReportSink",
        1,
    )
    text = text.replace(
        "        uint64_t region_id = 0;\n    };",
        "        uint64_t region_id = 0;\n"
        "        uint64_t mapping_epoch = 0;\n"
        "        unsigned logical_row = 0;\n    };",
        1,
    )
    if text.count("mapping_epoch") < 3:
        raise SystemExit("Phase1Reports.h did not gain mapping_epoch on all three structs")
    return text


def patch_reports_cpp(text: str) -> str:
    if "phase3NoteCe" not in text and "phase3MappingEpoch" in text:
        anchor = "    item.logical_row = phase3LogicalRow(detection.actual_bank, detection.row);\n"
        if anchor not in text:
            raise SystemExit("Phase1Reports.cpp logical-row anchor moved")
        return text.replace(
            anchor,
            anchor + "    if (is_ce)\n        phase3NoteCe(detection.actual_bank, detection.row);\n",
            1,
        )
    if "phase3MappingEpoch" in text:
        return text
    if '#include "Phase1Reports.h"\n' not in text:
        raise SystemExit("Phase1Reports.cpp missing include")
    text = text.replace(
        '#include "Phase1Reports.h"\n',
        '#include "Phase1Reports.h"\n#include "Phase3Bridge.h"\n',
        1,
    )
    needle = "    item.region_id = hostAddress / 4096ULL;\n    }\n"
    # The region_id assignments are followed by the capacity check.  Attach
    # the epoch once, after every granularity branch.
    anchor = "    if (pending_.size() >= capacity_)\n"
    if anchor not in text:
        raise SystemExit("Phase1Reports.cpp pending check moved")
    text = text.replace(
        anchor,
        "    item.mapping_epoch = phase3MappingEpoch(detection.actual_bank, detection.row);\n"
        "    item.logical_row = phase3LogicalRow(detection.actual_bank, detection.row);\n"
        + anchor,
        1,
    )
    text = text.replace(
        "    report.region_id = front.region_id;\n",
        "    report.region_id = front.region_id;\n"
        "    report.mapping_epoch = front.mapping_epoch;\n"
        "    report.logical_row = front.logical_row;\n",
        1,
    )
    return text


def patch_types(text: str) -> str:
    if "mapping_epoch" in text:
        return text
    anchor = "    bool is_replay = false;\n};"
    if anchor not in text:
        raise SystemExit("OpEntry anchor moved")
    return text.replace(anchor, "    bool is_replay = false;\n    uint64_t mapping_epoch = 0;\n};", 1)


def patch_ledger(text: str) -> str:
    if "phase3MappingEpoch" in text:
        return text
    if '#include "Phase2Ledger.h"\n' not in text:
        raise SystemExit("Phase2Ledger.cpp missing include")
    text = text.replace(
        '#include "Phase2Ledger.h"\n',
        '#include "Phase2Ledger.h"\n#include "Phase3Bridge.h"\n',
        1,
    )
    anchor = "    entry.dep_complete = dep_complete;\n"
    if anchor not in text:
        raise SystemExit("planOp anchor moved")
    # Row 16384 asks the hook for the current epoch rather than one physical row.
    return text.replace(
        anchor,
        anchor + "    entry.mapping_epoch = phase3MappingEpoch(0, 16384);\n",
        1,
    )


def patch_controller(text: str) -> str:
    if "phase3NoteHostLatency" in text:
        return text
    if '#include "MemoryController.h"\n' not in text:
        raise SystemExit("MemoryController.cpp missing include")
    text = text.replace(
        '#include "MemoryController.h"\n',
        '#include "MemoryController.h"\n#include "Phase3Bridge.h"\n',
        1,
    )
    anchor = "                memoryContStats->insertHistogram(\n                    currentClockCycle - pendingReadTransactions[i]->timeAdded, rank, bank);\n"
    if anchor not in text:
        raise SystemExit("latency histogram anchor moved")
    return text.replace(
        anchor,
        anchor
        + "                if (pendingReadTransactions[i]->tag.compare(0, 4, \"HOST\") == 0 ||\n"
        + "                    pendingReadTransactions[i]->tag.compare(0, 4, \"SWAP\") == 0)\n"
        + "                    phase3NoteHostLatency(currentClockCycle - pendingReadTransactions[i]->timeAdded);\n",
        1,
    )


def main() -> None:
    build = Path(sys.argv[1])
    files = {
        build / "src/Bank.h": patch_bank,
        build / "src/Phase1Reports.h": patch_reports_header,
        build / "src/Phase1Reports.cpp": patch_reports_cpp,
        build / "src/Phase2Types.h": patch_types,
        build / "src/Phase2Ledger.cpp": patch_ledger,
        build / "src/MemoryController.cpp": patch_controller,
    }
    for path, patch in files.items():
        path.write_text(patch(path.read_text()))


if __name__ == "__main__":
    main()

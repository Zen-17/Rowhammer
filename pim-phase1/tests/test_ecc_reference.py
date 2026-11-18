"""Independent Python SECDED reference and C++ exhaustive cross-check.

The reference uses a 72-bit list and explicit parity-position sets; it is not a
line-by-line translation of the C++ implementation.
"""
from __future__ import annotations

import csv
import os
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
CPP_CSV = Path(os.environ.get("PHASE1_ECC_CSV", ROOT / "runs/g3/ecc_cpp_exhaustive.csv"))

PARITY_POSITIONS = [1, 2, 4, 8, 16, 32, 64]
DATA_POSITIONS = [p for p in range(1, 72) if p not in PARITY_POSITIONS]
assert len(DATA_POSITIONS) == 64

PATTERNS = [
    0x0000000000000000,
    0xFFFFFFFFFFFFFFFF,
    0x5555555555555555,
    0xAAAAAAAAAAAAAAAA,
    0x0123456789ABCDEF,
    0xFEDCBA9876543210,
    0x0F1E2D3C4B5A6978,
    0x8877665544332211,
]


def encode(data: int) -> tuple[int, list[int]]:
    bits = [0] * 72
    for i, position in enumerate(DATA_POSITIONS):
        bits[position - 1] = (data >> i) & 1
    for i, position in enumerate(PARITY_POSITIONS):
        value = 0
        for q in DATA_POSITIONS:
            if q & position:
                value ^= bits[q - 1]
        bits[position - 1] = value
    overall = 0
    for q in range(1, 72):
        overall ^= bits[q - 1]
    bits[71] = overall
    parity = 0
    for i, position in enumerate(PARITY_POSITIONS):
        parity |= bits[position - 1] << i
    parity |= bits[71] << 7
    return parity, bits


def decode(data: int, parity: int) -> tuple[str, int, int, int]:
    bits = [0] * 72
    for i, position in enumerate(DATA_POSITIONS):
        bits[position - 1] = (data >> i) & 1
    for i, position in enumerate(PARITY_POSITIONS):
        bits[position - 1] = (parity >> i) & 1
    bits[71] = (parity >> 7) & 1
    syndrome = 0
    for position in range(1, 72):
        if bits[position - 1]:
            syndrome ^= position
    overall = 0
    for position in range(1, 73):
        overall ^= bits[position - 1]
    corrected = data
    if syndrome == 0 and overall == 0:
        status = "CLEAN"
    elif syndrome == 0 and overall == 1:
        status = "CORRECTED"
    elif syndrome <= 71 and overall == 1:
        bits[syndrome - 1] ^= 1
        corrected = 0
        for i, position in enumerate(DATA_POSITIONS):
            corrected |= bits[position - 1] << i
        status = "CORRECTED"
    elif syndrome > 71 and overall == 1:
        status = "INVALID"
    else:
        status = "UNCORRECTABLE"
    return status, corrected, syndrome, overall


def codeword(data: int, parity: int) -> list[int]:
    bits = [0] * 72
    for i, position in enumerate(DATA_POSITIONS):
        bits[position - 1] = (data >> i) & 1
    for i, position in enumerate(PARITY_POSITIONS):
        bits[position - 1] = (parity >> i) & 1
    bits[71] = (parity >> 7) & 1
    return bits


def from_codeword(bits: list[int]) -> tuple[int, int]:
    data = 0
    for i, position in enumerate(DATA_POSITIONS):
        data |= bits[position - 1] << i
    parity = 0
    for i, position in enumerate(PARITY_POSITIONS):
        parity |= bits[position - 1] << i
    parity |= bits[71] << 7
    return data, parity


@pytest.mark.parametrize("data", PATTERNS)
def test_reference_clean_and_single_bit(data):
    parity, _ = encode(data)
    assert decode(data, parity)[0] == "CLEAN"
    original = codeword(data, parity)
    for position in range(1, 73):
        bits = original[:]
        bits[position - 1] ^= 1
        mutated_data, mutated_parity = from_codeword(bits)
        status, corrected, _, _ = decode(mutated_data, mutated_parity)
        assert status == "CORRECTED", (hex(data), position)
        assert corrected == data, (hex(data), position)


@pytest.mark.parametrize("data", PATTERNS)
def test_reference_double_bit(data):
    parity, _ = encode(data)
    original = codeword(data, parity)
    for first in range(1, 73):
        for second in range(first + 1, 73):
            bits = original[:]
            bits[first - 1] ^= 1
            bits[second - 1] ^= 1
            mutated_data, mutated_parity = from_codeword(bits)
            status, _, _, _ = decode(mutated_data, mutated_parity)
            assert status == "UNCORRECTABLE", (hex(data), first, second)


ESCAPE_MASK = (1 << 0) | (1 << 1) | (1 << 4) | (1 << 10)
S4_MASK = (1 << 15) | (1 << 14)


def mutate_codeword(data: int, parity: int, data_bits: tuple[int, ...]):
    bits = codeword(data, parity)
    for data_bit in data_bits:
        bits[DATA_POSITIONS[data_bit] - 1] ^= 1
    return from_codeword(bits)


@pytest.mark.parametrize("data", PATTERNS)
def test_escape_patterns(data):
    """G11 pre-registered escape bit patterns (supplement §4.1).

    F3M and F4S deliver the same corrupted data; only the report differs.
    """
    parity, _ = encode(data)

    # Position mapping used by the pre-registered patterns.
    assert DATA_POSITIONS[0] == 3
    assert DATA_POSITIONS[1] == 5
    assert DATA_POSITIONS[4] == 9
    assert DATA_POSITIONS[10] == 15
    assert DATA_POSITIONS[14] == 20
    assert DATA_POSITIONS[15] == 21

    # F1: one data bit 15 flip -> corrected CE, original data.
    mutated_data, mutated_parity = mutate_codeword(data, parity, (15,))
    status, corrected, syndrome, overall = decode(mutated_data, mutated_parity)
    assert status == "CORRECTED"
    assert corrected == data
    assert syndrome == 21
    assert overall == 1

    # F3M: data bits {0,1,4} -> syndrome 15 (data bit 10) is "corrected"
    # to the wrong value, and a CE is still reported.
    mutated_data, mutated_parity = mutate_codeword(data, parity, (0, 1, 4))
    status, corrected, syndrome, overall = decode(mutated_data, mutated_parity)
    assert status == "CORRECTED"
    assert corrected == data ^ ESCAPE_MASK
    assert syndrome == 15
    assert overall == 1

    # F4S: data bits {0,1,4,10} -> syndrome 0, CLEAN, no report; the
    # delivered data is exactly the same bad value as F3M.
    mutated_data, mutated_parity = mutate_codeword(data, parity, (0, 1, 4, 10))
    status, f4s_data, syndrome, overall = decode(mutated_data, mutated_parity)
    assert status == "CLEAN"
    assert f4s_data == data ^ ESCAPE_MASK
    assert f4s_data == corrected
    assert syndrome == 0
    assert overall == 0

    # S4 second bit: {15} then {14} -> syndrome 1, overall 0, UE.
    mutated_data, mutated_parity = mutate_codeword(data, parity, (15, 14))
    status, s4_data, syndrome, overall = decode(mutated_data, mutated_parity)
    assert status == "UNCORRECTABLE"
    assert s4_data == data ^ S4_MASK
    assert syndrome == 1
    assert overall == 0

    # cw1 single bit: bitInBurst=79 is codeword 1, data bit 15; the ECC
    # result is the same as F1 because codewords are independent.
    assert 79 // 64 == 1
    assert 79 % 64 == 15
    mutated_data, mutated_parity = mutate_codeword(data, parity, (79 % 64,))
    status, corrected, syndrome, overall = decode(mutated_data, mutated_parity)
    assert status == "CORRECTED"
    assert corrected == data
    assert syndrome == 21
    assert overall == 1


@pytest.mark.skipif(not CPP_CSV.exists(), reason="C++ exhaustive CSV not generated")
def test_cpp_exhaustive_crosscheck():
    with CPP_CSV.open(newline="") as fh:
        rows = list(csv.DictReader(fh))
    assert len(rows) == 8 * (1 + 72 + 2556), len(rows)
    for row in rows:
        data = int(row["data_hex"], 16)
        parity = int(row["parity_hex"], 16)
        bits = codeword(data, parity)
        kind = row["flip_kind"]
        if kind == "single":
            bits[int(row["bit1"]) - 1] ^= 1
        elif kind == "double":
            bits[int(row["bit1"]) - 1] ^= 1
            bits[int(row["bit2"]) - 1] ^= 1
        elif kind != "none":
            raise AssertionError(f"unexpected flip kind: {kind}")
        mutated_data, mutated_parity = from_codeword(bits)
        status, corrected, syndrome, overall = decode(mutated_data, mutated_parity)
        assert status == row["status"], row
        assert corrected == int(row["corrected_hex"], 16), row
        assert syndrome == int(row["syndrome"]), row
        assert overall == int(row["overall_parity"]), row

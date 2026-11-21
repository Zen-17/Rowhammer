"""Cross-check the C++ Scheme8 codec against the Python model codec."""
from __future__ import annotations

from pathlib import Path

from phase2.layout import Geometry, Scheme8Codec

ROOT = Path(__file__).resolve().parents[2]
VECTORS = ROOT / "results/phase2/l1/address_vectors.csv"


def test_address_vectors_exist_and_match() -> None:
    assert VECTORS.exists(), f"missing {VECTORS}"
    codec = Scheme8Codec(Geometry())
    rows = 0
    with VECTORS.open() as handle:
        header = handle.readline().strip().split(",")
        assert header == ["addr", "channel", "rank", "bank", "row", "burst_col"]
        for line in handle:
            address, channel, rank, bank, row, burst_col = [int(v) for v in line.split(",")]
            tuple_value = codec.decode(address)
            assert tuple_value == (channel, rank, bank, row, burst_col)
            assert codec.encode(tuple_value) == address
            rows += 1
    assert rows >= 1000

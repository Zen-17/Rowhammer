"""Phase 2 physical layout and report footprints.

This module is deliberately independent of the simulator's runtime.  It maps
the Phase 1 Scheme8 address layout and the tile/operand table to the physical
burst keys used by the recovery engine.  It never reads fault schedules or
output data.
"""
from __future__ import annotations

from dataclasses import dataclass
from functools import lru_cache
from typing import Dict, FrozenSet, Iterable, List, Optional, Sequence, Tuple

# Workload-independent operand ids used by the recovery ledger.
OPERAND_A = 0
OPERAND_X = 1
OPERAND_B = 2
OPERAND_T = 3
OPERAND_Y = 4
OPERAND_U = 5
OPERAND_O = 6
OPERAND_W3_INPUT_BASE = 16
OPERAND_W3_OUTPUT = 32

OPERAND_NAMES = {
    OPERAND_A: "A",
    OPERAND_X: "X",
    OPERAND_B: "B",
    OPERAND_T: "T",
    OPERAND_Y: "Y",
    OPERAND_U: "U",
    OPERAND_O: "O",
}


@dataclass(frozen=True, order=True)
class BurstKey:
    """Finest physical unit: one 32 B burst."""

    channel: int
    rank: int
    bank: int
    row: int
    burst_col: int


@dataclass(frozen=True)
class Geometry:
    num_channels: int = 1
    num_ranks: int = 1
    num_banks: int = 16
    num_bank_groups: int = 4
    num_rows: int = 16384
    num_cols: int = 128
    burst_length: int = 4
    jedec_data_bus_bits: int = 64

    @property
    def burst_bytes(self) -> int:
        return self.burst_length * self.jedec_data_bus_bits // 8

    @property
    def burst_columns(self) -> int:
        return self.num_cols // self.burst_length


def _bits(value: int) -> int:
    if value <= 0 or value & (value - 1):
        raise ValueError(f"not a power of two: {value}")
    return value.bit_length() - 1


class Scheme8Codec:
    """Exact Scheme8 tuple <-> byte-address codec (rank:row:col:bg:bank:chan)."""

    def __init__(self, geometry: Geometry = Geometry()):
        self.geometry = geometry
        self.channel_bits = _bits(geometry.num_channels)
        self.rank_bits = _bits(geometry.num_ranks)
        self.bank_bits = _bits(geometry.num_banks)
        self.bankgroup_bits = _bits(geometry.num_bank_groups)
        self.bank_within_group_bits = self.bank_bits - self.bankgroup_bits
        self.row_bits = _bits(geometry.num_rows)
        self.column_bits = _bits(geometry.burst_columns)
        self.offset_bits = _bits(geometry.burst_bytes)

    @staticmethod
    def _mask(bits: int) -> int:
        return (1 << bits) - 1 if bits else 0

    def encode(self, tuple_value: Sequence[int]) -> int:
        channel, rank, bank, row, burst_col = tuple_value
        g = self.geometry
        if not (0 <= channel < g.num_channels and 0 <= rank < g.num_ranks
                and 0 <= bank < g.num_banks and 0 <= row < g.num_rows
                and 0 <= burst_col < g.burst_columns):
            raise ValueError("address tuple outside geometry")
        bank_low = bank & ((1 << self.bank_within_group_bits) - 1)
        bank_group = bank >> self.bank_within_group_bits
        address = rank & self._mask(self.rank_bits)
        address = (address << self.row_bits) | (row & self._mask(self.row_bits))
        address = (address << self.column_bits) | (burst_col & self._mask(self.column_bits))
        address = (address << self.bankgroup_bits) | (bank_group & self._mask(self.bankgroup_bits))
        address = (address << self.bank_within_group_bits) | bank_low
        address = (address << self.channel_bits) | (channel & self._mask(self.channel_bits))
        address = (address << self.offset_bits)
        return address

    def decode(self, address: int) -> Tuple[int, int, int, int, int]:
        value = address
        offset = value & self._mask(self.offset_bits)
        value >>= self.offset_bits
        channel = value & self._mask(self.channel_bits)
        value >>= self.channel_bits
        bank_low = value & self._mask(self.bank_within_group_bits)
        value >>= self.bank_within_group_bits
        bank_group = value & self._mask(self.bankgroup_bits)
        value >>= self.bankgroup_bits
        burst_col = value & self._mask(self.column_bits)
        value >>= self.column_bits
        row = value & self._mask(self.row_bits)
        value >>= self.row_bits
        rank = value & self._mask(self.rank_bits)
        value >>= self.rank_bits
        bank = (bank_group << self.bank_within_group_bits) | bank_low
        if offset != 0 or value != 0:
            raise ValueError("address has non-zero offset or high bits")
        return channel, rank, bank, row, burst_col

    def key(self, address: int) -> BurstKey:
        return BurstKey(*self.decode(address))


@dataclass(frozen=True)
class Layout:
    """Operand -> base row map for one workload instance."""

    name: str
    base_rows: Tuple[Tuple[int, int], ...]
    final_operand: int

    def base_row(self, operand: int) -> int:
        for key, value in self.base_rows:
            if key == operand:
                return value
        raise KeyError(f"operand {operand} is not in layout {self.name}")

    def operands(self) -> List[int]:
        return [key for key, _ in self.base_rows]

    def tile_position(self, tile: int) -> Tuple[int, int]:
        """Return (row offset, first burst column) of a tile inside an operand."""
        return (8 * tile) // 32, (8 * tile) % 32

    def tile_footprint(self, operand: int, tile: int) -> FrozenSet[BurstKey]:
        return _tile_footprint(self.base_row(operand), tile)


@lru_cache(maxsize=8192)
def _tile_footprint(base_row: int, tile: int) -> FrozenSet[BurstKey]:
    row_offset = (8 * tile) // 32
    first_col = (8 * tile) % 32
    keys = set()
    for bank in range(16):
        for col in range(first_col, first_col + 8):
            keys.add(BurstKey(0, 0, bank, base_row + row_offset, col))
    return frozenset(keys)


@dataclass(frozen=True)
class WorkloadLayouts:
    w1: Layout
    w2: Layout

    @staticmethod
    def default() -> "WorkloadLayouts":
        return WorkloadLayouts(
            w1=Layout("W1", ((OPERAND_A, 0), (OPERAND_X, 128), (OPERAND_B, 256)), OPERAND_B),
            w2=Layout(
                "W2",
                ((OPERAND_A, 0), (OPERAND_X, 128), (OPERAND_T, 256),
                 (OPERAND_Y, 384), (OPERAND_U, 512), (OPERAND_O, 640)),
                OPERAND_O,
            ),
        )

    @staticmethod
    def w3(fan_in: int) -> Layout:
        rows = [(OPERAND_W3_INPUT_BASE + i, 128 * i) for i in range(fan_in)]
        rows.append((OPERAND_W3_OUTPUT, 128 * fan_in))
        return Layout(f"W3_K{fan_in}", tuple(rows), OPERAND_W3_OUTPUT)

    @staticmethod
    def w4(fan_in: int) -> Layout:
        """W4: ADD -> fan-out MUL -> RELU, with F readers per T tile."""
        return Layout(
            f"W4_F{fan_in}",
            ((OPERAND_A, 0), (OPERAND_X, 128), (OPERAND_T, 256),
             (OPERAND_U, 512), (OPERAND_O, 640)),
            OPERAND_O,
        )


@dataclass(frozen=True)
class ReportFootprint:
    """Either one whole bank or an explicit set of burst keys."""

    kind: str
    bank: Optional[Tuple[int, int, int]] = None
    keys: Optional[FrozenSet[BurstKey]] = None

    def all_banks(self) -> Iterable[int]:
        if self.kind == "bank":
            return (self.bank[2],)
        return sorted({key.bank for key in self.keys or ()})


def page_region_id(layout: Layout, operand: int, tile: int) -> int:
    row_offset, first_col = layout.tile_position(tile)
    row = layout.base_row(operand) + row_offset
    return row * 4 + first_col // 8


def codeword_region_id(
    geometry: Geometry,
    bank: int,
    row: int,
    burst_col: int,
    codeword: int,
    channel: int = 0,
) -> int:
    return (((channel * geometry.num_banks + bank) * geometry.num_rows + row)
            * geometry.burst_columns + burst_col) * 4 + codeword


def page_footprint(region_id: int, codec: Scheme8Codec) -> ReportFootprint:
    base = region_id * 4096
    keys = set()
    for address in range(base, base + 4096, 32):
        keys.add(codec.key(address))
    return ReportFootprint("keys", keys=frozenset(keys))


def bank_footprint(region_id: int, geometry: Geometry) -> ReportFootprint:
    channel = region_id // geometry.num_banks
    bank = region_id % geometry.num_banks
    return ReportFootprint("bank", bank=(channel, 0, bank))


def codeword_footprint(region_id: int, geometry: Geometry) -> ReportFootprint:
    value = region_id
    codeword = value % 4
    value //= 4
    burst_col = value % geometry.burst_columns
    value //= geometry.burst_columns
    row = value % geometry.num_rows
    value //= geometry.num_rows
    bank = value % geometry.num_banks
    channel = value // geometry.num_banks
    return ReportFootprint(
        "keys",
        keys=frozenset({BurstKey(channel, 0, bank, row, burst_col)}),
    )


@dataclass(frozen=True)
class PublicReport:
    report_seq: int
    report_cycle: int
    error_class: str
    region_type: str
    region_id: int


def report_footprint(report: PublicReport, codec: Scheme8Codec) -> ReportFootprint:
    if report.region_type == "bank":
        return bank_footprint(report.region_id, codec.geometry)
    if report.region_type == "page":
        return page_footprint(report.region_id, codec)
    if report.region_type == "codeword":
        return codeword_footprint(report.region_id, codec.geometry)
    raise ValueError(f"unknown report region type: {report.region_type}")


def overlaps(item_keys: FrozenSet[BurstKey], footprint: ReportFootprint) -> bool:
    if footprint.kind == "bank":
        channel, rank, bank = footprint.bank
        return any(key.channel == channel and key.rank == rank and key.bank == bank
                   for key in item_keys)
    return not item_keys.isdisjoint(footprint.keys or frozenset())


def footprint_bursts(footprint: ReportFootprint, geometry: Geometry) -> int:
    if footprint.kind == "bank":
        return geometry.num_rows * geometry.burst_columns
    return len(footprint.keys or ())


@lru_cache(maxsize=4096)
def report_col_index(footprint: ReportFootprint) -> Tuple[Tuple[Tuple[int, int], FrozenSet[int]], ...]:
    """(bank,row) -> columns touched by an explicit footprint."""
    index: Dict[Tuple[int, int], set] = {}
    for key in footprint.keys or ():
        index.setdefault((key.bank, key.row), set()).add(key.burst_col)
    return tuple(sorted((key, frozenset(value)) for key, value in index.items()))


@lru_cache(maxsize=4096)
def report_row_index(footprint: ReportFootprint) -> Tuple[Tuple[int, Tuple[int, ...]], ...]:
    """bank -> sorted distinct rows touched by an explicit footprint."""
    index: Dict[int, set] = {}
    for key in footprint.keys or ():
        index.setdefault(key.bank, set()).add(key.row)
    return tuple(sorted((bank, tuple(sorted(rows))) for bank, rows in index.items()))


def item_overlaps(layout: Layout, operand: int, tile: int,
                  footprint: ReportFootprint) -> bool:
    """Fast tile-vs-report intersection without materialising 128 keys."""
    if footprint.kind == "bank":
        channel, rank, bank = footprint.bank
        return channel == 0 and rank == 0 and 0 <= bank < 16
    row_offset, first_col = layout.tile_position(tile)
    row = layout.base_row(operand) + row_offset
    columns = range(first_col, first_col + 8)
    for (bank, key_row), key_cols in report_col_index(footprint):
        if key_row != row:
            continue
        if not key_cols.isdisjoint(columns):
            return True
    return False


def item_touches_radius(layout: Layout, operand: int, tile: int,
                        footprint: ReportFootprint, radius: int) -> bool:
    if footprint.kind == "bank":
        channel, rank, bank = footprint.bank
        return channel == 0 and rank == 0 and 0 <= bank < 16
    row_offset, _first_col = layout.tile_position(tile)
    row = layout.base_row(operand) + row_offset
    for _bank, rows in report_row_index(footprint):
        for key_row in rows:
            if abs(row - key_row) <= radius:
                return True
    return False

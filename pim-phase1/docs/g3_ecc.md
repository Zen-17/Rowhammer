# G3 SECDED Model and Storage Injection

G3 implements the explicit extended Hamming SECDED(72,64) research model and a
storage wrapper for raw data/parity injection. It does not integrate ECC into
the full host/PIM paths, does not emit CE/UE reports, and does not implement
output gating, pause or recovery.

## 1. Codeword layout

- 64 data bits per codeword.
- Positions 1..71: data/parity.
- Positions 1,2,4,8,16,32,64: Hamming parity.
- The remaining 64 positions, in ascending order, carry data bits 0..63.
- Position 72: overall even parity over positions 1..71.
- Data byte order is little-endian: data bit j is bit j%8 of byte j/8.
- A 32 B burst has four independent 64-bit codewords; the storage key includes
  `codeword_index` 0..3.
- The 8-bit parity byte uses bits 0..6 for Hamming positions
  1,2,4,8,16,32,64 and bit 7 for position 72.

This is a **model layout**, not a claim about the physical HBM ECC codeword
layout of Aquabolt-XL or another device.

## 2. Decoder status

From the syndrome `s` and overall parity `p`:

| condition | status | data |
|---|---|---|
| `s=0, p=0` | CLEAN | unchanged |
| `s!=0, p=1, s<=71` | CORRECTED | flip position `s`; data returned corrected |
| `s=0, p=1` | CORRECTED | unchanged; position-72 parity bit was wrong |
| `s!=0, p=0` | UNCORRECTABLE | unchanged |
| `s>71, p=1` | INVALID | unchanged; no out-of-bounds repair |

Reads do **not** automatically scrub the raw storage; a persistent raw bit flip
can therefore produce a corrected event on every read. A legal write re-encodes
parity.

## 3. Storage interface

`Phase1Storage` keeps `(data64, parity8)` per `Phase1StorageKey`:

```text
channel, rank, actual_bank, row, burst_column, codeword_index
```

Normal writes use `writeLegal` and re-encode. `readProtected` decodes without
scrubbing. `injectDataBit`/`injectParityBit` and raw read methods are
test/evaluator-only interfaces and are not used by a runtime or report path.

## 4. Exhaustive verification

C++ `Phase1Ecc.ExhaustiveSingleAndDoubleBit` uses eight fixed data patterns:

```text
0x0000000000000000, 0xFFFFFFFFFFFFFFFF,
0x5555555555555555, 0xAAAAAAAAAAAAAAAA,
0x0123456789ABCDEF, 0xFEDCBA9876543210,
0x0F1E2D3C4B5A6978, 0x8877665544332211
```

For each pattern it checks:

- no-fault encode/decode -> CLEAN and original data;
- all 72 single-bit codeword flips -> CORRECTED and original data;
- all C(72,2)=2556 double-bit flips -> UNCORRECTABLE.

Total rows: 8 × (1 + 72 + 2556) = 21032. The C++ test writes
`runs/g3/ecc_cpp_exhaustive.csv` when `PHASE1_ECC_OUT` is set.

The Python reference in `tests/test_ecc_reference.py` uses a 72-bit list and
explicit position sets rather than compiling the C++ source. It independently
recomputes every single/double case and cross-checks the C++ CSV fields
`status`, `corrected_hex`, `syndrome`, and `overall_parity`. It also performs
its own exhaustive reference checks (17 pytest tests total including the
cross-check).

## 5. F1 witness

`Phase1Storage.F1WitnessAndNoScrub` initializes one codeword with
`0x0123456789ABCDEF`, flips data bit 15 (FP16 lane 0 sign bit within the 64-bit
codeword), and observes:

- `status=CORRECTED`;
- corrected data equals the original;
- a second read is still `CORRECTED` (no scrub);
- raw data remains flipped and raw parity is unchanged;
- a subsequent legal write restores `CLEAN`.

The witness is saved in `runs/g3/f1_witness.json`. This is a storage-level
witness, not a full PIM operand path test; full-path integration is G4.

## 6. Evidence

```text
runs/g3/ecc_cpp_exhaustive.csv
runs/g3/f1_witness.json
runs/g3/ecc_tests.stdout.log
runs/g3/ecc_python.stdout.log
runs/g3/clean_replay/
results/g3/ecc_summary.json
results/g3/validation.json
```

`scripts/run_g3.py` reruns the C++ tests, Python cross-check, and validates the
witness. `tests/test_ecc_reference.py` is the independent Python reference.

# BCMC RTL

Generic, hardware-agnostic Verilog implementation of the BCMC IP blocks.

Nothing in this directory targets a specific FPGA family. Board-specific
wrappers, pin constraints and vendor projects live under `fpga/`.

---

## The Core is a Transform, not an Accelerator

The entire contract of the BCMC Core is

```text
weights[]
    │
    ▼
offsets[]
```

It has no GPIO, no timers, no observers, no matrix output, no traversal and no
bus interface. Its only state is the prefix accumulator and the row index.

The Core computes the canonical prefix representation `(weights[], offsets[])`,
which is a **lossless representation of the canonical BCMC matrix** and is the
internal representation used by the hardware architecture. The canonical
mathematical object remains the matrix `M`; the prefix pair is the hardware's
compressed encoding of it.

The final accumulator value `P_C mod N` is deliberately **not** exposed. It
belongs to no row, evaluates no matrix element and appears nowhere in the proof.
Every BCMC transform begins at `offset[0] = 0`; transforms are not chainable.

---

## Files

| File               | Status      | Purpose                                             |
| ------------------ | ----------- | --------------------------------------------------- |
| `bcmc_core.v`      | **v0.2**    | Streaming prefix transform: weights → offsets       |
| `bcmc_pkg.vh`      | **v0.2**    | Shared default widths                               |
| `bcmc_cell.v`      | placeholder | v0.3 — characteristic function for one `(row, col)` |
| `bcmc_row.v`       | placeholder | v0.3 — `N` cells sharing one `(weight, offset)`     |
| `bcmc_column.v`    | placeholder | v0.3 — `C` cells sharing one column index           |
| `bcmc_evaluator.v` | placeholder | v0.3 — cell / row / column query front end          |

The placeholders are intentionally empty. The Core is completed and exhaustively
verified before any evaluation logic is written.

---

## `bcmc_core.v`

The BCMC Core is a **streaming prefix transform**. The mathematics is recursive

```text
offset[0]   = 0
offset[i+1] = (offset[i] + weight[i]) mod N
```

so the hardware is recursive too. There is deliberately no combinational
"specification" variant: at `C = 100000` a combinational prefix chain would be
absurd, and would misrepresent the algorithm.

### Emit before update

`offset_q` holds `oᵢ`. When `weight_valid` arrives, the Core

1. **first** presents `offset_q` on `offset_out`,
2. **then** updates `offset_q ← (offset_q + weight) mod N`.

Emitting before updating is what makes `offset[0] = 0` a structural property of
the datapath rather than a special case in the control logic.

### The reduction is not a division

The specification guarantees `0 ≤ wᵢ ≤ N`, hence `offset_q + weight < 2N`, so

```verilog
sum = offset_q + weight;
offset_q <= (sum >= N) ? sum - N : sum;
```

One comparator, one subtractor. No divider, ever. This is the hypothesis
`wᵢ ≤ N` of Lemma 2 (`docs/Proof.md`) appearing directly as hardware.

### Interface

`bcmc_core.v` implements the **BCMC Prefix Stream Interface**, specified with a
cycle-accurate timing diagram in
[`docs/Hardware_Architecture.md`](../docs/Hardware_Architecture.md). In summary:

```text
clk  rst  start   N  C   weight_in  weight_valid
                              │
                              ▼
                         bcmc_core
                              │
                              ▼
              offset_out  offset_valid  busy  done
```

All outputs are registered. `offset_valid` is asserted exactly one cycle after
each accepted `weight_valid`. `done` is a one-cycle pulse asserted _after_ the
final offset has been delivered, so it means unambiguously "the entire offset
stream has been emitted".

`weight_valid` may be asserted in any pattern: the sequence of accepted weights
alone determines the output sequence, and idle cycles affect timing only, never
values.

### Preconditions

`N ≥ 1` and `0 ≤ weight_in ≤ N` are preconditions of the specification, not
behaviours of the module. They are checked by simulation-only assertions guarded
by `` `ifndef SYNTHESIS ``. Violating them is outside the theorem's hypothesis
(see "Necessity of the hypothesis `wᵢ ≤ N`" in `docs/Proof.md`).

---

## Deliberately not done yet

Extracting an internal `bcmc_prefix.v` (the bare modular accumulator) is a
plausible later refactor. It is **not** done yet: hierarchy is introduced only
when the need is demonstrated by the code, not anticipated before it exists.

---

## Verification

The Python reference in `validation/reference.py` is the executable
specification. Every RTL module has a Python function with the same signature,
and the testbenches compare against it rather than inventing expected values.

```text
Proof  →  Python reference  →  Verilator tests  →  RTL  →  Tang Nano
```

See `sim/` for the harness and `docs/Verification.md` for results.

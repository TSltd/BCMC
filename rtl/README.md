# BCMC RTL

Generic, hardware-agnostic Verilog implementation of the BCMC IP blocks.

Nothing in this directory targets a specific FPGA family. Board-specific
wrappers, pin constraints and vendor projects live under `fpga/`.

---

## Two halves, and nothing else

BCMC is defined by two equations, and this directory contains one module for
each of them. Everything else here is replication.

```text
        weights[]                          M(i, j) = 1  ⟺
            │                              ((j − offset[i]) mod N) < weight[i]
            ▼                                        │
     offset[i+1] =                                   ▼
   (offset[i] + weight[i]) mod N                 bcmc_cell.v
            │
            ▼
       bcmc_core.v
```

The first is **recursive**, so `bcmc_core.v` is sequential. The second is
**pointwise** — `M(i, j)` depends on nothing but its own arguments — so
`bcmc_cell.v` is purely combinational: no clock, no reset, no state, no timing
assumptions of any kind.

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

| File            | Status   | Kind          | Purpose                                                       |
| --------------- | -------- | ------------- | ------------------------------------------------------------- |
| `bcmc_core.v`   | **v0.2** | sequential    | Streaming prefix transform: `weights[] → offsets[]`           |
| `bcmc_pkg.vh`   | **v0.2** | —             | Shared default widths                                         |
| `bcmc_cell.v`   | **v0.3** | combinational | The characteristic function `M(i, j)` for one `(row, column)` |
| `bcmc_row.v`    | **v0.3** | combinational | `MAX_N` cells sharing one `(weight, offset)` — the row of `M` |
| `bcmc_column.v` | **v0.3** | combinational | `MAX_C` cells sharing one column index — the column of `M`    |

There is no `bcmc_evaluator.v`, and there is not going to be one. "The BCMC
Evaluator" is an _architectural concept_ — the half of the design that evaluates
the characteristic function — and a concept does not imply a Verilog module.
Its mathematics lives entirely in `bcmc_cell.v`; a front end that chose between
cell, row and column queries would be a bus adapter, not evaluation logic, and
it would belong to whatever bus it adapted to.

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

## `bcmc_cell.v`

One matrix element, and the only new mathematics in v0.3:

```verilog
bit_out = (((column - offset) mod N) < weight)
```

`offsets → cell` has no history. There is no FSM, no state and no clock, because
`M(i, j)` is a function of its arguments and of nothing else. The module is a
literal transcription of the characteristic function.

### The wrap is not a division

The mirror image of the Core's conditional subtract. `0 ≤ column < N` and
`0 ≤ offset < N`, so `column − offset` lies in `(−N, N)`:

```verilog
wraps = (column < offset);
delta = wraps ? (column + (N - offset)) : (column - offset);
```

One comparator, one adder. No divider, and no `%`, ever.

The Core wraps **forwards** — `offset + weight` may overshoot `N`, so it
conditionally _subtracts_ `N`. The cell wraps **backwards** — `column − offset`
may undershoot `0`, so it conditionally _adds_ `N`. Both facts come from the same
hypothesis `0 ≤ wᵢ ≤ N` of Lemma 2.

The wrapped case is grouped as `column + (N − offset)` rather than the more
obvious `(column − offset) + N` on purpose: every intermediate value then stays
below `N`, which is why `VAL_W` bits suffice with no widening anywhere.

### No assertions inside the cell

`bcmc_core.v` carries its own precondition assertions; `bcmc_cell.v` deliberately
carries none. An assertion needs a moment at which to be true, and a purely
combinational module has no safe clock edge on which to evaluate one — mid-flight
input combinations are legitimate and transient. The preconditions are therefore
owned by the testbenches, which check them before driving anything.

---

## `bcmc_row.v` and `bcmc_column.v` — projections, not new logic

Matrix, row, column and cell are four **projections of one function**, and
neither rows nor columns are privileged:

| Projection | Bound    | Free     | Module           |
| ---------- | -------- | -------- | ---------------- |
| Cell       | `i`, `j` | —        | `bcmc_cell.v`    |
| Row        | `i`      | `j`      | `bcmc_row.v`     |
| Column     | `j`      | `i`      | `bcmc_column.v`  |
| Matrix     | —        | `i`, `j` | either, iterated |

Both are pure structural replication of `bcmc_cell` — a `generate` loop and
nothing else. This is not merely asserted in a comment: every bit of both modules
is compared against a **separately instantiated** `bcmc_cell` in every testbench,
290,913 bits per projection per simulator. The cell is the primitive; the
projections are provably just copies of it.

They exist for simulation and mathematical verification. Large designs should
instantiate `bcmc_cell` directly according to their traversal strategy, because
the `active` comparison against the runtime `N` or `C` in each lane is real logic
that a fixed traversal would not need.

### Bit ordering

```text
row_bits[0]    is column 0.    row_bits[j]    is column j.
column_bits[0] is row 0.       column_bits[i] is row i.
```

Verilog-2005 has no array ports, so `bcmc_column`'s weights and offsets arrive as
flat vectors with row `i` in bits `[VAL_W*i +: VAL_W]` — row 0 in the least
significant field, matching the bit ordering above.

### Lanes above `N` and `C`

`MAX_N` and `MAX_C` are synthesis-time bounds; `N` and `C` are runtime values and
may be smaller. The surplus lanes are **not** switched off and their outputs are
**not** masked. They are asked a question whose answer is zero: a cell with
`weight = 0` outputs 0 for every column, and `weight = 0` is legal for every `N`.
Every cell in both modules therefore receives a query that satisfies the cell's
own preconditions, which is why no masking is needed and why a mutant that
forgets to silence them is caught immediately.

---

## Deliberately not done yet

Extracting an internal `bcmc_prefix.v` (the bare modular accumulator) is a
plausible later refactor. It is **not** done yet: hierarchy is introduced only
when the need is demonstrated by the code, not anticipated before it exists.

The same rule is what keeps `bcmc_evaluator.v` from existing: no code has yet
demonstrated a need for anything between `bcmc_cell` and its caller.

---

## Verification

The Python reference in `validation/reference.py` is the executable
specification. Every RTL module has a Python function with the same signature,
and the testbenches compare against it rather than inventing expected values.

```text
Proof  →  Python reference  →  Verilator tests  →  RTL  →  Tang Nano
```

See `sim/` for the harness and `docs/Verification.md` for results.

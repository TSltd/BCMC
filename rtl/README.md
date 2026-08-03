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

`bcmc_context.v` is not a third half. It holds `(weights[], offsets[])` and
decides who may touch them, and it contains no BCMC mathematics at all: no
prefix sum, no modulo, no characteristic function. It does not even have an `N`
port or a `C` port, which is what makes that claim checkable rather than merely
asserted.

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

| File             | Status    | Kind          | Purpose                                                       |
| ---------------- | --------- | ------------- | ------------------------------------------------------------- |
| `bcmc_core.v`    | **v0.2**  | sequential    | Streaming prefix transform: `weights[] → offsets[]`           |
| `bcmc_pkg.vh`    | **v0.2**  | —             | Shared default widths                                         |
| `bcmc_cell.v`    | **v0.3**  | combinational | The characteristic function `M(i, j)` for one `(row, column)` |
| `bcmc_row.v`     | **v0.3**  | combinational | `MAX_N` cells sharing one `(weight, offset)` — the row of `M` |
| `bcmc_column.v`  | **v0.3**  | combinational | `MAX_C` cells sharing one column index — the column of `M`    |
| `bcmc_context.v` | **v0.4b** | sequential    | Stores the context; arbitrates software, Core and Evaluator   |
| `bcmc_wb.v`      | **v0.4c** | sequential    | Wishbone B4 Classic front end: the register map made real     |

There is no `bcmc_evaluator.v`, and there is not going to be one. "The BCMC
Evaluator" is an _architectural concept_ — the half of the design that evaluates
the characteristic function — and a concept does not imply a Verilog module.
Its mathematics lives entirely in `bcmc_cell.v`; a front end that chose between
cell, row and column queries would be a bus adapter, not evaluation logic, and
it would belong to whatever bus it adapted to.

`bcmc_wb.v` is that bus adapter, and it is named after the bus rather than after
the concept for exactly this reason: a second front end for a different bus would
be a sibling of it, not a replacement for the design.

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

## `bcmc_context.v` — storage and arbitration, and no mathematics

The Core produces offsets one at a time and forgets them; the Evaluator needs all
of them at once, forever. `bcmc_context.v` is the module that closes that gap. It
holds the **persistent BCMC context** — the pair `(weights[], offsets[])` — and
decides which of its three clients may touch it.

```text
   software ──────►┐                      ┌────► weights_flat  ─┐
                   │                      │                     ├─► the Evaluator
   bcmc_core ─────►│    bcmc_context      ├────► offsets_flat  ─┘
                   │  weights[] offsets[] │
   bcmc_core ◄─────┘                      └────► sw_weight, sw_offset
     (reads weights back)                          (indexed, for software)
```

### What is _not_ in here

No prefix sum. No modulo. No characteristic function. If any of those ever
appear in this file, responsibilities have bled across a module boundary.

That is easy to say and hard to keep, so the module is built so that it cannot
be broken quietly: **there is no `N` port and no `C` port.** Both equations need
`N`; the balance and conservation properties need `C`. A module given neither
cannot express BCMC mathematics even by accident. Its entire arithmetic content
is `wr_ptr + 1`, which is addressing.

The same absence settles two design questions for free:

- **Software cannot write an offset**, because no port exists through which to
  try. Offsets enter only from the Core, only while `loading`. The register map's
  read-only `OFFSET[i]` is therefore structural, not policed.
- **`load_start` clears the whole offset window**, not the first `C` lanes,
  because the module does not know `C`. That happens to be exactly what the
  register map requires (`OFFSET[C..MAX_C-1] = 0`), obtained without teaching the
  module anything it should not know.

### Flip-flops, not a RAM

It was called `bcmc_store.v` for about an hour, and both halves of that name were
wrong. It is not a store in the sense of a memory: the Evaluator reads _every_
lane combinationally and simultaneously, through `weights_flat` and
`offsets_flat`, so a one-port or two-port RAM cannot serve it. The storage is a
register file, `2 · MAX_C · VAL_W` flip-flops, and that cost is the reason
`MAX_C` is a synthesis-time bound.

### Two views, one truth

The indexed ports (`sw_weight`, `sw_offset`) and the flat vectors are two views
of the same registers, never two copies. The testbenches check this directly:
every value read through the indexed view is compared with the same lane of the
flat view, so a mutation that shifts one view by a lane dies immediately.

Reads outside `0 .. MAX_C-1` return zero rather than wrapping onto a real lane.

### Ownership

`loading` is the whole arbiter. `load_start` takes ownership, `load_done`
releases it, and while it is held, software weight writes are ignored. Offset
writes are accepted only while it is held, only up to lane `MAX_C-1`.

### Preconditions

Six simulation-only `$stop` assertions guarded by `` `ifndef SYNTHESIS `` cover
the illegal traffic: writing a weight during a load, offsets arriving outside a
load, overrunning the window, out-of-range indices. As with the Core, these are
preconditions of the specification, not behaviours. `sim/bcmc_context_test.cpp`
elaborates a _second_ top with `-DSYNTHESIS` so it can drive those violations and
watch the guards hold instead of tripping the alarms; `sim/tb_context.v` does the
opposite, driving only legal traffic with the alarms armed. The locks are tested
in one place, the alarms watched in the other.

### It is verified by composition

There is no formula in this module, so its testbenches cannot check one. They
instantiate a real `bcmc_core` alongside it instead and check the composition:
the offsets that end up in the window are the Core's, and the Core's offsets are
`validation/reference.py`'s.

---

## `bcmc_wb.v` — the register map made real

Everything above computes; nothing above is addressable. `bcmc_wb.v` is the only
module in the tree that knows what a bus is, and it is the whole of
[`docs/Register_Map.md`](../docs/Register_Map.md) in hardware: one 4 KiB
Wishbone B4 Classic slave containing the register file, the three windows, the
Core, the Context and one `bcmc_column` for `CELL[row][col]`.

```text
        Wishbone B4 Classic (32-bit, word granularity)
                        │
                        ▼
                   bcmc_wb.v
        ┌───────────┬───┴────┬───────────────┐
        ▼           ▼        ▼               ▼
   registers   bcmc_core  bcmc_context   bcmc_column
   ID CTRL      (the       (weights[],    (one cell of
   STATUS…      transform)  offsets[])     the matrix)
```

### Refuse, do not oblige

A slave may answer any access with `ACK` and made-up data, and nothing on the bus
would complain. This one does not: **every access that is not exactly right
receives `ERR`.** Unaligned, unmapped, wrong `sel`, a write to a read-only
register, a configuration write while `BUSY`, a matrix read before `VALID` — all
of them err, and none of them take effect. Silent success hides bugs, and a
driver that reads a stale matrix believing it fresh is a worse outcome than a
driver that faults.

Alignment and mapping are separate conditions and are checked separately, which
matters more than it sounds: an address may be word-aligned and unmapped, or
unaligned and yet land inside a mapped window. The vectors probe both.

### The response is exactly one cycle wide

```verilog
wire access = wb_cyc_i && wb_stb_i && !wb_ack_o && !wb_err_o;
```

A master that holds `stb` across the response cycle must not be served twice.
That is a property of this slave and not a courtesy of the master, so both
testbenches hold the request one clock beyond the response and inspect it. An
access therefore costs three clocks in simulation, which buys an entire class of
handshake bug.

### `busy` is structural

```verilog
wire busy = (seq != SEQ_IDLE);
```

There is no `busy` flag to forget to clear. The sequencer — `SEQ_IDLE`,
`SEQ_KICK`, `SEQ_STREAM` — is the only thing that says whether a transform is in
flight, so `STATUS.BUSY`, the `E4` refusals and the weight stream all read the
same truth. `C = 0` completes with no weight consumed, and the stream stops
because `stream_ptr` reaches `C`, not because the Core stopped asking.

### There is no `N` or `C` in the Context, so they live here

`N` and `C` are registers of this module. It is `bcmc_wb.v` that bounds the
windows, that presents `C` weights to the Core and no more, and that raises
`VALID` when `done` arrives. The Context still knows neither — see above — and
the division of labour is what makes the bus suites able to distinguish a
sequencing bug from a storage bug.

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

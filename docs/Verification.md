# Computational Verification

Verification happens in three layers, and each layer is only trusted because the
layer above it is:

| Layer                      | Question it answers                                     | Where                             |
| -------------------------- | ------------------------------------------------------- | --------------------------------- |
| 1. **The theorem**         | Is the Balance Theorem true?                            | `validation/verify_conjecture.py` |
| 2. **The reference model** | Does `reference.py` implement the theorem?              | `validation/test_reference.py`    |
| 3. **The RTL**             | Does the hardware compute what `reference.py` computes? | `sim/`                            |

No expected value anywhere in this project was written by hand. Every expected
offset and every expected matrix bit originates in `validation/reference.py`,
whose own output is checked against exhaustive enumeration and against the
independent implementations in `validation/verify_conjecture.py`.

The whole pipeline runs with:

```
./scripts/run_sim.sh          # add --big for the 10,000-case soak
```

---

## Layer 1 — the theorem

The theorem was verified exhaustively and by randomized search (see `validation/verify_conjecture.py`):

| Test                      | Scope                                                   | Result                                                                          |
| ------------------------- | ------------------------------------------------------- | ------------------------------------------------------------------------------- |
| Edge cases                | `C = 0`, `W = 0`, `N = 1`, zero rows, full rows         | all exact                                                                       |
| Exhaustive                | all weight vectors, `1 ≤ N ≤ 5`, `C ≤ 5` (15,029 cases) | no counterexample                                                               |
| Exhaustive                | all weight vectors, `1 ≤ N ≤ 7`, `C ≤ 4` (10,311 cases) | no counterexample                                                               |
| Random                    | 5,000 cases, `N ≤ 64`, `C ≤ 64`                         | no counterexample                                                               |
| Random                    | 2,000 cases, binary form vs residue counts              | identical in 2,000/2,000                                                        |
| Large scale               | `N = 10,000`, `C = 200`, `W ≈ 10⁶`                      | exact match on every column                                                     |
| Distribution independence | 900 + 5,000 random redistributions of the same `W`      | occupancy vector always identical                                               |
| Outside hypothesis        | `wᵢ > N` (2,000 cases)                                  | binary form diverges from residue counts in every case; row conservation breaks |

Every check validated the **exact** column-wise statement `L(j) = ResidueCount(j)` — not merely the weaker balance `max − min ≤ 1` — including the binary (0,1) matrix semantics, edge cases, and the independence of `L` from the weight distribution.

These results confirm the Balance Theorem: the exact occupancy statement holds in every tested case, and the only essential step in its proof is the bijection of Lemma 3 (see `docs/Proof.md`).

---

## Layer 2 — the reference model

`validation/reference.py` is the executable specification. Everything downstream
— test vectors, testbenches, RTL — is checked against it, so it is checked first,
by `validation/test_reference.py`:

| Test                      | Scope                                                   | Result             |
| ------------------------- | ------------------------------------------------------- | ------------------ |
| Doctests                  | every documented example in `reference.py`              | 11/11 pass         |
| Preconditions             | `N = 0`, `N < 0`, `wᵢ < 0`, `wᵢ > N`                    | 5/5 rejected       |
| Edge cases                | `C = 0`, `N = 1`, all-zero, all-`N` weight vectors      | 16/16 exact        |
| Exhaustive                | all weight vectors, `1 ≤ N ≤ 5`, `C ≤ 5` (15,029 cases) | no disagreement    |
| Exhaustive                | all weight vectors, `1 ≤ N ≤ 7`, `C ≤ 4` (10,311 cases) | no disagreement    |
| Random                    | 3,000 cases                                             | no disagreement    |
| Large scale               | `N` up to 65,535, `C` up to 1,024                       | no disagreement    |
| Distribution independence | 500 redistributions of the same `W`                     | occupancy constant |

Each case is checked against `itertools.accumulate` (an independent prefix sum
using a real `%`), against `verify_conjecture.py`'s `bcmc_algorithmic`,
`bcmc_interval`, `bcmc_binary` and `residue_counts`, and against the defining
recurrence asserted directly. Row conservation, `offset[0] = 0`,
`0 ≤ offset < N`, the balance property and the occupancy histogram are all
asserted independently.

---

## Layer 3 — the RTL

Every module is checked against the reference model by **two independent
simulators reading the same vector files**. The vector files are generated by
`validation/gen_vectors.py`, whose contents come from `reference.py` and nowhere
else.

| Module           | Verilator harness           | Icarus testbench   | Vector format |
| ---------------- | --------------------------- | ------------------ | ------------- |
| `bcmc_core.v`    | `sim/bcmc_core_test.cpp`    | `sim/tb_core.v`    | core          |
| `bcmc_cell.v`    | `sim/bcmc_cell_test.cpp`    | `sim/tb_cell.v`    | cell          |
| `bcmc_row.v`     | `sim/bcmc_row_test.cpp`     | `sim/tb_row.v`     | matrix        |
| `bcmc_column.v`  | `sim/bcmc_column_test.cpp`  | `sim/tb_column.v`  | matrix        |
| `bcmc_context.v` | `sim/bcmc_context_test.cpp` | `sim/tb_context.v` | matrix        |
| `bcmc_wb.v`      | `sim/bcmc_wb_test.cpp`      | `sim/tb_wb.v`      | bus           |

The last row reads a different _kind_ of file, generated by
`validation/gen_wb_vectors.py` rather than `gen_vectors.py`. See 3e.

### A tripped assertion must fail the run

Worth stating before any figures, because it invalidated one of them. The RTL's
`` `ifndef SYNTHESIS `` assertions end in `$stop`, and under a batch `vvp` with
no terminal attached `$stop` prints `** Continue **` and carries on. A design
that screamed on every case could therefore still reach its own `PASS` line. So
`sim/Makefile` greps the transcript for `stop called` and fails the run if it is
there, whatever the testbench concluded, and redirects `stdin` from `/dev/null`
so the behaviour does not depend on whether a terminal happens to be attached.

This was found by a mutation that appeared to survive and had in fact been
detected loudly three times over. The detection was right; the verdict was
wrong.

### 3a — the Core: the prefix transform

| Vector file           | Cases  | Rows    | Content                                      |
| --------------------- | ------ | ------- | -------------------------------------------- |
| `core_edge.txt`       | 26     | 106     | `C = 0`, `N = 1`, all-zero, all-`N`, `N` max |
| `core_random.txt`     | 500    | 14,986  | uniform weights                              |
| `core_biased.txt`     | 500    | 15,809  | weights biased towards `0` and `N`           |
| `core_large.txt`      | 7      | 3,380   | `N` up to 65,535, `C` up to 1,024            |
| `core_random_10k.txt` | 10,000 | 320,272 | soak (`gen_vectors.py --big`)                |

Every case is driven through the DUT **four times** — weights back to back, then
with 1, 3 and randomised idle cycles between them — so the figures above
correspond to 44,132 transforms per simulator, or 88,264 in total.

Each run is checked in five independent ways:

1. **Against Python.** The offset stream must equal `bcmc_core(weights, N)`
   element for element.
2. **Against the defining recurrence**, recomputed inside each testbench with a
   real `%` operator, referring neither to Python nor to the RTL's
   conditional-subtract shortcut: `o[0] = 0` and `o[i+1] = (o[i] + w[i]) mod N`.
3. **Residues.** Every emitted offset satisfies `0 ≤ offset < N`.
4. **Protocol.** Exactly `C` offsets; `offset_valid` never outside a transform;
   `done` a single-cycle pulse strictly after the last offset; `busy` and `done`
   never simultaneous; nothing emitted after `done`; `C = 0` completes cleanly.
5. **Gap invariance.** All four runs of a case must produce identical offsets.
   The sequence of accepted weights alone determines the output sequence; idle
   cycles change timing, never values.

The RTL additionally carries its own precondition and invariant assertions
(`N ≥ 1`, `weight ≤ N`, no `start` while busy, no more than `C` weights, and the
datapath invariant `offset < N` that makes the single subtraction valid). These
are active in both simulators.

| Simulator           | Harness                  | Result                                |
| ------------------- | ------------------------ | ------------------------------------- |
| Verilator 5.020     | `sim/bcmc_core_test.cpp` | 11,033 cases, 44,132 runs, 0 failures |
| Icarus Verilog 12.0 | `sim/tb_core.v`          | 11,033 cases, 44,132 runs, 0 failures |

Both are lint-clean under `verilator --lint-only -Wall`.

#### Testing the tests

A test suite that cannot fail proves nothing, so the harness was itself checked
by mutating the RTL and confirming that the mutant is rejected:

| Mutation                                | Detected by                                    |
| --------------------------------------- | ---------------------------------------------- |
| Emit _after_ update instead of before   | comparison against Python and the recurrence   |
| Remove the reduction entirely           | `-Wall` (the comparator becomes unused)        |
| `>=` becomes `>` in the wrap comparison | the RTL's own `offset < N` invariant assertion |
| `done` held for two cycles              | the protocol check on the `done` pulse         |

Each mutation was reverted immediately; the RTL in the repository is the
unmutated version, and both simulators are green against it.

---

### 3b — the Cell: the characteristic function

`bcmc_cell.v` is the other half of the BCMC definition and the only new
mathematics in v0.3. It is purely combinational, so it has no protocol to check
and no timing to check — but it has something the Core does not: for small `N`
the input space is **finite and small enough to enumerate completely**.

| Vector file              | Cases | Content                                                                 |
| ------------------------ | ----- | ----------------------------------------------------------------------- |
| `cell_edge.txt`          | 410   | `N = 1`, `weight = 0`, `weight = N`, `column = offset`, wrap boundaries |
| `cell_exhaustive.txt`    | 6,734 | **every** `(N, weight, offset, column)` with `N ≤ 8`                    |
| `cell_random.txt`        | 5,000 | `N` up to 65,535                                                        |
| `cell_exhaustive_32.txt` | soak  | **every** query with `N ≤ 32` (`gen_vectors.py --big`)                  |

`cell_exhaustive.txt` is exhaustive, not sampled: for `N ≤ 8` it contains every
legal combination of the four inputs, so within that range the module is not
tested but **proven** by enumeration. The Verilator harness additionally sweeps
`--sweep 40`, enumerating every query for `N ≤ 40` directly against
`reference.py` without going through a file at all.

Each of the 12,144 file cases is evaluated **three times** — forwards, backwards
and shuffled — giving 36,432 evaluations per simulator. Every evaluation is
checked in five ways:

1. **Against Python.** `bit_out` must equal `reference.py`'s `bcmc_cell(...)`.
2. **Against the definition**, recomputed in the harness with a real `%`
   operator: `((column − offset) mod N) < weight`, referring neither to Python
   nor to the RTL's conditional-add shortcut.
3. **Row conservation (Lemma 1).** Sweeping `column` across a whole row must
   yield exactly `weight` ones.
4. **The degenerate rows.** `weight = 0` must give 0 for every column and
   `weight = N` must give 1 for every column — as consequences of the
   comparison, never as special cases.
5. **Order invariance.** All three passes must agree. This is the stateless
   analogue of the Core's gap invariance: a combinational module cannot have a
   history, and the way to test that claim rather than assert it is to ask the
   same questions in a different order. Inputs are driven to `x` between
   evaluations, so a module that had latched a value would reveal it as `x`.

Preconditions (`N ≥ 1`, `0 ≤ weight ≤ N`, `0 ≤ offset < N`, `0 ≤ column < N`) are
checked by the **testbenches**, not by the RTL. A combinational module has no safe
clock edge on which to evaluate an assertion, so `bcmc_cell.v` deliberately
contains none.

| Simulator           | Harness                  | Result                                       |
| ------------------- | ------------------------ | -------------------------------------------- |
| Verilator 5.020     | `sim/bcmc_cell_test.cpp` | 12,144 cases, 36,432 evaluations, 0 failures |
| Icarus Verilog 12.0 | `sim/tb_cell.v`          | 12,144 cases, 36,432 evaluations, 0 failures |

#### Testing the tests

| Mutation                                            | Detected by                                            |
| --------------------------------------------------- | ------------------------------------------------------ |
| `<` becomes `<=` in the characteristic function     | Python, the recomputed definition and row conservation |
| `column < offset` becomes `column <= offset`        | Python and the recomputed definition                   |
| Conditional add removed (`delta = column - offset`) | Python, and `-Wall` (`N` becomes unused)               |
| `delta < weight` becomes `delta < N`                | Python, and `-Wall` (`weight` becomes unused)          |

Two of the four are caught by lint alone, before any simulation runs: deleting
part of the mathematics leaves an input with nothing to do, and Verilator's
`UNUSEDSIGNAL` notices. All four were reverted immediately.

---

### 3c — the projections: row, column and the whole matrix

`bcmc_row.v` and `bcmc_column.v` introduce **no new mathematics**. They are
`generate` loops over `bcmc_cell`, and the verification is designed to prove
exactly that rather than to re-test the cell through a wrapper.

| Vector file               | Cases | Bits   | Content                                                    |
| ------------------------- | ----- | ------ | ---------------------------------------------------------- |
| `matrix_edge.txt`         | 17    | 3,718  | `C = 0`, `N = 1`, all-zero, all-`N`, `W` a multiple of `N` |
| `matrix_exhaustive.txt`   | 1,274 | 16,426 | **every** weight vector for `N ≤ 4`, `C ≤ 4`               |
| `matrix_random.txt`       | 300   | 76,827 | `N`, `C` up to 32                                          |
| `matrix_exhaustive_6.txt` | soak  | —      | every weight vector for `N ≤ 6`, `C ≤ 5`                   |
| `matrix_random_5k.txt`    | soak  | —      | 5,000 random matrices                                      |

Each case is a whole matrix: the weights, the offsets from `bcmc_core`, one `R`
line of bits per row, and the column occupancy `L`. Both projections read the
**same** files, so they assemble the same matrix from opposite directions and must
agree on every bit.

Each case is replayed three times (forwards, backwards, shuffled — rows for the
row projection, columns for the column projection) and checked in five ways:

1. **Against Python.** Every bit must equal the corresponding bit of the `R`
   lines, which came from `reference.py`.
2. **Against a separately instantiated `bcmc_cell`.** Each harness contains a
   _second_, standalone cell alongside the DUT, and every single bit the
   projection produces is compared against what that cell answers for the same
   `(N, weight, offset, column)`. This is the replication claim, tested rather
   than asserted: the cell is the primitive, and the projections are demonstrably
   nothing but copies of it.
3. **Lanes above the bound read zero.** `MAX_N` and `MAX_C` exceed the runtime
   `N` and `C`; the surplus lanes are handed `weight = 0` and must therefore
   answer 0 for every column.
4. **Row conservation (Lemma 1).** `popcount(row i) == weights[i]`. The row
   projection sees a whole row in one query; the column projection has to
   accumulate it across all the columns of the case, in whatever order that pass
   chose.
5. **The Balance Theorem.** `W = qN + r` is recomputed in each harness from the
   weights alone, and the load on column `j` must be `q + 1` for `j < r` and `q`
   otherwise — then cross-checked against `reference.py`'s `L`. For the column
   projection this is the popcount of a single output of a single module, which is
   the reason that projection is worth having.

| Simulator           | Projection | Matrices | Bits vs a separate `bcmc_cell` |
| ------------------- | ---------- | -------- | ------------------------------ |
| Verilator 5.020     | row        | 1,591    | 290,913                        |
| Verilator 5.020     | column     | 1,591    | 290,913                        |
| Icarus Verilog 12.0 | row        | 1,591    | 290,913                        |
| Icarus Verilog 12.0 | column     | 1,591    | 290,913                        |

All four figures agree exactly — 1,163,652 cell comparisons in total, with 0
failures.

#### Testing the tests

| Mutation                                                | Detected by                                         |
| ------------------------------------------------------- | --------------------------------------------------- |
| row: lane `j` answers for column `j + 1`                | Python, the separate cell, **and** row conservation |
| row: lanes above `N` not silenced                       | the "lane above `N` reads 1" check                  |
| column: lane `i` reads row `(i + 1) mod MAX_C`'s weight | Python and the separate cell                        |
| column: `weight` and `offset` ports transposed          | Python and the separate cell                        |

The first mutation is instructive: shifting a row by one column leaves its
popcount unchanged, so row conservation alone would not have caught it — but the
Balance Theorem and the per-bit comparison both do. The four mutations were
reverted immediately, and `git diff rtl/` confirms the projections in the
repository are the unmutated versions.

---

### 3d — the context: storage and arbitration

`bcmc_context.v` contains **no mathematics at all**, which changes what its
verification can be. There is no formula to compare against, so the harnesses do
not check one. They check the **composition** instead: a real `bcmc_core` is
instantiated alongside the context and wired to it, and the offsets that end up
in the window must be the Core's — which are `reference.py`'s.

Both harnesses read the same `matrix_*.txt` files as the projections. The `R`
lines and the `L` line are read and discarded; they belong to `bcmc_row` and
`bcmc_column`. Only the `W` and `O` lines matter here.

Each case is replayed three times, with the weight write order forwards,
backwards and shuffled, and checked in six ways:

1. **Reset.** After reset, every lane of both windows reads 0.
2. **The weight window.** Every weight written by "software" reads back
   unchanged, before the transform and again after it.
3. **The offset window.** Every lane `i < C` must hold the `O` line's offset, and
   every lane `C ≤ i < MAX_C` must hold 0 — and the value came from the Core, not
   from the testbench.
4. **Two views, one truth.** Every value read through the indexed `sw_*` ports is
   compared against the same lane of the flat `weights_flat` / `offsets_flat`
   vectors the Evaluator sees. They are two views of one register file, never two
   copies.
5. **No stale offsets.** Reset happens once per pass, not once per case, so a
   case with a small `C` follows one with a large `C` and would inherit its tail
   if `load_start` did not clear the whole window.
6. **Ownership.** `loading` must track the Core's `busy` and release on `done`,
   and weight writes attempted while it is held must be ignored.

Reads of lanes that do not exist are checked at the end of a case rather than
just after reset, so that the lanes an out-of-range index would truncate onto are
non-zero when the question is asked. That distinction is not cosmetic: it is what
makes the "out of range reads 0" check able to fail.

| Simulator           | Harness                     | Contexts | Stored values checked |
| ------------------- | --------------------------- | -------- | --------------------- |
| Verilator 5.020     | `sim/bcmc_context_test.cpp` | 1,591    | 559,275               |
| Icarus Verilog 12.0 | `sim/tb_context.v`          | 4,773    | 382,566               |

0 failures in both. The two figures differ because the harnesses count and probe
differently — Icarus counts each pass as a context — and that is the point of
having two.

#### Two harnesses, two jobs

The context's illegal traffic (a weight write during a load, an offset arriving
outside one, an overrun past `MAX_C`) is covered by six `$stop` assertions guarded
by `` `ifndef SYNTHESIS ``. That creates a problem no earlier module had: the
assertions and the **guards behind them** cannot both be exercised by the same
elaboration, because driving the violation trips the alarm before the guard can
be observed holding.

So the two harnesses do opposite jobs. `sim/bcmc_context_test.cpp` verilates a
_second_ top with `-DSYNTHESIS`, silencing the assertions, and drives exactly the
forbidden accesses to confirm the guards ignore them. `sim/tb_context.v` drives
only legal traffic with the assertions armed. **The alarms are watched in one
place and the locks tested in the other.**

#### Testing the tests

Ten mutations, each a plausible single-edit bug, were run against both harnesses:

| Mutation                                               | Verilator | Icarus       |
| ------------------------------------------------------ | --------- | ------------ |
| Arbitration removed (software may write mid-load)      | killed    | **survived** |
| `load_start` no longer clears the offset window        | killed    | killed       |
| Write pointer never advances                           | killed    | killed       |
| Write pointer starts at lane 1                         | killed    | killed       |
| `sw_offset` reads the weight window                    | killed    | killed       |
| Flat view shifted one lane from the indexed view       | killed    | killed       |
| Bounds dropped on the read path                        | killed    | killed       |
| Ownership never released                               | at build  | killed       |
| Reset does not clear the weight window                 | killed    | killed       |
| Overrun guard removed (offset `MAX_C` wraps to lane 0) | at build  | **survived** |

Ten of ten die under Verilator, eight of ten under Icarus. The two Icarus
survivors are exactly the two guards on illegal traffic, which `tb_context.v` is
forbidden by design to drive — the gap is real, it is named in that file's header,
and it is covered by the `-DSYNTHESIS` top next door. Two of the Verilator kills
are lint kills rather than behavioural ones: removing a guard leaves its
condition wire unused, and `-Wall` refuses to build the result.

Every mutation was reverted, and `diff` against a pre-mutation copy confirms the
`bcmc_context.v` in the repository is the unmutated version.

---

### 3e — the bus: a recorded conversation

`bcmc_wb.v` is the top of the tree, so its harnesses instantiate everything
there is — wrapper, context, core, column, cell — and there is nothing left
inside the fabric to compare against. The mathematics has already been settled
by 3a to 3d. What is new in v0.4c is a **protocol**, and the class of bug it can
produce is decode, `sel`, sequencing, `err` and interrupt lifetime.

So the vectors are a different kind of object: not instances of the mathematics
but **recorded conversations**. `validation/gen_wb_vectors.py` drives
`validation/bcmc_periph.py` — the executable form of `docs/Register_Map.md`,
itself checked by 817 assertions in `validation/test_periph.py` before a line of
RTL existed — through the sequences of `docs/Transaction_Sequences.md`, and
writes down what happened.

```text
docs/Register_Map.md → bcmc_periph.py → wb_*.txt → both harnesses → bcmc_wb.v
```

Five ops, one per line, hexadecimal throughout:

| Op                              | Meaning                                    |
| ------------------------------- | ------------------------------------------ |
| `Z`                             | reset the peripheral                       |
| `L <text>`                      | a label, free text to end of line          |
| `R adr sel data ACK\|ERR rdata` | read, expecting that outcome and that data |
| `W adr sel data ACK\|ERR rdata` | write, expecting that outcome              |
| `P adr sel mask ACK value`      | poll `adr` until `(read & mask) == value`  |

The `P` op is the one that had to be invented. **The Python model has no
clock**: `BcmcPeripheral(latency=k)` makes the next `k` accesses observe `BUSY`,
which is a count of accesses, not of cycles. A recorded `STATUS` read taken just
after `START` therefore cannot be replayed literally against hardware, which
needs `C + 4` clocks to answer the same way. What model and hardware genuinely
share is not the value but **the wait**, so the wait is what gets recorded.

| Vector file        | Ops   | Content                                                    |
| ------------------ | ----- | ---------------------------------------------------------- |
| `wb_sequences.txt` | 195   | S1–S8 and F1–F11 of `docs/Transaction_Sequences.md`        |
| `wb_busy.txt`      | 138   | one long transform, probed from every angle mid-flight     |
| `wb_random.txt`    | 2,366 | 17 instances, each read back twice — by cell and by column |

`wb_sequences.txt` contains 25 accesses that expect `ERR` against 154 that
expect `ACK`, and the refusals are recorded **inline among the successes**, not
in a file of their own. An address decoder that accepts too much is a far more
common defect than one that accepts too little, and `ERR` is the only place the
hardware can say no — which is why `docs/Register_Map.md` chose `ERR` over
`ACK`-with-zero. Silent success hides bugs.

`wb_random.txt` expects no errors at all: it reads 17 whole matrices back
through the bus, once cell by cell through `CELL[row][col]` and once column by
column through the `COLUMN` window. Both readings must produce the same matrix,
and that matrix must be `reference.py`'s. It is the only place in the suite
where `bcmc_column` and `bcmc_cell` are compared **through 4 KiB of address
decoding**.

#### One bus functional model, two languages

`sim/common/wb_bfm.{h,cpp}` is the Verilator master; the bus-master section of
`sim/tb_wb.v` is the Icarus one. They are written to the same three rules:

1. Wishbone B4 **Classic** — no bursts, no pipelining, 32-bit, word
   granularity, `ack` or `err` in the cycle after `cyc & stb`.
2. Every access is followed by an idle cycle, because
   `access = cyc && stb && !ack && !err` is qualified on the response and a
   back-to-back request would otherwise be misread.
3. **The request is held one clock beyond the response, and that clock is
   inspected.** A slave that had forgotten to qualify a new access would answer
   the same request twice, and a master that dropped `stb` the instant it saw
   `ack` could never notice. Every access costs three clocks because of this.

Rule 3 is the interesting one: it is a property of the slave, not a courtesy of
the master, so the master is built to check it rather than to rely on it.

#### What the harnesses judge for themselves

Nothing about BCMC, and nothing about register values. There is not one expected
register value written by hand in either harness. The only judgements they make
on their own are the three things the model cannot express because the model has
no wires:

1. **`ack` and `err` are never asserted together.** Checked every cycle.
2. **Every access is answered exactly once**, inside a timeout. A slave that
   stops talking is a different bug from a slave that refuses, and a slave that
   answers twice is a third; the message says which.
3. **`irq_o` agrees with the registers.** At every label boundary `STATUS` and
   `CTRL` are read back over the bus and the pin compared against
   `STATUS.IRQ & CTRL.IRQ_EN`. The pin is not in the vector format, so its
   expectation is derived from the device, never remembered.

The RTL's own assertions are left armed in both, because every recorded access
obeys `docs/Register_Map.md`: anything that trips one is the harness's fault.

`sim/tb_wb.v` additionally replays each file **twice** with no simulator reset
in between — each file begins with its own `Z` — so a second pass that disagrees
with the first has found state that reset does not clear.

| Simulator           | Suite              | Ops   | Checks | Accesses | Clocks |
| ------------------- | ------------------ | ----- | ------ | -------- | ------ |
| Verilator 5.020     | `wb_sequences.txt` | 180   | 202    | —        | 664    |
| Verilator 5.020     | `wb_busy.txt`      | 135   | 144    | —        | 454    |
| Verilator 5.020     | `wb_random.txt`    | 2,349 | 2,401  | —        | 7,267  |
| Icarus Verilog 12.0 | `wb_sequences.txt` | 195   | 404    | 436      | 1,329  |
| Icarus Verilog 12.0 | `wb_busy.txt`      | 138   | 288    | 296      | 909    |
| Icarus Verilog 12.0 | `wb_random.txt`    | 2,366 | 4,802  | 4,838    | 14,535 |

0 failures throughout. The two op counts differ by exactly the number of `L`
lines — Icarus counts a label as an op, Verilator does not — and the Icarus
figures are totals over both passes.

#### Testing the tests

Sixteen mutations of `rtl/bcmc_wb.v`, one plausible slip each, run against
`wb_sequences.txt` and `wb_busy.txt`:

| Mutation                                      | Caught by                      |
| --------------------------------------------- | ------------------------------ |
| `E1` alignment not checked                    | `wb_sequences`                 |
| `E1` mapping not checked                      | `wb_sequences`                 |
| `E3` `sel` not checked                        | `wb_sequences`                 |
| `E2` write to a read-only register allowed    | `wb_sequences`                 |
| `E4` configuration write while `BUSY` allowed | `wb_busy` (RTL assertion)      |
| `START` while `BUSY` allowed                  | `wb_busy`                      |
| The matrix readable before `VALID`            | `wb_sequences`                 |
| The response not one cycle wide               | `wb_sequences`                 |
| An erring write still takes effect            | `wb_sequences`                 |
| The `COLUMN` span not bounded                 | `wb_sequences`                 |
| `STATUS` fields transposed                    | `wb_sequences`                 |
| The wrong `ID`                                | `wb_sequences`                 |
| `CAPS` reports the wrong `MAX_C`              | `wb_sequences`                 |
| The `IRQ` flag never raised                   | `wb_sequences`                 |
| Any `CTRL` write starts a transform           | `wb_sequences` (RTL assertion) |
| The weight stream never stops                 | `wb_sequences` (RTL assertion) |

**All sixteen die.** Three of them are caught by the RTL's own assertions rather
than by a comparison, which is the correct outcome — a wrapper that presents
more than `C` weights to the Core has violated the Core's precondition, and the
Core says so.

Three mutations survived the first attempt, and all three exposed a real gap:

- **`E1` alignment.** The unaligned addresses being probed were also
  _unmapped_, so `!mapped` alone still refused them — for the wrong reason. A
  register cannot be probed for this, since it is decoded by equality and any
  unaligned address near it is unmapped too; the three _windows_ can be, so
  `0x02A`, `0x402` and `0x802` were added, unaligned yet landing squarely on
  `COLUMN[0]`, `WEIGHT[0]` and `OFFSET[0]`.
- **The response width.** Both masters dropped `stb` immediately on seeing the
  response, so a second `ack` could never appear. Rule 3 above is the fix, and
  it applies to every access in the suite.
- **The weight stream.** Detected loudly and reported as surviving — see "a
  tripped assertion must fail the run" at the head of Layer 3. The fix was to
  `sim/Makefile`, and it closed the same hole for every testbench in the
  repository, not just this one.

Every mutation was reverted; `git diff rtl/` confirms the `bcmc_wb.v` in the
repository is the unmutated version.

### 3f — the driver: software, held to the same standard

`sw/bcmc.c` is the first thing in this repository that is not hardware, and it
is the last link in the chain: everything above it is only reachable by a CPU
through this file. So it is verified the same way, with one substitution forced
by the material.

**Two compilers instead of two simulators.** Every module in `rtl/` is checked
by Verilator and by Icarus, because a single tool's agreement with itself proves
nothing about the language. A C translation unit cannot be checked that way —
Verilog simulators do not compile C — so the second opinion comes from a second
compiler instead. Step 7 of `scripts/run_sim.sh` builds `sw/bcmc.c` under every
C compiler present with

```text
-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wstrict-prototypes
-Wmissing-prototypes -Werror
```

and then compiles a translation unit that includes `bcmc.h` under every C++
compiler present, because a header a C++ project cannot include is a header
half the potential callers cannot use. There is no `sim/tb_driver.v`, and there
should not be; this is a deliberate, documented exception to the two-simulator
rule rather than an omission.

#### The driver under test is the driver

`sim/bcmc_driver_test.cpp` compiles `sw/bcmc.c` — the same translation unit a
bare-metal target would link, not a copy and not a reimplementation — and points
its two function pointers at `bcmc::WbMaster`, which drives the verilated
`rtl/bcmc_wb.v`. When `bcmc_load()` writes a weight here, a Wishbone cycle
happens and a flip-flop in `bcmc_context` changes state.

That matters because a driver tested against a _model_ of its peripheral only
proves that the two agree, and the interesting bugs live exactly where they
disagree: the width of a field, the address of a window, which state refuses
which access.

The expected answers come from where they always do:

```text
docs/Proof.md → reference.py → matrix_*.txt → bcmc_wb.v
                                           → sw/bcmc.c → here
```

The `wb_*.txt` conversations of 3e are deliberately **not** replayed. Those
record what `bcmc_periph.py` chose to do, access by access; replaying them
through the driver would test the recording. What is checked instead is that the
driver, left to choose its own accesses, arrives at `reference.py`'s matrix.

#### Access counting is how the composition claim is checked

`sw/bcmc.h` claims that a primitive is one bus access and that a composition
adds nothing but sequence. That is a claim about traffic, so the harness counts
traffic — `WbMaster::accesses()` before and after every single call — which is
the difference between a documented design rule and an enforced one.

| Call                                   | Accesses  | Why                                   |
| -------------------------------------- | --------- | ------------------------------------- |
| every primitive                        | 1         | one register, one access              |
| `bcmc_start()`                         | **2**     | the stated exception, below           |
| `bcmc_read_cell()`                     | 3         | row, column, read                     |
| `bcmc_read_column()`                   | 1 + words | one index write, one word per 32 rows |
| `bcmc_load()`                          | C + 5 up  | C, N, C weights, START, ≥ 1 poll      |
| a **geometry** refusal (`BCMC_ERANGE`) | **0**     | `CAPS` already said so                |
| a **state** refusal (`BCMC_EREFUSED`)  | **1**     | only the peripheral knows             |

The last two rows are the doctrine of `sw/bcmc.h` made observable. Geometry is
discovered once from `CAPS`, so an out-of-range index is refused locally and the
bus is never touched. State is _unknowable_ without asking, so the driver never
pre-checks `BUSY` or `VALID`: it issues the access and reports the `ERR`. A
driver that started guessing state would show up as an extra access; a driver
that started assuming `MAX_C` would show up as an access that should not exist.
Neither can be introduced quietly.

`bcmc_start()` costs two because `bcmc_wb.v` latches `IRQ_EN` from **every**
`CTRL` write, so a lone `START` write would silently clear the caller's
interrupt enable. It reads `CTRL`, sets `START`, and preserves `IRQ_EN` — and
the harness checks the outcome by reading `CTRL` back rather than by trusting
the comment.

#### A bus that is not a BCMC

One stub, in one place, for one reason: `bcmc_probe()` decides whether a
peripheral is a BCMC at all, and `bcmc_wb.v` **is** one. It has no setting in
which it reports a foreign `ID`, an unsupported major version, or `MAX_C = 0`,
so refusing a foreign device is only observable against a foreign device.

The `Foreign` stub is three constants and a counter, not a model. It serves
`ID`, `VERSION` and `CAPS` and errs on everything else, which is all
`bcmc_probe()` looks at, so there is no behaviour in it to drift out of step
with `docs/Register_Map.md`. Each case differs from the real peripheral's own
reported geometry in exactly one register, and each must be refused **at the
register that settled it** — one read for a foreign `ID`, two for a bad version,
three for `MAX_C = 0`. A probe that read `CAPS` after seeing a foreign `ID`
would be reading a register it has no reason to believe exists.

The last case re-probes a device that was already good, which is the only way to
observe that a _failed_ probe discards the geometry of the peripheral that used
to be there. A stale `MAX_C` is a plausible number, and a range check against it
would go on succeeding silently.

#### Results

| Vector file             | Cases | Checks    | Accesses | Clocks  |
| ----------------------- | ----- | --------- | -------- | ------- |
| `matrix_edge.txt`       | 17    | 24,281    | 4,628    | 13,889  |
| `matrix_exhaustive.txt` | 1,274 | 827,696   | 166,214  | 498,647 |
| `matrix_random.txt`     | 300   | 564,624   | 105,634  | 316,907 |
| all three               | 1,591 | 1,415,185 | 276,056  | 828,173 |

0 failures, 0.65 s for all three, so every case runs on every invocation and
nothing is sampled. Of the 276,056 accesses exactly 5 are refused — the five
`ERR`s the suite provokes on purpose — and no access ever timed out or was
answered twice.

#### Testing the tests

Twenty-seven mutations of `sw/bcmc.c`, one plausible slip each, against
`matrix_edge.txt`:

| Mutation                                       | Caught by                        |
| ---------------------------------------------- | -------------------------------- |
| A lone `START` write, clearing `IRQ_EN`        | `CTRL` read back after the start |
| `done` read as `!BUSY` instead of `VALID`      | reads before `VALID`             |
| `BUSY` and `VALID` confused                    | `BUSY` observed mid-transform    |
| write-1-to-clear aimed at `VALID`, not `IRQ`   | the interrupt suite              |
| `IRQ_EN` written as `START`                    | the interrupt suite              |
| weight index not scaled to a word address      | weight read-back                 |
| `OFFSET[i]` read as `OFFSET[i+1]`              | `reference.py`'s offsets         |
| column word index not scaled                   | `reference.py`'s matrix          |
| `WEIGHT[MAX_C]` treated as mapped              | a geometry refusal that cost 0   |
| `C == MAX_C` refused, though it is legal       | the boundary case                |
| ⌈`MAX_C`/32⌉ computed with the wrong bias      | the column width check           |
| the cell taken from bit 1 of `CELL`            | cell against column              |
| `bcmc_load()` returns before the matrix exists | the first read refused           |
| the weight loop stops one row short            | `reference.py`'s offsets         |
| the weight vector loaded back to front         | `reference.py`'s offsets         |
| the transform never started                    | the first read refused           |
| the poll bound overshot by one                 | `bcmc_wait(dev, 1)` cost         |
| an undersized caller buffer accepted           | a geometry refusal that cost 0   |
| words past the column left as they were        | the oversized-buffer check       |
| `N` written to the `C` register                | `reference.py`'s matrix          |
| a device that claims to be probed              | the pre-probe refusals           |
| any peripheral accepted as a BCMC              | the `Foreign` stub               |
| a failed probe keeps stale geometry            | the re-probe                     |
| a future major version accepted                | the `Foreign` stub               |
| a peripheral with `MAX_C = 0` accepted         | the `Foreign` stub               |
| `CELL` read without selecting the column       | access count and value           |
| `COLUMN` read without selecting the column     | access count and value           |

**All twenty-seven die**, and a mutation the compiler rejects outright counts as
caught by the compiler rather than as a pass.

Three survived the first attempt, and each exposed a real gap in the harness
rather than a false alarm:

- **`BUSY` and `VALID` confused.** Outside a running transform both read zero,
  so nothing in the suite could tell them apart. The fix was to observe `BUSY`
  at the one moment it is certainly set — immediately after starting a transform
  of `C = MAX_C` rows, which needs `MAX_C + 4` clocks while a poll takes three.
- **Any peripheral accepted as a BCMC.** There was no peripheral present that
  was not a BCMC. This is what the `Foreign` stub was written for, and it closed
  the version and `MAX_C = 0` checks at the same time.
- **A failed probe keeps stale geometry.** Every case attached a fresh device,
  and `bcmc_attach()` clears `probed` itself, so clearing it again on entry to
  `bcmc_probe()` was invisible. Only re-probing an already-good device can see
  it.

The mutation runner restores `sw/bcmc.c` in a `finally:` block and rebuilds;
`git diff sw/` confirms the file in the repository is the unmutated one.

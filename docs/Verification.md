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

| Module          | Verilator harness          | Icarus testbench  | Vector format |
| --------------- | -------------------------- | ----------------- | ------------- |
| `bcmc_core.v`   | `sim/bcmc_core_test.cpp`   | `sim/tb_core.v`   | core          |
| `bcmc_cell.v`   | `sim/bcmc_cell_test.cpp`   | `sim/tb_cell.v`   | cell          |
| `bcmc_row.v`    | `sim/bcmc_row_test.cpp`    | `sim/tb_row.v`    | matrix        |
| `bcmc_column.v` | `sim/bcmc_column_test.cpp` | `sim/tb_column.v` | matrix        |

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

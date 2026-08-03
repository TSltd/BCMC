# Computational Verification

Verification happens in three layers, and each layer is only trusted because the
layer above it is:

| Layer                      | Question it answers                                     | Where                                     |
| -------------------------- | ------------------------------------------------------- | ----------------------------------------- |
| 1. **The theorem**         | Is the Balance Theorem true?                            | `validation/verify_conjecture.py`         |
| 2. **The reference model** | Does `reference.py` implement the theorem?              | `validation/test_reference.py`            |
| 3. **The RTL**             | Does the hardware compute what `reference.py` computes? | `sim/bcmc_core_test.cpp`, `sim/tb_core.v` |

No expected value anywhere in this project was written by hand. Every expected
offset originates in `validation/reference.py`, whose own output is checked
against exhaustive enumeration and against the independent implementations in
`validation/verify_conjecture.py`.

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

`rtl/bcmc_core.v` is checked against the reference model by **two independent
simulators reading the same vector files**. The vector files are generated by
`validation/gen_vectors.py`, whose offsets come from `reference.py` and nowhere
else.

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

### Testing the tests

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

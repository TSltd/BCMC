# Balanced Cyclic Matrix Construction (BCMC)

BCMC is a deterministic construction of binary matrices from integer weight vectors. It exactly preserves prescribed row weights while guaranteeing that column occupancies differ by at most one — in fact, the exact occupancy vector is determined entirely by the total weight, independent of how the weight is distributed among the rows.

## Documentation

| Document                                                                   | Contents                                                                           |
| -------------------------------------------------------------------------- | ---------------------------------------------------------------------------------- |
| [docs/BCMC.md](docs/BCMC.md)                                               | The construction: notation, definitions, invariants, and balance theorem statement |
| [docs/Motivation_and_Applications.md](docs/Motivation_and_Applications.md) | Origin, engineering motivation, and application contexts                           |
| [docs/Proof.md](docs/Proof.md)                                             | Full proof of the Balance Theorem, including the critical bijection (Lemma 3)      |
| [docs/Verification.md](docs/Verification.md)                               | Exhaustive and randomized computational verification                               |

## Source

- `src/verify_conjecture.py` — the verification script for the Balance Theorem (see `docs/Verification.md`).

# Computational Verification

The theorem was verified exhaustively and by randomized search (see `src/verify_conjecture.py`):

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

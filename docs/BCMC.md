# Balanced Cyclic Matrix Construction (BCMC)

> **Abstract**
>
> We introduce the **Balanced Cyclic Matrix Construction (BCMC)**, a deterministic construction of binary matrices from integer weight vectors. The BCMC transform exactly preserves prescribed row weights while producing a column occupancy distribution determined solely by the total weight. We prove that if the total weight is `W=qN+r`, then every column has occupancy either `q` or `q+1`, with exactly `r` columns attaining the larger value. The proof follows from a bijection between matrix cells and residue classes of a partition of `[0,W)`. The construction is independent of traversal order and admits efficient software and hardware implementations.

## Notation

| Symbol | Meaning           |
| ------ | ----------------- |
| `N`    | number of columns |
| `C`    | number of rows    |
| `wᵢ`   | weight of row i   |
| `W`    | total weight      |
| `Pᵢ`   | prefix sum        |
| `Iᵢ`   | prefix interval   |
| `M`    | BCMC matrix       |
| `L(j)` | column occupancy  |

## Overview

The **Balanced Cyclic Matrix Construction (BCMC)** algorithm defines a binary matrix from a vector of non-negative integer weights.

Unlike many scheduling algorithms, the BCMC transform is not defined procedurally. It does not require rows to be rotated, matrices to be physically constructed, or columns to be traversed in any particular order.

Instead, the BCMC transform defines a binary relation between row and column indices. Explicit matrices, rotated bit vectors, pointer-based implementations and hardware realisations are simply different representations of the same mathematical object.

Applications such as scheduling, load balancing, PWM, burst firing and event generation are interpretations of the resulting matrix and are intentionally outside the scope of the construction itself.

---

## BCMC Pipeline

The BCMC transform consists of two independent stages.

```
Weight Vector
      │
      ▼
Balanced Cyclic Matrix Construction
      │
      ▼
Binary Matrix M
      │
      ▼
(Optional) Column Permutation τ
      │
      ▼
Application-specific interpretation
```

The BCMC algorithm defines only the binary matrix M.

Any subsequent permutation of the column indices is external to the construction.

Since every permutation is a bijection, all proven properties of BCMC—including row conservation, exact column occupancies and the Balance Theorem—are preserved.

---

## Definitions

Let

- `C` = number of rows
- `N` = number of columns
- `w = (w₀, w₁, ..., w_{C−1})` = weight vector

where

```text
0 ≤ wᵢ ≤ N
```

Define the total weight

```text
W = Σ wᵢ
```

The algorithm defines a binary matrix

```text
M ∈ {0,1}^(C×N)
```

whose rows satisfy the prescribed weights.

---

## Balance Theorem

Let

```text
W=qN+r
```

where

```text
0≤r<N.
```

BCMC satisfies the stronger — in fact exact — property

```text
L(j) ∈ {q,q+1}
```

for every column.

Equivalently, the exact occupancy vector is

```text
L(j) = q+1     for  0 ≤ j < r,
L(j) = q       for  r ≤ j < N.
```

Consequently

- exactly `N−r` columns have occupancy `q`,
- exactly `r` columns have occupancy `q+1`,

and

```text
max(L)-min(L) ≤ 1.
```

A complete proof is given in `docs/Proof.md`. The proof establishes the exact column-wise correspondence

```text
ColumnOccupancy(j) = ResidueCount(j)
```

by a bijection (the critical step, Lemma 3 of the proof), after which the theorem reduces to elementary residue counting. As a corollary, the occupancy vector depends only on `(N, W)` — never on how the weight is distributed among the rows.

---

## Prefix Partition

Rather than beginning with cyclic intervals, BCMC first defines a partition of the interval

```text
[0, W)
```

Define the cumulative prefix sums

```text
P₀ = 0

Pᵢ₊₁ = Pᵢ + wᵢ
```

giving

```text
0 = P₀ ≤ P₁ ≤ ... ≤ P_C = W.
```

The intervals

```text
Iᵢ = [Pᵢ, Pᵢ₊₁)
```

form a disjoint partition of

```text
[0, W).
```

Each interval has length

```text
|Iᵢ| = wᵢ.
```

---

## Projection onto the Cyclic Domain

Let

```text
π(x) = x mod N.
```

Each interval is projected onto the cyclic domain

```text
{0,1,...,N−1}.
```

Since every interval satisfies

```text
wᵢ ≤ N,
```

its projection is always a single contiguous cyclic interval.

Define the binary matrix by

```text
M(i,j)=1
```

iff (i.e., if and only if) there exists an integer `k` such that

```text
j + kN ∈ Iᵢ.
```

Otherwise

```text
M(i,j)=0.
```

This definition is equivalent to the cyclic interval formulation

```text
M(i,j)=1
iff

((j−sᵢ) mod N) < wᵢ
```

where

```text
sᵢ = Pᵢ mod N.
```

---

## Invariants

The following properties follow directly from the construction.

### Row Conservation

Every row contains exactly its prescribed number of ones.

```text
row_sum(i)=wᵢ
```

---

### Binary Values

Every matrix element is either

```text
0 or 1.
```

---

### Canonical Construction

The matrix is uniquely determined by

- `N`
- the weight vector `w`.

No additional state is required.

---

## Consumer Layer and Interpretation

Matrix traversal is intentionally excluded from the BCMC definition.

BCMC produces the canonical representative before permutation.

Traversal defines only how an external consumer observes that matrix.

Consequently, traversal is an interpretation layer rather than part of the algorithm.

```
BCMC

↓

Canonical matrix

↓

Permutation

↓

Application
```

Any bijection

```text
τ : {0,...,N−1} → {0,...,N−1}
```

may be used by a downstream consumer.

Examples include

- sequential order,
- Fisher–Yates permutation,
- Gray-code ordering,
- any deterministic or random permutation.

The consumer observes

```text
M(i,τ(j))
```

rather than

```text
M(i,j).
```

Since every column is visited exactly once, traversal order preserves every structural property of the construction.

Traversal is therefore outside the definition of BCMC.

More generally, BCMC defines a deterministic binary matrix from a weight vector. The algorithm itself is independent of time, scheduling, software or hardware. Any interpretation of the columns as temporal samples, switching events, communication slots, resource allocations or other ordered sequences is external to the mathematical definition.

---

## Column Occupancy

Define the occupancy of column `j` by

```text
L(j)=Σ M(i,j).
```

The average occupancy is

```text
μ=W/N.
```

---

## Geometric Interpretation

The prefix partition provides an intuitive interpretation of the balance property.

The intervals

```text
Iᵢ
```

partition

```text
[0,W)
```

without overlap.

Projecting those intervals modulo `N` folds the partition onto the cyclic domain.

Since

```text
W=qN+r,
```

the interval

```text
[0,W)
```

contains

- `q` complete copies of every residue class modulo `N`,
- together with one additional occurrence of the first `r` residue classes.

The observed column occupancies inherit this distribution exactly; the correspondence is proved in `docs/Proof.md`.

---

## Representations

The BCMC transform is independent of implementation.

Equivalent representations include

- explicit binary matrices,
- rotated bit vectors,
- pointer-offset traversal,
- interval evaluation,
- FPGA logic,
- hardware lookup tables.

All evaluate the same mathematical construction.

---

## Future Work

- Sparse matrix generation.
- Dynamic updates.
- Incremental reconstruction.
- FPGA peripheral implementation.
- Formal verification (Coq/Lean).
- Multi-dimensional BCMC.
- Weighted traversal strategies.
- Applications to arbitration and NoCs.

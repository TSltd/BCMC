# The BCMC Balance Theorem — Proof

## Statement of the Result

**Theorem (BCMC Balance — exact form).**
Let `N ≥ 1`, `C ≥ 0`, and let

```text
w = (w₀, w₁, ..., w_{C−1}),      0 ≤ wᵢ ≤ N
```

be integer weights with total

```text
W = Σwᵢ = qN + r,      0 ≤ r < N.
```

In the BCMC matrix `M ∈ {0,1}^(C×N)` of the prefix-partition construction, the occupancy of column `j` is

```text
L(j) := Σᵢ M(i,j)

L(j) = q + 1      for  0 ≤ j < r
L(j) = q          for  r ≤ j < N
```

Consequences:

1. **Balance.** `maxⱼ L(j) − minⱼ L(j) ≤ 1`. Exactly `r` columns carry `q+1` ones and exactly `N−r` columns carry `q`.
2. **Distribution independence.** The occupancy vector `(L(0), …, L(N−1))` depends only on `N` and `W` — never on how `W` is distributed among the rows.

The proof has exactly one non-tautological ingredient: **Lemma 3**, which establishes the correspondence

```text
ColumnOccupancy(j) = ResidueCount(j)
```

by an explicit bijection. Every other step is elementary counting.

---

## Framework

### The prefix partition

Define the cumulative prefix sums

```text
P₀ = 0,      Pᵢ₊₁ = Pᵢ + wᵢ
```

so that `0 = P₀ ≤ P₁ ≤ ⋯ ≤ P_C = W`, and set

```text
Iᵢ = [Pᵢ, Pᵢ₊₁) = { Pᵢ, Pᵢ+1, …, Pᵢ₊₁−1 }.
```

Each interval has length `|Iᵢ| = wᵢ`.

### The matrix

The BCMC matrix is defined by the characteristic condition

```text
M(i,j) = 1   iff   there exists k ∈ ℤ  with  j + kN ∈ Iᵢ.
```

Equivalently, row `i` marks the projection of `Iᵢ` onto the cyclic domain `{0, …, N−1}`.

---

## Lemma 1 (Partition)

```text
I₀, I₁, …, I_{C−1}  form a disjoint partition of [0,W).
```

**Proof.**
`P₀ = 0` and `P_C = W`, so the union is `[0,W)`. The intervals meet only at shared endpoints — `Pᵢ₊₁` is the right endpoint of `Iᵢ` and the left endpoint of `Iᵢ₊₁` — and the half-open convention places `Pᵢ₊₁` in the later interval, never in the earlier one. Hence every integer `x ∈ [0,W)` lies in **exactly one** interval. Denote it `I_{i(x)}`.

□

---

## Lemma 2 (Residue injectivity in an interval)

For every interval `Iᵢ`, the map

```text
x ↦ x mod N
```

is injective on `Iᵢ`.

**Proof.**
Suppose `a, b ∈ Iᵢ` are distinct integers with `a ≡ b (mod N)`. Then `|a − b|` is a positive multiple of `N`, hence

```text
|a − b| ≥ N.
```

But any two distinct integers in a half-open interval of consecutive integers of length `wᵢ` differ by at most

```text
wᵢ − 1 ≤ N − 1 < N,
```

using the hypothesis `wᵢ ≤ N`. Contradiction. (When `wᵢ = 0` the interval is empty and the claim is vacuous.)

□

_Remark (why the bound is strict)._ Because the interval is half-open, the distance between two _distinct_ elements is at most `wᵢ − 1`, not `wᵢ`. Combined with `wᵢ ≤ N` this gives a distance strictly less than `N`, while any two distinct integers congruent mod `N` are separated by at least `N`. The strictness is essential and is exactly where the hypothesis `wᵢ ≤ N` enters the proof.

---

## Lemma 3 (The critical correspondence)

For every column `j`,

```text
L(j) = ResidueCount(j)
```

where

```text
ResidueCount(j) := #{ x ∈ [0,W) : x ≡ j (mod N) }.
```

**Proof.**
Consider the map

```text
Φ : [0,W) → { (i,j) : M(i,j) = 1 }
Φ(x) = ( i(x), x mod N )
```

where `i(x)` is the unique interval containing `x`, given by Lemma 1.

- **Well-defined.** The row `i(x)` is unique by Lemma 1; the column `x mod N` is determined by `x`; and the cell `(i(x), x mod N)` indeed contains the integer `x ∈ Iᵢ(x)`, so it is a `1` by the characteristic definition of `M`.
- **Injective.** If `Φ(x) = Φ(x′)` then `x` and `x′` lie in the same interval and `x ≡ x′ (mod N)`. Lemma 2 forces `x = x′`.
- **Surjective.** Given a `1` at `(i,j)`, the characteristic definition supplies some `x ∈ Iᵢ` with `x ≡ j (mod N)`. Lemma 2 guarantees that `x` is the _unique_ such element of `Iᵢ`, so `Φ(x) = (i,j)`.

Hence `Φ` is a bijection. Fixing `j` and restricting to the preimage of the set of cells in column `j`:

```text
{ x ∈ [0,W) : x ≡ j (mod N) }  ↔  { (i,j) : M(i,j) = 1 }
```

and the right-hand set has exactly `L(j)` elements. Therefore `L(j) = ResidueCount(j)`.

□

_Remark._ This bijection is the "critical correspondence"; it is the only place in the proof where the construction is used in an essential way. Lemma 1 supplies the unique row for each integer; Lemma 2 supplies the uniqueness that makes the correspondence one-to-one rather than merely a many-to-one collapse. Both directions are genuinely needed: without Lemma 1, `x` might contribute to several rows; without Lemma 2, several integers could collapse onto one matrix cell and the count would be lost.

---

## Lemma 4 (Residue counts)

If `W = qN + r` with `0 ≤ r < N`, then

```text
ResidueCount(j) = q + 1     for  0 ≤ j < r
ResidueCount(j) = q         for  r ≤ j < N.
```

**Proof.**
Decompose

```text
[0,W) = [0,qN)  ∪  [qN, qN+r).
```

The first block splits into `q` consecutive slabs of length `N`:

```text
[0,N), [N,2N), …, [(q−1)N, qN).
```

The slab `[tN, (t+1)N)` contains the integers `tN, tN+1, …, tN+N−1`, whose residues mod `N` are `0, 1, …, N−1` in order. So every residue class occurs exactly once in every slab, i.e. exactly `q` times in `[0,qN)`.

Since `r < N`, the tail `[qN, qN+r)` contains `qN, qN+1, …, qN+r−1`, which contribute one _additional_ occurrence of each residue `0, 1, …, r−1`, and nothing else.

□

---

## Theorem (Exact column occupancy)

With `W = qN + r`, `0 ≤ r < N`, the BCMC column occupancies are

```text
L(j) = q + 1     for  0 ≤ j < r,
L(j) = q         for  r ≤ j < N.
```

**Proof.**
By Lemma 3, `L(j) = ResidueCount(j)` for every column. Lemma 4 evaluates the residue counts explicitly.

□

---

## Corollary 1 (Balance)

```text
maxⱼ L(j) − minⱼ L(j) ≤ 1
```

with exactly `r` columns at `q+1` and exactly `N − r` at `q`.

**Proof.**
Immediate from the theorem. The average occupancy is `μ = W/N ∈ [q, q+1]`, consistent with a distribution of `q`'s and `q+1`'s.

□

---

## Corollary 2 (Distribution independence)

The occupancy vector `(L(0), …, L(N−1))` depends only on `N` and `W`. In particular, any two weight vectors with the same total weight — however differently the weight is split among the rows — produce **identical** column occupancies, cell for cell.

**Proof.**
`L(j) = ResidueCount(j)`, and the residue counts of `[0,W)` depend only on `N` and `W`.

□

_Remark._ This corollary is striking: for a fixed total `W`, the column profile is a function of `(N, W)` alone. Re-splitting, reordering, or arbitrarily redistributing the weight across rows leaves every column occupancy untouched. The earlier exploratory scripts confirmed this on all tested cases (see `docs/Verification.md`).

---

## Necessity of the hypothesis `wᵢ ≤ N`

The hypothesis is not decorative: Lemma 2 — and with it the entire theorem — fails if some `wᵢ > N`.

Take `N = 5`, `w = (7, 3)`. Then `W = 10`, so `q = 2, r = 0`, and the theorem predicts `L(j) = 2` for every column. But the interval

```text
I₀ = [0, 7) = {0,1,2,3,4,5,6}
```

contains `0` and `5`, both congruent to `0` mod `5`: Lemma 2 fails. The binary matrix of the characteristic definition gives column loads

```text
L = [1, 1, 2, 2, 2]    ≠    [2, 2, 2, 2, 2].
```

Worse, even **row conservation** fails: row `0` marks only the residues `{0,1,2,3,4}` — five ones instead of the prescribed seven — so the matrix no longer represents the given weights at all.

_Why the multiplicity view is not a contradiction._ The algorithmic variant used by the exploratory scripts (advancing a cyclic offset and writing `wᵢ` marks) counts with multiplicity, and is trivially equal to `ResidueCount` for arbitrary weights. What forces the balance theorem to be about the binary matrix is the requirement that a cell `(i,j)` holds a single `1`; this is exactly the content of Lemma 2, which is in turn exactly what the hypothesis `wᵢ ≤ N` guarantees.

---

## Remark: the three equivalent forms

Under `wᵢ ≤ N`, the following three descriptions of BCMC agree:

1. **Characteristic (existence):** `M(i,j) = 1` iff some multiple of `N` shifted by `j` lies in `Iᵢ`.
2. **Constructive (assignment):** for each `x ∈ [0,W)`, place a `1` at `(i(x), x mod N)`.
3. **Cyclic-offset (algorithmic):** start at `s₀ = 0`; row `i` marks `sᵢ, sᵢ+1, …, sᵢ+wᵢ−1` mod `N`, then `sᵢ₊₁ = (sᵢ + wᵢ) mod N`.

Form 1 and form 2 agree by Lemma 3's bijection; form 3 is form 2 transported through the residue map. Lemma 2 is precisely the statement that the constructive form is genuinely binary — that no cell is claimed twice.

---

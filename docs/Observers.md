# BCMC Observers

This document is **specification**, not documentation of an implementation.

Reference Observers demonstrate ways to consume the BCMC representation. They
are not part of the BCMC definition and may be replaced or extended without
affecting the mathematical or hardware contracts of the primitive.

That sentence is the whole point of this file, and everything below exists to
make it _checkable_ rather than merely polite. An observer that could change a
proven property of BCMC would not be an observer; it would be a second
construction wearing the first one's name.

```text
docs/BCMC.md          ->  validation/reference.py     ->  rtl/bcmc_{core,cell}.v
docs/Register_Map.md  ->  validation/bcmc_periph.py   ->  rtl/bcmc_wb.v
docs/Observers.md     ->  validation/observers.py     ->  sw/bcmc_observer.{h,c}
```

Where an implementation and this document disagree, this document is right and
the implementation is a bug.

---

## The problem observers exist to solve

The Balance Theorem is a statement about a **set** of columns. It says that if
`W = qN + r` then exactly `r` columns carry `q + 1` and the remaining `N - r`
carry `q`. It says nothing whatever about which columns those are.

For the canonical BCMC matrix, we know which ones they are, and the answer is
lopsided:

```text
L(j) = q + 1   for  0 <= j < r
L(j) = q       for  r <= j < N
```

The heavy columns are all at the bottom of the index range. That is a fact
about the construction, and it is not a defect: the matrix is balanced exactly
as proved. But a consumer that walks `j = 0, 1, 2, ...` and treats each step as
a **tick of time** does not experience a balanced load. It experiences `r` busy
ticks followed by `N - r` quiet ones.

So there are two different notions in play, and BCMC only defines the first:

| Property           | Belongs to   | Established by                       |
| ------------------ | ------------ | ------------------------------------ |
| Balance            | the matrix   | the Balance Theorem, `docs/Proof.md` |
| Smoothness in time | the observer | the traversal order it chooses       |

**Balance is a property of the matrix. Smoothness is a property of the
observer.** An observer cannot create balance that the matrix does not have,
and cannot destroy balance that it does. All an observer can do is decide the
order in which an already-balanced object is looked at.

---

## Definition

Fix a BCMC context: a weight vector `weights[0 .. C-1]` and a modulus `N`
satisfying the hypothesis of the Balance Theorem, and the matrix `M` they
determine.

An **observer** is a finite sequence of **visits**. A visit is a column index
`j`, and the **result** of visiting `j` is the set of rows active in that
column:

```text
R(j) = { i : M(i, j) = 1 },   listed in ascending order of i
```

A **pass** is a sequence of exactly `N` visits. An observer is therefore
completely described by the order in which it emits column indices during a
pass — that is, by a function

```text
pi : { 0, 1, ..., N-1 }  ->  { 0, 1, ..., N-1 }
```

where `pi(t)` is the column visited at step `t`.

Everything else about an observer — when it steps, what it does with `R(j)`,
whether it runs once or forever — is application semantics and is outside this
specification.

---

## The observer contract

An implementation is a conforming observer if and only if it satisfies all
three of the following.

### O1 — Completeness: `pi` is a bijection

Over one pass, every column index in `0 .. N-1` is visited exactly once. No
column is skipped and no column is visited twice.

This is the only structural requirement, and it is not negotiable: it is what
makes an observer a _reordering_ rather than a filter or a resampler.

### O2 — Fidelity: the result is `R(j)`, unmodified

Visiting column `j` yields exactly the rows the characteristic function says
are active in column `j`, in ascending row order. An observer may not add a
row, drop a row, or reorder rows within a column.

Row order within a column is fixed by this document so that two conforming
observers with the same `pi` produce byte-identical output. It is a convention,
not a mathematical claim; the mathematics knows only the set.

### O3 — Determinism: `pi` depends only on its declared inputs

Given the same declared inputs, an observer produces the same `pi` on every
run, on every machine, in every language. An observer may not consult the wall
clock, an uninitialised variable, a pointer value, the C library's `rand()`, or
anything else the caller did not hand it.

A pseudorandom observer is therefore not random. It is a **deterministic
function of a seed**, and the seed is a declared input like any other.
Reproducibility is the requirement; unpredictability is at most a convenience.

---

## What follows from the contract

These are the properties `validation/test_observers.py` and
`sim/bcmc_observer_test.cpp` are obliged to check. They are consequences of O1
and O2 alone, so they hold for **every** conforming observer, including ones
that do not exist yet.

### P1 — Coverage

Over one pass, the emitted events

```text
{ (i, pi(t)) : 0 <= t < N,  i in R(pi(t)) }
```

are exactly the support of `M`, and each is emitted exactly once. In
particular, every pass emits exactly `W = sum(weights)` events.

### P2 — Row conservation is observer-invariant

Counting how many times row `i` is emitted over a pass gives `weights[i]`, for
every `i` and every conforming observer. Row Conservation is a theorem about
`M`; a reordering cannot touch it.

### P3 — Balance is observer-invariant

The multiset `{ L(pi(t)) : 0 <= t < N }` equals the multiset `{ L(j) : 0 <= j <
N }`, which by the Balance Theorem is `r` copies of `q + 1` and `N - r` copies
of `q`. An observer changes the _order_ of the occupancies, never the
_multiset_.

### P4 — Observer equivalence

Any two conforming observers over the same context emit the same events, and
differ only in the order of emission. This is the formal content of "may be
replaced or extended without affecting the mathematical contracts of the
primitive", and it is the property that makes the plural in "Reference
Observers" safe.

---

## Two reference traversals, and why exactly two

What follows are **reference implementations**, in the same sense that
`validation/reference.py` is a reference model: they are pinned down exactly so
that something can be checked against them, and they are not the definition of
anything. There are two of them for one reason — two is the smallest number that
can demonstrate that the choice matters and that the choice changes nothing
proven.

- One is the traversal you get by not choosing (`pi(t) = t`), which is the
  control case.
- One is a traversal that could not plausibly be the same traversal, which is
  what makes the control case falsifiable.

A third would add no obligation that these two do not already discharge, which
is why there is no third. Adding one later is a matter of writing a bijection
and a Python reference for it; it requires no change to `docs/BCMC.md`,
`docs/Register_Map.md`, `sw/bcmc.{h,c}` or `rtl/`, and — as `examples/` is
arranged to show — no change to any application either.

---

## Reference observer 1 — the sequential column iterator

```text
pi(t) = t
```

Declared inputs: `N`.

The identity traversal. It is a conforming observer by inspection, which is
precisely why it is worth having: it is the control case, the thing every other
observer is compared against, and the cheapest possible consumer of the
peripheral — no state but a counter, no storage at all.

It is also the observer that exhibits the lopsidedness described above. That is
not a bug in the observer; it is the canonical matrix being seen in canonical
order.

---

## Reference observer 2 — the deterministic permutation observer

```text
pi = a seeded Fisher-Yates shuffle of the identity
```

Declared inputs: `N` and a 32-bit `seed`.

Any deterministic bijection of `{0, ..., N-1}` would satisfy the contract. A
seeded shuffle is chosen because it is uniform over permutations, needs no
number theory, and imposes no condition on `N`.

O3 has teeth here: "a Fisher-Yates shuffle" is not a specification, because two
implementations of it will disagree unless the random source and the loop
direction are pinned down. Both are therefore fixed below, exactly, so that
`validation/observers.py` and `sw/bcmc_observer.c` are obliged to produce the
same permutation from the same seed.

### The random source

A 32-bit SplitMix generator. All arithmetic is modulo `2^32`; all shifts are
logical.

```text
state <- seed

next():
    state <- (state + 0x9E3779B9)
    z     <- state
    z     <- (z XOR (z >> 16)) * 0x21F0AAAD
    z     <- (z XOR (z >> 15)) * 0x735A2D97
    z     <- (z XOR (z >> 15))
    return z
```

Chosen because it is exactly expressible in C99 with `uint32_t` and in Python
with a mask, has no forbidden seeds (including zero), and needs no 64-bit
arithmetic.

### The bounded draw

To draw uniformly from `0 .. m` inclusive, by rejection:

```text
uniform(m):
    if m == 0:
        return 0
    mask <- the smallest (2^k - 1) that is >= m
    loop:
        x <- next() AND mask
        if x <= m:
            return x
```

Rejection rather than `next() mod (m+1)`, because the modulo is biased and,
more importantly here, because rejection is specified by these five lines and
the bias is not specified by anything.

### The shuffle

Downward, swapping with an index at or below the cursor:

```text
p[i] <- i   for i in 0 .. N-1

for i from N-1 down to 1:
    j <- uniform(i)
    swap p[i] and p[j]

pi(t) = p[t]
```

`i` is included in the draw range: `j == i` is a legal outcome and means the
element stays put. Excluding it would produce a different — and non-uniform —
distribution, and would silently disagree with the Python reference.

---

## What an observer must not do

**An observer must not hold the matrix.** It holds a traversal order. `M` is
evaluated on demand by the peripheral, from `(weights[], offsets[])`, and there
is no framebuffer to go stale. An observer that cached matrix bits would have
to answer for their coherence, and nothing in this project wants that job.

**An observer must not reach past its interface.** It consumes columns through
the driver primitives and knows nothing about wires, addresses or bus errors
beyond the status codes the driver returns.

**An observer must not allocate.** `sw/` has no allocator, by policy. Anything
an observer needs to remember — a permutation of `N` indices, for instance — is
supplied by the caller as a buffer, exactly as `bcmc_read_column()` already
requires. This is why the permutation observer's storage cost appears in its
signature instead of in its footnotes.

**An observer must not be privileged.** Nothing downstream may assume the
sequential observer. The register map deliberately has no "next column"
register, and no observer is permitted to sneak the concept back in below this
line.

---

## An observer is a traversal strategy, not an application

An observer answers exactly one question — **which column next** — and it is
the only question it is allowed to answer. It does not know why it is being
asked. Symmetrically, the thing doing the asking does not know how the answer
was produced.

That symmetry is what makes the two concerns a product rather than a hierarchy:

| Layer       | Supplies | Does not know                |
| ----------- | -------- | ---------------------------- |
| Application | purpose  | how traversal is implemented |
| Observer    | order    | what is done with each visit |

So an application **chooses** a traversal strategy and never **implements** one,
and the number of programs one can build is the number of applications times the
number of traversals. `examples/` is laid out to make that arithmetic visible:
three applications — a printer, a GPIO scheduler, a mains heater controller —
each runnable over either reference traversal, from three source files rather
than six. See `examples/README.md`.

The claim being made there is the one this document opened with, in its
operational form: **an observer contributes order, and nothing else.** It is
checked by running the same application twice, changing only the traversal, and
comparing the two outputs — the summaries must be identical (P1–P4 held) and the
running logs must differ (the traversal really was different). Prose cannot
establish that; a diff can.

The consequence for anyone extending this file is precise. A new traversal
strategy belongs in `sw/bcmc_observer.{h,c}`, specified here first, and every
application acquires it for free. A traversal written inside an application
would be a private strategy that no other application can use and no diff can
check, which is to say it would destroy the property this section exists to
name.

---

## Verification obligations

Following the same rule as everywhere else in this project: an observer is not
checked against expected values invented by a testbench, but against the
reference model that was validated first.

| Obligation                                     | Where                          |
| ---------------------------------------------- | ------------------------------ |
| `pi` is a bijection, for both observers        | `validation/test_observers.py` |
| P1–P3 hold for both observers                  | `validation/test_observers.py` |
| P4: the two observers agree as multisets       | `validation/test_observers.py` |
| The shuffle is uniform over permutations       | `validation/test_observers.py` |
| The C observers match the Python permutation ✔ | `sim/bcmc_observer_test.cpp`   |
| The C observers, driven against real RTL ✔     | `sim/bcmc_observer_test.cpp`   |

The last two matter most. `pi` is a pure function of `(N, seed)`, so the C
implementation can be checked against the Python one index for index — and if
the two ever disagree about a shuffle, the specification above is the tiebreak.

Both are discharged. `validation/gen_observer_vectors.py` writes down what the
Python generator and both order builders produce — 11 seeds, 480 traversals over
40 lengths — into `sim/vectors/observer_prng.txt` and
`sim/vectors/observer_order.txt`, and `sim/bcmc_observer_test.cpp` holds
`sw/bcmc_observer.c` to those tables index for index before it simulates
anything at all. It then loads the same matrices `validation/reference.py`
recorded, through the real driver into the verilated `rtl/bcmc_wb.v`, and
traverses them twice: once in column order and once permuted. O1, O2 and P1–P4
are checked on every case, and every call is metered, because "a visit costs one
`bcmc_read_column()` and a traversal costs nothing" is a claim about traffic and
nothing but counting can settle it.

The negative control is the same one the driver and the RTL were given: 37
plausible observer bugs — a drifted generator constant, an exclusive draw where
the specification says inclusive, a shuffle run upward, a bit numbered from the
wrong end, a cursor that ignores `pi` — planted one at a time in
`sw/bcmc_observer.c`. All 37 were caught, none survived. That number is the
measure of the harness, not of the observer.

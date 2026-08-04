"""
BCMC reference observers.

This module is the *golden model* for `sw/bcmc_observer.{h,c}`, in exactly the
sense that `reference.py` is the golden model for the RTL and `bcmc_periph.py`
is the golden model for the bus wrapper. It implements `docs/Observers.md`, and
where the two disagree the document is right.

    docs/BCMC.md          ->  reference.py     ->  rtl/bcmc_{core,cell}.v
    docs/Register_Map.md  ->  bcmc_periph.py   ->  rtl/bcmc_wb.v
    docs/Observers.md     ->  observers.py     ->  sw/bcmc_observer.{h,c}

An observer is a *traversal order* and nothing else. Every matrix bit it
reports comes from `reference.py`; there is no BCMC mathematics in this file,
which is what makes "an observer cannot change a proven property" checkable
rather than merely claimed.

Notes on scope
--------------
An observer has no notion of time. `observe()` returns a whole pass as a list
because a pass is a finite mathematical object; whether a consumer walks it
once per tick, once per second, or all at once is application semantics and
lives in `examples/`.
"""

from reference import bcmc_column, bcmc_core, check_weights

__all__ = [
    "MASK32",
    "SplitMix32",
    "sequential_order",
    "permuted_order",
    "visit",
    "observe",
    "observe_sequential",
    "observe_permuted",
    "events",
    "occupancy_sequence",
]

MASK32 = 0xFFFFFFFF


# ---------------------------------------------------------------------------
# The random source (docs/Observers.md, "The random source")
# ---------------------------------------------------------------------------

class SplitMix32:
    """
    The 32-bit SplitMix generator fixed by `docs/Observers.md`.

    This class exists so that Python and C99 can be *obliged* to agree. It is
    not a general-purpose PRNG and should never be swapped for one: the exact
    constants, the exact shifts and the exact order of operations are part of
    the specification, because the permutation observer is only reproducible
    if its random source is.

    Zero is a perfectly ordinary seed, unlike in xorshift:

    >>> SplitMix32(0).next() != SplitMix32(1).next()
    True
    >>> all(0 <= SplitMix32(7).next() <= MASK32 for _ in range(4))
    True
    """

    def __init__(self, seed):
        self.state = seed & MASK32

    def next(self):
        """One 32-bit output, advancing the state."""
        self.state = (self.state + 0x9E3779B9) & MASK32
        z = self.state
        z = ((z ^ (z >> 16)) * 0x21F0AAAD) & MASK32
        z = ((z ^ (z >> 15)) * 0x735A2D97) & MASK32
        z = z ^ (z >> 15)
        return z & MASK32

    def uniform(self, m):
        """
        A uniform draw from 0 .. m inclusive, by rejection.

        Rejection rather than `next() % (m + 1)`: the modulo is biased, and a
        bias is only reproducible if it is specified, which it is not.

        >>> rng = SplitMix32(1234)
        >>> all(0 <= rng.uniform(5) <= 5 for _ in range(100))
        True
        >>> SplitMix32(99).uniform(0)
        0
        """
        if m < 0:
            raise ValueError(f"m must be >= 0, got {m}")
        if m == 0:
            return 0
        mask = 1
        while mask < m:
            mask = (mask << 1) | 1
        while True:
            x = self.next() & mask
            if x <= m:
                return x


# ---------------------------------------------------------------------------
# Traversal orders -- pi, and nothing but pi
# ---------------------------------------------------------------------------

def sequential_order(N):
    """
    Reference observer 1: the identity traversal, pi(t) = t.

    >>> sequential_order(6)
    [0, 1, 2, 3, 4, 5]
    """
    if N < 1:
        raise ValueError(f"N must be >= 1, got {N}")
    return list(range(N))


def permuted_order(N, seed):
    """
    Reference observer 2: a seeded Fisher-Yates shuffle of the identity.

    Downward, with `j == i` a legal outcome -- see `docs/Observers.md`.
    Excluding it would be a different and non-uniform shuffle.

    The same seed always gives the same permutation, which is the whole
    requirement; that it looks shuffled is a convenience:

    >>> permuted_order(8, 2024) == permuted_order(8, 2024)
    True
    >>> sorted(permuted_order(37, 5)) == list(range(37))
    True
    >>> permuted_order(1, 12345)
    [0]
    """
    if N < 1:
        raise ValueError(f"N must be >= 1, got {N}")
    rng = SplitMix32(seed)
    p = list(range(N))
    for i in range(N - 1, 0, -1):
        j = rng.uniform(i)
        p[i], p[j] = p[j], p[i]
    return p


# ---------------------------------------------------------------------------
# Visiting
# ---------------------------------------------------------------------------

def visit(weights, offsets, column, N):
    """
    R(j): the rows active in column j, in ascending row order.

    This is a projection of `bcmc_column`, not a new computation -- the
    observer layer contributes the word "which", never the word "is".

    >>> visit([6, 3, 5], [0, 6, 1], 0, 8)
    [0, 1]
    >>> visit([6, 3, 5], [0, 6, 1], 6, 8)
    [1]
    """
    bits = bcmc_column(weights, offsets, column, N)
    return [i for i, b in enumerate(bits) if b]


def observe(weights, N, order):
    """
    One pass, in the given traversal order.

    Returns a list of `(column, rows)` pairs, one per visit. The order is the
    observer's contribution; the rows are the matrix's.

    >>> observe([6, 3, 5], 8, [0, 7])
    [(0, [0, 1]), (7, [1])]
    """
    check_weights(weights, N)
    offsets = bcmc_core(weights, N)
    return [(j, visit(weights, offsets, j, N)) for j in order]


def observe_sequential(weights, N):
    """
    A full pass under reference observer 1.

    Row 0 has weight 2 at offset 0, so it occupies columns 0 and 1; row 1 has
    weight 1 at offset 2. Column 3 is visited and is simply empty -- an empty
    result is a visit, not a skipped one, which is what O1 requires.

    >>> observe_sequential([2, 1], 4)
    [(0, [0]), (1, [0]), (2, [1]), (3, [])]
    """
    return observe(weights, N, sequential_order(N))


def observe_permuted(weights, N, seed):
    """
    A full pass under reference observer 2.

    >>> p = observe_permuted([2, 1], 4, 99)
    >>> sorted(col for col, _ in p)
    [0, 1, 2, 3]
    """
    return observe(weights, N, permuted_order(N, seed))


# ---------------------------------------------------------------------------
# What a pass yields (docs/Observers.md, P1 -- P4)
# ---------------------------------------------------------------------------

def events(pass_):
    """
    The `(row, column)` events emitted by a pass, in emission order.

    P1 says this is the support of M, once each, for every conforming
    observer -- so its *set* is an invariant and its *order* is not.

    >>> events(observe_sequential([2, 1], 4))
    [(0, 0), (0, 1), (1, 2)]
    """
    return [(i, col) for col, rows in pass_ for i in rows]


def occupancy_sequence(pass_):
    """
    L(pi(t)) for t = 0 .. N-1: the occupancy *as the observer experiences it*.

    P3 says the multiset is observer-invariant. The sequence is not, and that
    is the entire reason the permutation observer exists -- the canonical
    matrix puts all of its heavy columns first:

    >>> occupancy_sequence(observe_sequential([6, 3, 5], 8))
    [2, 2, 2, 2, 2, 2, 1, 1]
    >>> sorted(occupancy_sequence(observe_permuted([6, 3, 5], 8, 1)))
    [1, 1, 2, 2, 2, 2, 2, 2]
    """
    return [len(rows) for _, rows in pass_]


# ---------------------------------------------------------------------------
# Self-check
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    import doctest

    failures, tests = doctest.testmod()
    if failures:
        raise SystemExit(f"observers.py: {failures} of {tests} doctests FAILED")
    print(f"observers.py: {tests} doctests passed")

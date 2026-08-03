"""
BCMC executable specification.

This module is the *golden model* for the hardware. It is not a demonstration
and not an experiment: it is the reference against which every RTL module is
verified.

Every function here corresponds one-to-one with a hardware module, and carries
the same signature, so that a Verilog testbench never has to invent expected
values:

    reference.py                RTL
    ------------------------    ----------------------
    bcmc_core(weights, N)       rtl/bcmc_core.v
    bcmc_cell(w, o, j, N)       rtl/bcmc_cell.v
    bcmc_row(w, o, N)           rtl/bcmc_row.v
    bcmc_column(ws, os, j, N)   rtl/bcmc_column.v

The flow of authority is:

    Proof  ->  this module  ->  vectors  ->  RTL

Definitions follow docs/BCMC.md and docs/Proof.md exactly.

Notes on scope
--------------
This module models the *mathematical transform*. It has no notion of clocks,
of `weight_valid`, or of any temporal protocol; those belong to the RTL alone.
"""

from collections import Counter

__all__ = [
    "check_weights",
    "bcmc_core",
    "bcmc_cell",
    "bcmc_row",
    "bcmc_column",
    "bcmc_matrix",
    "column_occupancy",
    "predicted_occupancy",
    "row_weights",
]


# ---------------------------------------------------------------------------
# Preconditions
# ---------------------------------------------------------------------------

def check_weights(weights, N):
    """
    Validate the hypothesis of the Balance Theorem.

        N >= 1
        0 <= w_i <= N   for every i

    The bound `w_i <= N` is not decorative: it is exactly the hypothesis of
    Lemma 2 in docs/Proof.md, and both the theorem and the binary-matrix
    semantics fail without it. Outside this hypothesis the construction is
    simply undefined, so we refuse rather than return a wrong answer.
    """
    if N < 1:
        raise ValueError(f"N must be >= 1, got {N}")
    for i, w in enumerate(weights):
        if not (0 <= w <= N):
            raise ValueError(
                f"weight[{i}] = {w} violates 0 <= w <= N with N = {N}"
            )


# ---------------------------------------------------------------------------
# BCMC Core  --  the canonical prefix transform
# ---------------------------------------------------------------------------

def bcmc_core(weights, N):
    """
    The BCMC Core transform:  weights[] -> offsets[]

        offset[0]   = 0
        offset[i+1] = (offset[i] + weight[i]) mod N

    Returns exactly C offsets, where C = len(weights). offset[i] is the cyclic
    start position of row i, i.e. `P_i mod N`.

    The final accumulator value `P_C mod N` is deliberately NOT returned. It
    belongs to no row, evaluates no matrix element and appears nowhere in the
    proof; see "P_C mod N is not an output" in docs/Hardware_Architecture.md.

    >>> bcmc_core([6, 3, 5], 8)
    [0, 6, 1]
    >>> bcmc_core([], 8)
    []
    """
    check_weights(weights, N)

    offsets = []
    offset = 0

    for w in weights:
        # Emit before update. This is what makes offset[0] == 0 structural.
        offsets.append(offset)

        # Because 0 <= w <= N we have offset + w < 2N, so the reduction is a
        # single comparison and subtraction -- never a division. The RTL
        # implements precisely this.
        s = offset + w
        offset = s - N if s >= N else s

    return offsets


# ---------------------------------------------------------------------------
# BCMC Evaluator  --  the characteristic function
# ---------------------------------------------------------------------------

def bcmc_cell(weight, offset, column, N):
    """
    The characteristic function for a single matrix element.

        M(i,j) = 1  iff  ((j - offset[i]) mod N) < weight[i]

    Depends only on that row's (weight, offset) pair -- never on other rows.
    That independence is what allows the hardware Evaluator to answer arbitrary
    queries with one subtractor and one comparator.

    >>> bcmc_cell(6, 0, 3, 8)
    1
    >>> bcmc_cell(6, 0, 7, 8)
    0
    """
    return 1 if ((column - offset) % N) < weight else 0


def bcmc_row(weight, offset, N):
    """
    One complete row of the BCMC matrix, as a list of N bits.

    Row i marks the wrapped cyclic interval starting at offset[i]:

    >>> bcmc_row(6, 0, 8)
    [1, 1, 1, 1, 1, 1, 0, 0]
    >>> bcmc_row(3, 6, 8)              # marks columns 6, 7, 0 -- wraps
    [1, 0, 0, 0, 0, 0, 1, 1]
    """
    return [bcmc_cell(weight, offset, j, N) for j in range(N)]


def bcmc_column(weights, offsets, column, N):
    """
    One complete column of the BCMC matrix, as a list of C bits.

    Evaluates the characteristic function independently for every row.

    >>> bcmc_column([6, 3, 5], [0, 6, 1], 0, 8)
    [1, 1, 0]
    """
    if len(weights) != len(offsets):
        raise ValueError("weights and offsets must have the same length")
    return [
        bcmc_cell(w, o, column, N)
        for w, o in zip(weights, offsets)
    ]


def bcmc_matrix(weights, N):
    """
    The complete BCMC matrix as a list of C rows of N bits.

    This is the canonical *mathematical* object. The hardware never stores it;
    it stores the lossless prefix representation (weights[], offsets[]) and
    evaluates the matrix on demand.

    >>> for row in bcmc_matrix([6, 3, 5], 8):
    ...     print(row)
    [1, 1, 1, 1, 1, 1, 0, 0]
    [1, 0, 0, 0, 0, 0, 1, 1]
    [0, 1, 1, 1, 1, 1, 0, 0]
    """
    offsets = bcmc_core(weights, N)
    return [
        bcmc_row(w, o, N)
        for w, o in zip(weights, offsets)
    ]


# ---------------------------------------------------------------------------
# Observables used by the Balance Theorem
# ---------------------------------------------------------------------------

def column_occupancy(weights, N):
    """
    L(j) = sum_i M(i,j),  computed from the matrix itself.

    W = 14 = 1*8 + 6, so six columns carry 2 and two carry 1:

    >>> column_occupancy([6, 3, 5], 8)
    [2, 2, 2, 2, 2, 2, 1, 1]
    """
    offsets = bcmc_core(weights, N)
    return [
        sum(bcmc_column(weights, offsets, j, N))
        for j in range(N)
    ]


def predicted_occupancy(W, N):
    """
    The Balance Theorem's closed form, depending only on (N, W):

        L(j) = q + 1   for  0 <= j < r
        L(j) = q       for  r <= j < N

    where W = qN + r.

    >>> predicted_occupancy(14, 8)     # 14 = 1*8 + 6
    [2, 2, 2, 2, 2, 2, 1, 1]
    """
    if N < 1:
        raise ValueError(f"N must be >= 1, got {N}")
    q, r = divmod(W, N)
    return [q + 1 if j < r else q for j in range(N)]


def row_weights(weights, N):
    """
    Row sums of the constructed matrix -- must equal the prescribed weights
    (Row Conservation).

    >>> row_weights([6, 3, 5], 8)
    [6, 3, 5]
    """
    return [sum(row) for row in bcmc_matrix(weights, N)]


# ---------------------------------------------------------------------------
# Self-check
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    import doctest

    failures, tests = doctest.testmod()
    print(f"doctests: {tests - failures}/{tests} passed")

    # The three documented examples, shown as the hardware would see them.
    examples = [
        ([6, 3, 5], 8),
        ([4, 13, 13, 11, 12, 5, 10, 2, 0, 12], 15),
        ([24, 26, 28, 11, 8, 20, 19, 26, 2, 30], 31),
    ]

    for weights, N in examples:
        offsets = bcmc_core(weights, N)
        W = sum(weights)
        q, r = divmod(W, N)
        loads = column_occupancy(weights, N)
        print()
        print(f"N = {N}   C = {len(weights)}   W = {W} = {q}*{N} + {r}")
        print(f"  weights = {weights}")
        print(f"  offsets = {offsets}")
        print(f"  L       = {loads}")
        assert loads == predicted_occupancy(W, N), "Balance Theorem violated"
        assert row_weights(weights, N) == weights, "Row conservation violated"
        assert max(loads) - min(loads) <= 1, "Balance violated"
        assert Counter(loads)[q + 1] == r, "Histogram mismatch"
        print("  OK: row conservation, exact occupancy, balance, histogram")

    if failures:
        raise SystemExit(1)

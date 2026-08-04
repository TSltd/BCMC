"""
Conformance tests for the BCMC reference observers.

`observers.py` is to `docs/Observers.md` what `bcmc_periph.py` is to
`docs/Register_Map.md`. This script is what makes that restatement
trustworthy, and it follows the rules the project has used since v0.2.

  1. **No invented answers.** Every matrix bit an expectation contains is read
     out of `sim/vectors/matrix_*.txt`, which `reference.py` generated and
     which already drive the RTL suites. The observer properties are therefore
     checked against the same object the hardware is checked against.

  2. **The properties are re-derived, not imported.** P1-P4 are recomputed here
     from the vector files, with no help from `observers.py` beyond the pass it
     produced. An observer that quietly dropped a column has to disagree with
     the file, not merely with itself.

  3. **The uniformity test has a negative control.** A test that a shuffle is
     uniform is worthless unless it rejects a shuffle that is not, so the
     classic off-by-one Fisher-Yates bug is implemented here on purpose and
     required to fail. If the bad shuffle ever passes, the good one proves
     nothing.

Run:
    python3 validation/test_observers.py
"""

import os
import sys
from collections import Counter
from itertools import permutations

import reference as ref
from observers import (
    SplitMix32,
    events,
    observe,
    observe_permuted,
    observe_sequential,
    occupancy_sequence,
    permuted_order,
    sequential_order,
)
from test_periph import load_matrix_vectors

VECTOR_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "sim", "vectors")


# ---------------------------------------------------------------------------
# O3 -- the random source is reproducible
# ---------------------------------------------------------------------------

def suite_prng():
    """
    The generator must be a pure function of its seed, must accept every seed,
    and must stay inside 32 bits. It does not have to be a good generator; it
    has to be an *exactly specified* one, because C99 has to reproduce it.
    """
    fails = []

    for seed in (0, 1, 2, 0xFFFFFFFF, 0x9E3779B9, 123456789):
        a = [SplitMix32(seed).next() for _ in range(1)]
        b = [SplitMix32(seed).next() for _ in range(1)]
        if a != b:
            fails.append(f"seed {seed:#x}: not reproducible")

        rng = SplitMix32(seed)
        vals = [rng.next() for _ in range(1000)]
        if any(v < 0 or v > 0xFFFFFFFF for v in vals):
            fails.append(f"seed {seed:#x}: output left 32 bits")
        if len(set(vals)) < 990:
            fails.append(f"seed {seed:#x}: {1000 - len(set(vals))} collisions in 1000")

    # Zero is an ordinary seed. In xorshift it is a fixed point, and an
    # observer whose seed happened to be zero would never move.
    if SplitMix32(0).next() == 0:
        fails.append("seed 0 is a fixed point")

    # Distinct seeds must not be silently the same stream.
    firsts = [SplitMix32(s).next() for s in range(256)]
    if len(set(firsts)) != 256:
        fails.append("distinct seeds collided on their first output")

    # uniform(m) must stay in range, for every m, including the m == 0 case
    # that the rejection loop is not allowed to enter.
    rng = SplitMix32(4242)
    for m in list(range(0, 40)) + [63, 64, 65, 1000]:
        for _ in range(50):
            x = rng.uniform(m)
            if not 0 <= x <= m:
                fails.append(f"uniform({m}) returned {x}")
                break

    if fails:
        for f in fails:
            print(f"  FAIL {f}")
        return False
    print("  the generator is reproducible, 32-bit, and total over all seeds.")
    return True


# ---------------------------------------------------------------------------
# O1 -- pi is a bijection
# ---------------------------------------------------------------------------

def suite_bijection():
    """
    Over one pass every column is visited exactly once. This is the whole
    structural content of the contract, so it is checked over a wide sweep of
    N rather than a token one.
    """
    fails = []
    checked = 0

    for N in list(range(1, 65)) + [100, 255, 256, 257, 1000]:
        identity = list(range(N))

        if sequential_order(N) != identity:
            fails.append(f"N={N}: the sequential order is not the identity")
        checked += 1

        for seed in (0, 1, 7, 2024, 0xFFFFFFFF):
            p = permuted_order(N, seed)
            if len(p) != N:
                fails.append(f"N={N} seed={seed}: pass has {len(p)} visits, not N")
            elif sorted(p) != identity:
                fails.append(f"N={N} seed={seed}: not a permutation of 0..N-1")
            checked += 1

    # O3 again, at the level of the whole permutation.
    for N, seed in ((37, 5), (64, 99), (256, 0)):
        if permuted_order(N, seed) != permuted_order(N, seed):
            fails.append(f"N={N} seed={seed}: permutation not reproducible")

    if fails:
        for f in fails[:10]:
            print(f"  FAIL {f}")
        return False
    print(f"  {checked:,} traversal orders, every one a bijection of 0..N-1.")
    return True


# ---------------------------------------------------------------------------
# The shuffle is uniform -- and the test knows the difference
# ---------------------------------------------------------------------------

def naive_permuted_order(N, seed):
    """
    The classic Fisher-Yates bug, implemented deliberately.

    Drawing from the *whole* range at every step instead of from 0..i gives
    N^N equally likely execution paths mapped onto N! permutations, and since
    N! does not divide N^N for N > 2 the result cannot be uniform. It is still
    a bijection, so O1 alone would not catch it: only the distribution does.

    This exists to be rejected. Nothing in the shipping model calls it.
    """
    rng = SplitMix32(seed)
    p = list(range(N))
    for i in range(N - 1, 0, -1):
        j = rng.uniform(N - 1)
        p[i], p[j] = p[j], p[i]
    return p


def chi_square(order_fn, N, trials):
    """Goodness of fit of the observed permutation counts against uniform."""
    counts = Counter(tuple(order_fn(N, seed)) for seed in range(trials))
    total = len(list(permutations(range(N))))
    expected = trials / total
    return sum(
        (counts.get(perm, 0) - expected) ** 2 / expected
        for perm in permutations(range(N))
    ), len(counts), total


def suite_uniformity():
    """
    Uniformity is not required by the contract -- any deterministic bijection
    conforms -- but it is the reason a shuffle was chosen over an affine map,
    so it is worth holding the reference to it.

    N = 4 has 24 permutations. With 24,000 seeds each should appear about
    1,000 times. The 0.1% critical value for 23 degrees of freedom is about
    49.7; the threshold below is deliberately slack, because the point is to
    separate "uniform" from "badly broken", not to do statistics.
    """
    N, trials, threshold = 4, 24_000, 80.0
    fails = []

    good, seen, total = chi_square(permuted_order, N, trials)
    if seen != total:
        fails.append(f"the shuffle produced only {seen} of {total} permutations")
    if good > threshold:
        fails.append(f"chi-square {good:.1f} exceeds {threshold} -- not uniform")

    bad, _, _ = chi_square(naive_permuted_order, N, trials)
    if bad <= threshold:
        fails.append(
            f"NEGATIVE CONTROL FAILED: the off-by-one shuffle scored {bad:.1f}, "
            f"which this test would have accepted"
        )

    if fails:
        for f in fails:
            print(f"  FAIL {f}")
        return False
    print(f"  all {total} permutations of {N} elements seen in {trials:,} seeds.")
    print(f"  chi-square {good:.1f} (threshold {threshold:.0f}).")
    print(f"  negative control: the off-by-one shuffle scores {bad:.1f} and is rejected.")
    return True


# ---------------------------------------------------------------------------
# O2 and P1-P4, against the reference vectors
# ---------------------------------------------------------------------------

def support_of(case):
    """{(row, column)} read straight out of the vector file."""
    return {
        (i, j)
        for i, row in enumerate(case["rows"])
        for j, bit in enumerate(row)
        if bit
    }


def check_case(case, seed):
    """
    Every obligation of `docs/Observers.md` for one reference matrix, with all
    expectations taken from the file rather than from `observers.py`.
    """
    N, C = case["N"], case["C"]
    weights, rows, loads = case["weights"], case["rows"], case["loads"]
    fails = []

    passes = {
        "sequential": observe_sequential(weights, N),
        "permuted": observe_permuted(weights, N, seed),
    }

    expected_support = support_of(case)
    W = sum(weights)

    for name, p in passes.items():
        # O1: N visits, each column once.
        if len(p) != N:
            fails.append(f"{name}: {len(p)} visits, expected N = {N}")
            continue
        visited = [col for col, _ in p]
        if sorted(visited) != list(range(N)):
            fails.append(f"{name}: visits are not a permutation of 0..N-1")

        # O2: the rows for a column are exactly the file's, ascending.
        for col, got in p:
            want = [i for i in range(C) if rows[i][col]]
            if got != want:
                fails.append(f"{name}: column {col} gave {got}, file says {want}")
                break

        ev = events(p)

        # P1: coverage, exactly once each, W in total.
        if len(ev) != W:
            fails.append(f"{name}: emitted {len(ev)} events, expected W = {W}")
        if len(set(ev)) != len(ev):
            fails.append(f"{name}: emitted a duplicate event")
        if set(ev) != expected_support:
            fails.append(f"{name}: emitted events are not the support of M")

        # P2: row conservation survives reordering.
        per_row = Counter(i for i, _ in ev)
        got_weights = [per_row.get(i, 0) for i in range(C)]
        if got_weights != list(weights):
            fails.append(f"{name}: row counts {got_weights} != weights {list(weights)}")

        # P3: the occupancy multiset is the file's, and the Balance Theorem's.
        occ = occupancy_sequence(p)
        if sorted(occ) != sorted(loads):
            fails.append(f"{name}: occupancy multiset differs from the file")
        if sorted(occ) != sorted(ref.predicted_occupancy(W, N)):
            fails.append(f"{name}: occupancy multiset differs from the Balance Theorem")

    # The sequential observer sees the canonical order, so its occupancy
    # *sequence* -- not just its multiset -- must be the file's L line.
    if occupancy_sequence(passes["sequential"]) != list(loads):
        fails.append("sequential: occupancy sequence differs from the file's L line")

    # P4: the two observers differ in order and in nothing else.
    if set(events(passes["sequential"])) != set(events(passes["permuted"])):
        fails.append("P4: the two observers emitted different events")

    return fails


def suite_vectors(files):
    total = 0
    for name, stride in files:
        path = os.path.join(VECTOR_DIR, name)
        cases = load_matrix_vectors(path)
        used = 0
        for n, case in enumerate(cases):
            if n % stride:
                continue
            fails = check_case(case, seed=n * 2654435761 & 0xFFFFFFFF)
            used += 1
            total += 1
            if fails:
                print(f"  FAIL [{name} case {n}]  N={case['N']} weights={case['weights']}")
                for f in fails:
                    print(f"        {f}")
                return False
        print(f"  {used:,} of {len(cases):,} cases from {name} passed.")
    print(f"  {total:,} reference matrices observed two ways, with identical results.")
    return True


# ---------------------------------------------------------------------------
# The observer contributes order, and only order
# ---------------------------------------------------------------------------

def suite_order_only():
    """
    The sharpest statement of P4: for an *arbitrary* order, not just the two
    reference ones, sorting the pass by column reproduces the sequential pass
    exactly. If an observer could change anything but order, this would fail.
    """
    fails = []
    checked = 0

    for weights, N in (
        ([6, 3, 5], 8),
        ([1], 1),
        ([0, 0, 0], 5),
        ([4, 4, 4, 4], 4),
        ([7, 2, 9, 1, 5], 11),
        ([32] * 8, 32),
    ):
        base = observe_sequential(weights, N)
        for seed in range(12):
            p = observe_permuted(weights, N, seed)
            if sorted(p) != sorted(base):
                fails.append(f"weights={weights} N={N} seed={seed}: not a reordering")
            checked += 1

        # A hand-written order that is neither reference observer.
        reversed_order = list(range(N - 1, -1, -1))
        if sorted(observe(weights, N, reversed_order)) != sorted(base):
            fails.append(f"weights={weights} N={N}: reversal changed the pass")
        checked += 1

    if fails:
        for f in fails[:10]:
            print(f"  FAIL {f}")
        return False
    print(f"  {checked:,} passes, every one a pure reordering of the sequential pass.")
    return True


# ---------------------------------------------------------------------------
# Preconditions
# ---------------------------------------------------------------------------

def suite_preconditions():
    """
    Observers inherit the hypothesis of the Balance Theorem and add nothing.
    Outside it the construction is undefined, so they refuse rather than
    return a wrong answer -- the same rule `check_weights` already applies.
    """
    fails = []

    for N in (0, -1):
        for fn, args in ((sequential_order, (N,)), (permuted_order, (N, 1))):
            try:
                fn(*args)
                fails.append(f"{fn.__name__}({args}) accepted N = {N}")
            except ValueError:
                pass

    for weights, N in (([9], 8), ([-1], 8), ([1, 2, 3], 0)):
        try:
            observe_sequential(weights, N)
            fails.append(f"observe_sequential({weights}, {N}) accepted bad input")
        except ValueError:
            pass

    # An empty context is legal: C = 0 means no rows, so every column is empty
    # and a pass is N visits that report nothing. Emptiness is not an error.
    p = observe_sequential([], 4)
    if len(p) != 4 or events(p) != []:
        fails.append("an empty weight vector did not give four empty visits")

    if fails:
        for f in fails:
            print(f"  FAIL {f}")
        return False
    print("  the hypothesis of the Balance Theorem is inherited, not weakened.")
    return True


# ---------------------------------------------------------------------------

def main():
    results = []

    print("The random source:")
    results.append(suite_prng())
    print()

    print("O1 -- every traversal order is a bijection:")
    results.append(suite_bijection())
    print()

    print("The shuffle is uniform, and the test can tell:")
    results.append(suite_uniformity())
    print()

    print("O2 and P1-P4, against the reference matrices:")
    results.append(suite_vectors([
        ("matrix_edge.txt", 1),
        ("matrix_random.txt", 1),
        ("matrix_exhaustive.txt", 7),
    ]))
    print()

    print("An observer contributes order, and only order:")
    results.append(suite_order_only())
    print()

    print("Preconditions:")
    results.append(suite_preconditions())
    print()

    if all(results):
        print("observers.py satisfies docs/Observers.md.")
        print("The observer contract is now an executable contract for sw/bcmc_observer.c.")
        return 0

    print("CONFORMANCE FAILED")
    return 1


if __name__ == "__main__":
    sys.exit(main())

"""
Validation of the executable specification itself.

`reference.py` is the golden model for all RTL. Before it can be trusted in
that role it must be shown to agree with the independent implementations that
were used to validate the mathematics in the first place.

This script cross-checks `reference.py` against:

  1. `itertools.accumulate` -- a prefix sum written a completely different way,
     with a real `%` operator rather than a conditional subtraction.
  2. `verify_conjecture.bcmc_algorithmic` -- the cyclic/offset form.
  3. `verify_conjecture.bcmc_interval`   -- the interval/projection form.
  4. `verify_conjecture.bcmc_binary`     -- the TRUE binary (0,1) matrix form.
  5. `verify_conjecture.residue_counts`  -- the closed form of Lemma 4.

It also re-establishes, through `reference.py` alone, the properties proved in
docs/Proof.md: row conservation, exact occupancy, balance, the histogram, and
distribution independence.

Run:
    python3 validation/test_reference.py
"""

import random
import sys
from collections import Counter
from itertools import accumulate, product

import reference as ref
import verify_conjecture as vc


# ---------------------------------------------------------------------------
# An independent prefix implementation
# ---------------------------------------------------------------------------

def offsets_via_accumulate(weights, N):
    """
    The offsets computed a deliberately different way: a full prefix sum over
    the integers, reduced with a true modulo, dropping the final element.

        offsets = [P_0 mod N, ..., P_{C-1} mod N]

    Sharing no code with `reference.bcmc_core`, this catches errors in the
    conditional-subtraction trick and in the emit-before-update ordering.
    """
    prefixes = [0] + list(accumulate(weights))
    return [p % N for p in prefixes[:-1]]


# ---------------------------------------------------------------------------
# One case
# ---------------------------------------------------------------------------

def check_case(weights, N):
    """Return a list of failure messages (empty if the case passes)."""
    fails = []
    weights = list(weights)
    W = sum(weights)
    q, r = divmod(W, N)

    # --- the Core transform -------------------------------------------------
    offsets = ref.bcmc_core(weights, N)

    if len(offsets) != len(weights):
        fails.append(f"len(offsets)={len(offsets)} != C={len(weights)}")

    if weights and offsets[0] != 0:
        fails.append(f"offset[0]={offsets[0]} != 0")

    # The defining recurrence, asserted directly and independently.
    for i in range(len(weights) - 1):
        expect = (offsets[i] + weights[i]) % N
        if offsets[i + 1] != expect:
            fails.append(
                f"recurrence broken at i={i}: "
                f"offset[{i+1}]={offsets[i+1]} != {expect}"
            )
            break

    if offsets != offsets_via_accumulate(weights, N):
        fails.append("bcmc_core != independent accumulate-based prefix")

    if any(not (0 <= o < N) for o in offsets):
        fails.append("offset outside [0, N)")

    # --- the Evaluator ------------------------------------------------------
    if ref.row_weights(weights, N) != weights:
        fails.append("row conservation violated")

    loads = ref.column_occupancy(weights, N)

    # --- agreement with the three independent forms -------------------------
    if loads != vc.bcmc_algorithmic(weights, N):
        fails.append("occupancy != verify_conjecture.bcmc_algorithmic")
    if loads != vc.bcmc_interval(weights, N):
        fails.append("occupancy != verify_conjecture.bcmc_interval")
    if loads != vc.bcmc_binary(weights, N):
        fails.append("occupancy != verify_conjecture.bcmc_binary")
    if loads != vc.residue_counts(W, N):
        fails.append("occupancy != verify_conjecture.residue_counts")

    # --- the Balance Theorem, via reference.py alone ------------------------
    if loads != ref.predicted_occupancy(W, N):
        fails.append(f"occupancy {loads} != predicted {ref.predicted_occupancy(W, N)}")
    if loads and max(loads) - min(loads) > 1:
        fails.append("balance violated: max - min > 1")
    if sum(loads) != W:
        fails.append("total occupancy != W")

    hist = Counter(loads)
    if hist.get(q + 1, 0) != r or hist.get(q, 0) != N - r:
        fails.append(
            f"histogram mismatch: {hist.get(q+1,0)} columns at q+1, expected r={r}"
        )

    # --- the matrix is genuinely binary (Lemma 2) ---------------------------
    for row in ref.bcmc_matrix(weights, N):
        if any(b not in (0, 1) for b in row):
            fails.append("matrix contains a non-binary value")
            break

    return fails


def report(name, weights, N, fails):
    print(f"  FAIL [{name}]  N={N}  weights={weights}")
    for f in fails:
        print(f"        {f}")


# ---------------------------------------------------------------------------
# Suites
# ---------------------------------------------------------------------------

def suite_edge():
    cases = [
        ([], 5),                    # C = 0
        ([], 1),
        ([0, 0, 0], 7),             # W = 0
        ([0], 1),
        ([1], 1),                   # N = 1, weight = N
        ([1, 1, 1, 1], 1),          # N = 1 throughout
        ([5], 5),                   # single full row
        ([5, 5], 5),
        ([1, 1, 1, 1, 1], 5),
        ([0, 4, 0, 4], 4),          # interleaved zeros
        ([4, 2, 0, 4, 3], 5),
        ([10, 10], 10),
        ([10, 0, 0, 10], 10),
        ([6, 3, 5], 8),             # the documented examples
        ([4, 13, 13, 11, 12, 5, 10, 2, 0, 12], 15),
        ([24, 26, 28, 11, 8, 20, 19, 26, 2, 30], 31),
    ]
    ok = True
    for weights, N in cases:
        fails = check_case(weights, N)
        if fails:
            ok = False
            report("edge", weights, N, fails)
    if ok:
        print(f"  {len(cases)} edge cases passed.")
    return ok


def suite_exhaustive(N_max, C_max):
    checked = 0
    for N in range(1, N_max + 1):
        for C in range(0, C_max + 1):
            for weights in product(range(N + 1), repeat=C):
                checked += 1
                fails = check_case(weights, N)
                if fails:
                    report("exhaustive", list(weights), N, fails)
                    return False
    print(f"  {checked:,} exhaustive cases (N<={N_max}, C<={C_max}) passed.")
    return True


def suite_random(num, max_N, max_C):
    for _ in range(num):
        N = random.randint(1, max_N)
        C = random.randint(0, max_C)
        weights = [random.randint(0, N) for _ in range(C)]
        fails = check_case(weights, N)
        if fails:
            report("random", weights, N, fails)
            return False
    print(f"  {num:,} random cases (N<={max_N}, C<={max_C}) passed.")
    return True


def suite_large():
    """
    Large N and C. The full matrix is far too big to build, so only the Core
    transform and its recurrence are checked here.
    """
    cases = [(10_000, 200), (65_535, 64), (4096, 1024)]
    for N, C in cases:
        weights = [random.randint(0, N) for _ in range(C)]
        offsets = ref.bcmc_core(weights, N)
        if offsets != offsets_via_accumulate(weights, N):
            print(f"  FAIL [large]  N={N} C={C}: prefix mismatch")
            return False
        if offsets[0] != 0:
            print(f"  FAIL [large]  N={N} C={C}: offset[0] != 0")
            return False
        for i in range(C - 1):
            if offsets[i + 1] != (offsets[i] + weights[i]) % N:
                print(f"  FAIL [large]  N={N} C={C}: recurrence broken at {i}")
                return False
    print(f"  {len(cases)} large cases (N up to 65,535, C up to 1,024) passed.")
    return True


def suite_distribution_independence(num):
    """
    Corollary 2: the occupancy vector depends only on (N, W), never on how W is
    distributed among the rows.
    """
    for _ in range(num):
        N = random.randint(1, 24)
        C = random.randint(2, 40)
        weights = [random.randint(0, N) for _ in range(C)]
        base = ref.column_occupancy(weights, N)
        for _ in range(4):
            other = vc.perturb(weights, N, 200)
            if sum(other) != sum(weights):
                print("  FAIL [independence]: perturbation changed W")
                return False
            if ref.column_occupancy(other, N) != base:
                print(f"  FAIL [independence]  N={N}")
                print(f"        weights = {weights}")
                print(f"        other   = {other}")
                return False
    print(f"  {num:,} redistributions left the occupancy vector unchanged.")
    return True


def suite_preconditions():
    """The specification must refuse inputs outside the theorem's hypothesis."""
    bad = [
        ([1], 0),        # N = 0
        ([1], -1),       # N < 0
        ([6], 5),        # w > N  -- the case from docs/Proof.md
        ([7, 3], 5),     # the explicit counterexample in the proof
        ([-1], 5),       # negative weight
    ]
    ok = True
    for weights, N in bad:
        try:
            ref.bcmc_core(weights, N)
        except ValueError:
            continue
        print(f"  FAIL [precondition]: accepted invalid input N={N} w={weights}")
        ok = False
    if ok:
        print(f"  {len(bad)} out-of-specification inputs correctly rejected.")
    return ok


# ---------------------------------------------------------------------------

def main():
    random.seed(20260731)
    results = []

    print("Preconditions:")
    results.append(suite_preconditions())
    print()

    print("Edge cases:")
    results.append(suite_edge())
    print()

    print("Exhaustive (N<=5, C<=5):")
    results.append(suite_exhaustive(5, 5))
    print()

    print("Exhaustive (N<=7, C<=4):")
    results.append(suite_exhaustive(7, 4))
    print()

    print("Random:")
    results.append(suite_random(3_000, max_N=64, max_C=64))
    print()

    print("Large:")
    results.append(suite_large())
    print()

    print("Distribution independence:")
    results.append(suite_distribution_independence(500))
    print()

    if all(results):
        print("reference.py agrees with every independent implementation.")
        print("The executable specification is validated.")
        return 0

    print("VALIDATION FAILED")
    return 1


if __name__ == "__main__":
    sys.exit(main())

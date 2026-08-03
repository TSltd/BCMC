"""
Deep verification of the BCMC Balance Theorem.

Checks, for a large space of inputs:
  1. Algorithmic (cyclic) form and interval (projection) form agree.
  2. Column-by-column:  L(j) = ResidueCount(j)  for EVERY column j.
  3. Stronger exact prediction:  L(j) = q+1 for j < r,  L(j) = q for j >= r,
     where W = qN + r.  (The occupancy vector depends ONLY on (N, W).)
  4. max(L) - min(L) <= 1.
  5. Histogram:  exactly r columns with q+1, exactly N-r with q.
  6. Edge cases:  C=0, W=0, N=1, all weights zero.
  7. Independence:  re-distributing the same total weight among rows leaves the
     occupancy vector unchanged.
  8. Informational:  what breaks if the hypothesis w_i <= N is violated.
"""

import random
from collections import Counter
from itertools import product


def bcmc_algorithmic(weights, N):
    """BCMC in the cyclic/algorithmic form used by the test scripts."""
    loads = [0] * N
    s = 0
    for w in weights:
        for t in range(w):
            loads[(s + t) % N] += 1
        s = (s + w) % N
    return loads


def bcmc_interval(weights, N):
    """BCMC in the interval/projection form (the BCMC.md definition)."""
    loads = [0] * N
    P = 0
    for w in weights:
        for x in range(P, P + w):
            loads[x % N] += 1
        P += w
    return loads


def bcmc_binary(weights, N):
    """
    TRUE binary-matrix form per BCMC.md:  M(i,j) = 1 iff exists x in I_i
    with x mod N = j.  Column sums of the actual (0,1) matrix.

    For w_i <= N this coincides with bcmc_algorithmic / bcmc_interval
    (that coincidence IS the content of Lemma 2).  For w_i > N it does
    not: multiplicity is lost.
    """
    loads = [0] * N
    P = 0
    for w in weights:
        seen = set()
        for x in range(P, P + w):
            seen.add(x % N)
        for j in seen:
            loads[j] += 1
        P += w
    return loads


def residue_counts(W, N):
    """ResidueCount(j) = #{ x in [0,W) : x congruent j (mod N) }."""
    q, r = divmod(W, N)
    return [q + 1 if j < r else q for j in range(N)]


def check(weights, N):
    """Return (ok, messages, loads)."""
    W = sum(weights)
    q, r = divmod(W, N)

    alg = bcmc_algorithmic(weights, N)
    intv = bcmc_interval(weights, N)
    binm = bcmc_binary(weights, N)
    pred = residue_counts(W, N)

    ok = True
    msg = []

    if alg != intv:
        ok = False
        msg.append("algorithmic form != interval form")

    if alg != binm:
        ok = False
        msg.append(
            "multiplicity form != binary (0,1) matrix form  "
            "-- Lemma 2 consequence violated"
        )

    if alg != pred:
        ok = False
        msg.append(f"loads {alg} != predicted {pred}")

    if max(alg) - min(alg) > 1:
        ok = False
        msg.append("max-min > 1")

    hist = Counter(alg)
    if hist.get(q + 1, 0) != r or hist.get(q, 0) != N - r:
        ok = False
        msg.append(f"histogram mismatch (q+1 count {hist.get(q+1,0)} != r={r})")

    if sum(alg) != W:
        ok = False
        msg.append("row conservation violated")

    return ok, msg, alg


def exhaustive(N_max, C_max):
    checked = 0
    for N in range(1, N_max + 1):
        for C in range(0, C_max + 1):
            for weights in product(range(N + 1), repeat=C):
                checked += 1
                ok, msg, _ = check(list(weights), N)
                if not ok:
                    print("  COUNTEREXAMPLE:", N, weights, msg)
                    return False
    print(
        f"  All {checked:,} cases with 1<=N<={N_max}, 0<=C<={C_max} passed."
    )
    return True


def random_within_hypothesis(num_tests, max_N, max_C):
    for t in range(num_tests):
        N = random.randint(1, max_N)
        C = random.randint(1, max_C)
        weights = [random.randint(0, N) for _ in range(C)]
        ok, msg, _ = check(weights, N)
        if not ok:
            print("  COUNTEREXAMPLE:", N, weights, msg)
            return False
    print(f"  {num_tests:,} random cases (N<={max_N}, C<={max_C}, w_i<=N) passed.")
    return True


def random_extra_large(num_cases, N, C):
    for _ in range(num_cases):
        weights = [random.randint(0, N) for _ in range(C)]
        ok, msg, _ = check(weights, N)
        if not ok:
            print("  COUNTEREXAMPLE:", N, weights, msg)
            return False
    print(f"  {num_cases} extra-large cases (N={N}, C={C}, W~{C*N//2:,}) passed.")
    return True


def perturb(weights, N, steps):
    """Move units between rows uniformly at random, preserving W and bounds."""
    ws = list(weights)
    C = len(ws)
    for _ in range(steps):
        if C < 2:
            break
        i = random.randrange(C)
        k = random.randrange(C)
        if i == k:
            continue
        if ws[i] == 0:
            continue
        if ws[k] == N:
            continue
        ws[i] -= 1
        ws[k] += 1
    return ws


def independence_test(num_tests, max_N, max_C, pert_steps=2000):
    tested = 0
    for _ in range(num_tests):
        N = random.randint(1, 20)
        C = random.randint(2, max_C)
        weights = [random.randint(0, N) for _ in range(C)]
        base = bcmc_algorithmic(weights, N)
        W = sum(weights)
        q, r = divmod(W, N)
        pred = residue_counts(W, N)
        for _ in range(5):
            other = perturb(weights, N, pert_steps)
            tested += 1
            if bcmc_algorithmic(other, N) != base:
                print("  DISTRIBUTION-INDEPENDENCE FAILURE")
                print("   N =", N, "W =", W)
                print("   weights =", weights)
                print("   other   =", other)
                print("   loads   =", base)
                print("   pred    =", pred)
                return False
    print(f"  {tested:,} re-distributions of total weight produced identical occupancy vectors.")
    return True


def random_violating(num_tests, max_N, max_C, factor=3):
    """
    INFORMATIONAL: behaviour of the TRUE binary matrix outside the
    hypothesis w_i <= N.  (The multiplicity-counting form is trivially
    identical to residue counting for ANY weights, so it cannot show a
    failure; the binary (0,1) matrix is the object that can break.)
    """
    corr_bad = 0
    balance_bad = 0
    example = None
    for _ in range(num_tests):
        N = random.randint(2, max_N)
        C = random.randint(1, max_C)
        weights = [random.randint(0, N * factor) for _ in range(C)]
        W = sum(weights)
        q, r = divmod(W, N)
        alg = bcmc_binary(weights, N)
        pred = residue_counts(W, N)
        if alg != pred:
            corr_bad += 1
            if example is None:
                example = (N, weights, alg, pred)
        if max(alg) - min(alg) > 1:
            balance_bad += 1
    print(f"  {num_tests:,} cases with w_i up to {factor}N (TRUE binary matrix):")
    print(f"    exact correspondence L(j)=ResidueCount(j) failed in {corr_bad:,} cases")
    print(f"    weak balance (max-min<=1) failed in {balance_bad:,} cases")
    if example:
        N, weights, alg, pred = example
        print(f"    example: N={N}, weights={weights}")
        print(f"      loads         = {alg}")
        print(f"      residue counts= {pred}")
    return True


def edge_cases():
    cases = [
        ([], 5),              # C = 0
        ([0, 0, 0], 7),       # W = 0
        ([5], 1),             # N = 1
        ([1, 1, 1, 1, 1], 5), # full row count
        ([5], 5),             # single full row
        ([5, 5], 5),
        ([0, 4, 0, 4], 4),    # interleaved zeros
        ([4, 2, 0, 4, 3], 5),
        ([10, 10], 10),       # two full rows
        ([10, 0, 0, 10], 10),
    ]
    for weights, N in cases:
        ok, msg, alg = check(list(weights), N)
        W = sum(weights)
        q, r = divmod(W, N)
        status = "OK" if ok else "FAIL: " + ", ".join(msg)
        print(f"    N={N:3d} w={str(weights):35s} W={W:4d}  loads={alg}  -> {status}")


if __name__ == "__main__":
    random.seed(20260731)

    print("Edge cases:")
    edge_cases()
    print()

    print("Exhaustive small (N<=5, C<=5):")
    exhaustive(5, 5)
    print()

    print("Exhaustive (N<=7, C<=4):")
    exhaustive(7, 4)
    print()

    print("Random within hypothesis:")
    random_within_hypothesis(5_000, max_N=64, max_C=64)
    print()

    print("Extra-large single instances:")
    random_extra_large(10, N=10_000, C=200)
    print()

    print("Distribution independence:")
    independence_test(1_000, max_N=20, max_C=50, pert_steps=500)
    print()

    print("Outside hypothesis (informational):")
    random_violating(5_000, max_N=32, max_C=32, factor=3)
    print()

    print("All verification complete.")
"""
Conformance suite for `traversal_sources.py` -- and an attack on
`docs/Traversal_Sources_Specification.md`.

The three layers are tested separately, because a failure in a composition run
that could have come from the permutation, the preparation, the readiness
contract or the seam tells you nothing.

What this suite is for
----------------------
Not to show that the model runs. To try to break the *decisions* the
specification made, and to check that the checks have teeth:

  * section 4.3's `a`-then-`b` ordering, with rejected draws **consumed**, is
    checked against `sim/vectors/observer_prng.txt` -- a frozen artefact -- rather
    than against this model;
  * section 4.2's `VAL_W + 1` width is checked with an `N > 2^(VAL_W - 1)` case,
    because a small-`N` testbench cannot see the truncation;
  * section 7.3's repeat-last-complete-bank rule is attacked with partial banks;
  * section 2's no-withholding rule is checked by requiring `bind_pass` to
    *refuse*, rather than to wait.

Usage:
    python3 test_traversal_sources.py
"""

import os
import sys
from math import gcd

from observers import SplitMix32
from traversal_sources import (
    AffineSource, IdentitySource, ShuffledSource, LEAD, VAL_W,
    affine_incremental, affine_incremental_narrow, affine_pure, affine_sequence,
    bank_and_cost, bind_pass, derive_ab, engine_signature, identity_sequence,
    identity_source, incremental_survives_abort, pass_through_engine,
    required_rate, shuffle_from, uniform_with_stream,
)

VECTOR_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                          "..", "sim", "vectors")

FAILURES = []
CHECKS = 0


def check(ok, what):
    global CHECKS
    CHECKS += 1
    if not ok:
        FAILURES.append(what)
    return ok


def suite(text):
    print(text)


# ---------------------------------------------------------------------------
# Layer 1 -- pure source functions
# ---------------------------------------------------------------------------

def test_bijections():
    suite("Layer 1 -- every source is a bijection, for many N")
    bad = []
    Ns = list(range(1, 40)) + [64, 127, 128, 255, 256, 1000]
    for N in Ns:
        if sorted(identity_sequence(N)) != list(range(N)):
            bad.append(("identity", N))
        a, b, _ = derive_ab(N, 0xC0FFEE)
        if sorted(affine_sequence(a, b, N)) != list(range(N)):
            bad.append(("closed", N))
        if sorted(affine_incremental(a, b, N)) != list(range(N)):
            bad.append(("incremental", N))
    check(not bad, f"every source is a bijection (bad: {bad[:4]})")
    print(f"  {3 * len(Ns)} source/N combinations are bijections, "
          f"no counterexample")


def test_coprime_composite_N():
    suite("Layer 1 -- affine on N with many small prime factors")
    worst = 1.0
    for N in (4, 6, 10, 12, 30, 60, 210, 2310, 4620):
        a, b, _ = derive_ab(N, 0x9E3779B9)
        check(gcd(a, N) == 1, f"a is coprime for N={N}")
        check(sorted(affine_sequence(a, b, N)) == list(range(N)),
              f"a permutation for N={N}")
        phi = sum(1 for k in range(N) if gcd(k, N) == 1)
        worst = min(worst, phi / N)
    print(f"  every pass a permutation at N up to 4620; worst phi(N)/N = {worst:.3f}")
    print("  (2310 = 2*3*5*7*11 makes 'a must be odd' fail, which is the point)")


def test_width_trap():
    suite("Layer 1 -- the VAL_W + 1 width, and the truncation it exposes")
    # A fast demonstration at a reduced width: the sum exceeds 2^val_w.
    ok8 = affine_incremental(7, 3, 300, val_w=8)
    bad8 = affine_incremental_narrow(7, 3, 300, val_w=8)
    check(ok8 == affine_sequence(7, 3, 300), "wide form is right at val_w=8")
    check(bad8 != ok8, "the narrow form is wrong at N > 2^(val_w-1)")

    # The real case, at the model's VAL_W. Whether a pass ever needs 17 bits
    # depends on the (a, b) the seed yields -- `r + a >= 2^VAL_W` is a property
    # of the walk, not of N -- so the case is *searched* rather than assumed.
    hit = None
    for N in (40000, 65535, 65537, 100000):
        for seed in (0x1234, 1, 2, 3):
            a, b, _ = derive_ab(N, seed)
            w = affine_incremental(a, b, N)
            nr = affine_incremental_narrow(a, b, N)
            if w != nr:
                hit = (N, seed, a, b, w, nr)
                break
        if hit:
            break
    check(hit is not None, "a pass whose 16-bit sum overflows was found")
    if hit is None:
        print("  no configuration overflowed; the trap was NOT exercised")
        return
    N, seed, a, b, wide, narrow = hit
    check(wide == affine_sequence(a, b, N), "the wide form is the closed form")
    check(sorted(wide) == list(range(N)), "the wide form is still a bijection")
    first = next(t for t in range(N) if wide[t] != narrow[t])
    check(first > 0, "the divergence begins at a definite step, not a scramble")
    # A small-N testbench is blind, because the sum cannot reach 2^VAL_W while
    # N <= 2^(VAL_W - 1). The step must be derived for *that* N: reusing a step
    # from the large N is not the same experiment, and the model now refuses it.
    a64, b64, _ = derive_ab(64, seed)
    small_sees = (affine_incremental(a64, b64, 64)
                  == affine_incremental_narrow(a64, b64, 64))
    check(small_sees, "a small-N testbench sees nothing (the trap is real)")
    try:
        affine_incremental(a, b, 64)
        check(False, "the model accepted a step that is not < N")
    except ValueError:
        check(True, "the model refuses a step not < N, so the precondition holds")
    print(f"  val_w=8, N=300: truncation caught at once")
    print(f"  val_w={VAL_W}, N={N}, seed={seed:#x}, a={a}: needs 17 bits, "
          f"caught at step {first}")
    print("  and it is correct for every N <= 2^(VAL_W - 1), so small N is blind")


# ---------------------------------------------------------------------------
# Layer 2 -- stateful preparation
# ---------------------------------------------------------------------------

def load_prng_rows():
    """
    The pinned generator's own vectors: `G <seed> <z0> .. <z15>`.

    Reading these is what makes the rejection-stream test a check against a
    frozen artefact instead of a check against this model.
    """
    rows = {}
    path = os.path.join(VECTOR_DIR, "observer_prng.txt")
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            tok = line.split()
            if tok and tok[0] == "G":
                rows[int(tok[1], 16)] = [int(x, 16) for x in tok[2:]]
    return rows


def test_rejection_stream_against_prng():
    suite("Layer 2 -- section 4.3's stream, checked against observer_prng.txt")
    rows = load_prng_rows()
    check(len(rows) >= 8, f"the pinned corpus has rows ({len(rows)})")

    compared = 0
    for seed, z in rows.items():
        for N in (2, 3, 5, 7, 8, 12, 30):
            _, _, draws = derive_ab(N, seed)
            if len(draws) > len(z):
                continue
            check(draws == z[:len(draws)],
                  f"draw stream for N={N} seed={seed:#x} matches the corpus")
            compared += 1
    check(compared >= 40, f"enough (seed, N) pairs were comparable ({compared})")
    print(f"  {compared} (seed, N) pairs: the consumed draws are a prefix of the")
    print("  pinned stream, so rejections are consumed and b follows a")


def test_draw_order_matters():
    suite("Layer 2 -- drawing b first is wrong, and O1 would never say so")
    swaps = 0
    for seed in (1, 0x2A, 0x9E3779B9):
        for N in (7, 12, 30):
            a1, b1, _ = derive_ab(N, seed)
            rng = SplitMix32(seed)
            b_early, _ = uniform_with_stream(rng, N - 1)
            while True:
                a_late, _ = uniform_with_stream(rng, N - 1)
                if gcd(a_late, N) == 1:
                    break
            still_a_bijection = (sorted(affine_sequence(a_late, b_early, N))
                                 == list(range(N)))
            agrees = (a_late, b_early) == (a1, b1)
            check(still_a_bijection and not agrees,
                  f"swapped order is a bijection yet differs, seed={seed:#x} N={N}")
            swaps += 1
    print(f"  {swaps} swapped-order cases: every one still a bijection, every one")
    print("  a different permutation -- only the draw stream catches this")


def test_fill_and_lead():
    suite("Layer 2 -- the fill, the lead, and readiness")
    for N in (4, 8, 16, 32):
        s = ShuffledSource(N, 1)
        check(not s.ready(), f"N={N}: not ready before any bank exists")
        ticks = 0
        while not s.ready() and ticks < 4000:
            s.tick()
            ticks += 1
        check(s.ready(), f"N={N}: ready once {LEAD} banks exist")
        check(ticks >= LEAD * (N - 1),
              f"N={N}: readiness took at least {LEAD} fills ({ticks})")
    print(f"  lead = {LEAD} banks, and readiness is never granted before them")


def test_startup_readiness_is_latched():
    suite("Layer 2 -- readiness is a latched start-up condition, not a depth test")
    s = ShuffledSource(8, 1)
    check(not s.ready(), "not ready before any bank has been built")
    ticks = 0
    while s.built < LEAD and ticks < 4000:
        s.tick()
        ticks += 1
    check(s.ready(), "ready once LEAD banks have been built")
    check(len(s.banks) == LEAD, "exactly LEAD banks are queued at that moment")
    s.start_pass()
    check(len(s.banks) == LEAD - 1, "starting a pass consumes a queued bank")
    check(s.ready(), "and readiness survives it -- the queue is short, not empty")
    s.tick()          # let the fill add one
    check(s.ready(), "still ready")
    # the composition trap this fixes: bind_pass refuses a source that is not
    # ready, so a depth-based ready() made a *second* pass unboundable.
    from traversal_sources import bind_pass
    s2 = ShuffledSource(8, 3)
    while not s2.ready():
        s2.tick()
    _ = bind_pass(s2)
    _ = bind_pass(s2)          # would raise if ready had gone false
    check(True, "a second pass binds to a shuffled source (it did not raise)")
    # and an unservable N is still refused, because no time fixes it
    big = ShuffledSource(4096, 1)
    for _ in range(50):
        big.tick()
    check(not big.ready(), "an unservable N stays unready however long you wait")
    print(f"  LEAD = {LEAD} banks fund the buffer, and readiness latches; a")
    print("  short queue afterwards is a repeat, not a refusal")


def test_underrun_repeats_a_complete_bank():
    suite("Layer 2 -- underrun repeats a complete bank, so O1 holds")
    s = ShuffledSource(8, 5)
    while not s.ready():
        s.tick()
    passes = 0
    underruns = 0
    for _ in range(40):
        s.underrun_flag = False
        s.start_pass()
        passes += 1
        if s.underrun_flag:
            underruns += 1
        check(sorted(s.walk_bank()) == list(range(8)),
              f"pass {passes} is a permutation even under underrun")
    check(underruns > 0, "the run did underrun, so the case was exercised")
    print(f"  {passes} passes, {underruns} of them underrun, every one a bijection")


def partial_inplace_bank(N, seed, steps):
    """The bank read while an **in-place** shuffle is part-written."""
    rng = SplitMix32(seed)
    p = list(range(N))
    for i in range(N - 1, 0, -1):
        if steps <= 0:
            break
        j = rng.uniform(i)
        p[i], p[j] = p[j], p[i]
        steps -= 1
    return p


def partial_result_bank(N, seed, steps):
    """
    The bank read while a shuffle is written into it as a *result*: entries not
    yet computed still hold their reset value.
    """
    rng = SplitMix32(seed)
    src = list(range(N))
    for i in range(N - 1, 0, -1):
        if steps <= 0:
            break
        j = rng.uniform(i)
        src[i], src[j] = src[j], src[i]
        steps -= 1
    p = [0] * N
    p[N - steps:] = src[N - steps:]
    return p


def test_partial_banks():
    suite("Layer 2 -- is a partial bank caught by O1? It depends, and that matters")
    N = 8
    inplace_always_bijective = True
    for steps in range(0, N):
        if sorted(partial_inplace_bank(N, 0x5EED, steps)) != list(range(N)):
            inplace_always_bijective = False
    check(inplace_always_bijective,
          "an in-place partial bank is still a bijection at every prefix")

    result_caught = False
    for steps in range(1, N):
        if sorted(partial_result_bank(N, 0x5EED, steps)) != list(range(N)):
            result_caught = True
    check(result_caught,
          "a result-written partial bank is not a bijection, so O1 catches it")

    print("  FINDING: a partial bank is a bijection iff the shuffle is in place.")
    print("  Swaps preserve the multiset, so a half-advanced in-place bank is a")
    print("  perfectly good permutation and O1 cannot see it. Only a bank written")
    print("  as a result (unwritten slots stale) violates O1. Section 7.3's")
    print("  rationale therefore holds only for the result-written construction,")
    print("  and must name which one the RTL uses.")


def test_readiness_is_a_contract():
    suite("Layer 2 -- readiness is external, and ready is required, not awaited")
    s = IdentitySource(4)
    check(s.ready(), "identity is ready immediately: a constant 1 (section 7.1)")
    check(bind_pass(s)(3) == 3, "and bind_pass binds it: pi(3) = 3")
    a = AffineSource(7, 0x1234)
    check(not a.ready(), "the affine is NOT ready immediately: it derives")
    try:
        bind_pass(a)
        check(False, "bind_pass accepted a source that is not ready")
    except ValueError:
        check(True, "bind_pass refuses a source that is not ready")
    for N in (5, 11):
        a = AffineSource(N, 0x77)
        check(not a.ready(), f"affine N={N} is not instantly ready")
        check(len(a.draws) > 0, f"affine N={N} consumed draws to derive (a, b)")
        while not a.ready():
            a.tick()
        check(a.ready(), f"affine N={N} becomes ready")
    print("  the identity is never late; the affine waits for its derivation")
    print("  and a pass is refused, never stalled (section 2)")


# ---------------------------------------------------------------------------
# Layer 3 -- composition
# ---------------------------------------------------------------------------

def test_identity_wiring_is_v2a():
    suite("Layer 3 -- wiring the seam to identity reproduces v2.0a exactly")
    for N in (1, 2, 3, 5, 8, 16, 33):
        s = IdentitySource(N)
        s.tick()                      # the one-cycle recovery of section 7.2
        want = engine_signature(N, identity_source)
        got = engine_signature(N, bind_pass(s))
        check(want == got, f"N={N}: identity through the seam == v2.0a's own trace")
    print("  same trace, cycle for cycle, at N up to 33 -- the seam is a swap,")
    print("  and the v2.0a suites re-run as the regression that proves it")


def test_ordering_changes_nothing_else():
    suite("Layer 3 -- a different source changes the order and nothing else")
    for N in (4, 7, 12, 30):
        a, b, _ = derive_ab(N, 0xABCD)
        aff = pass_through_engine(N, affine_pure(a, b, N))
        ident = pass_through_engine(N, identity_source)
        check(sorted(aff) == sorted(ident),
              f"N={N}: same multiset as the identity (O1 and P2 preserved)")
        check(sorted(aff) == list(range(N)), f"N={N}: a bijection through the engine")
        check(all(0 <= c < N for c in aff), f"N={N}: every column is in range")
        if a % N != 1 or b % N != 0:
            check(aff != ident, f"N={N}: a genuinely different order")
    print("  same multiset, same range, different order -- at N up to 30")


def test_composition_invariant_reused():
    suite("Layer 3 -- the composition rule, reused from the peripheral model")
    from observer_periph import ObserverPeriph, OBS_CTRL, CTRL_START, CTRL_STEP
    from observer_periph import replay
    p = ObserverPeriph(N=2, valid=True)
    p.write(OBS_CTRL, CTRL_START)
    p.tick()
    p.write(OBS_CTRL, CTRL_STEP)
    p.tick()
    check(replay(p.in_trace) == p.out_trace,
          "a bare engine reproduces the peripheral's engine trace exactly")
    print("  the invariant v2.0a established is unchanged by v2.0c -- the source")
    print("  sits behind the engine, so the engine still composes")


def test_rate_is_a_source_property():
    suite("Layer 3 -- the rate bound belongs to the source, not the engine")
    check(required_rate(8, 1) == 1, "N=8 is servable at one visit per clock")
    for N in (64, 128, 256):
        check(required_rate(N, 1) >= 2,
              f"N={N}: a shuffled source cannot run at one visit per clock")
    for N in (64, 128):
        a, b, _ = derive_ab(N, 3)
        check(len(affine_incremental(a, b, N)) == N,
              f"N={N}: the affine source answers with no lead at all")
    print("  identity and affine: no lead. Shuffled: >= 2 clocks per visit.")
    print("  That is section 5.4's sustained condition, and it is a property of")
    print("  the source -- the engine and output stage are untouched by it")


# ---------------------------------------------------------------------------
# The mutation battery: does the suite have teeth?
# ---------------------------------------------------------------------------

def test_mutants_are_caught():
    suite("Mutants -- each must be caught, and the correct source must not be")
    caught = []

    # M1: the VAL_W + 1 sum truncated to VAL_W. (40000, seed 1) is a searched
    # configuration: its `a` makes the sum exceed 2^VAL_W mid-pass.
    N = 40000
    a, b, _ = derive_ab(N, 1)
    if affine_incremental_narrow(a, b, N) != affine_sequence(a, b, N):
        caught.append("M1 narrow width (VAL_W+1 -> VAL_W)")

    # M2: b drawn before a, from the same stream.
    a1, b1, _ = derive_ab(12, 0x2A)
    rng = SplitMix32(0x2A)
    b_early, _ = uniform_with_stream(rng, 11)
    while True:
        a_late, _ = uniform_with_stream(rng, 11)
        if gcd(a_late, 12) == 1:
            break
    if (a_late, b_early) != (a1, b1):
        caught.append("M2 b drawn before a")

    # M3: a not coprime to N.
    bad_a = 4
    if sorted(affine_sequence(bad_a, 0, 12)) != list(range(12)):
        caught.append("M3 a not coprime to N")

    # M4: a result-written partial bank.
    if sorted(partial_result_bank(8, 0x5EED, 4)) != list(range(8)):
        caught.append("M4 partial result-written bank")

    # M5: an in-place partial bank. NOT caught by O1 -- listed so the gap is
    # visible rather than implied.
    inplace_gap = (sorted(partial_inplace_bank(8, 0x5EED, 4)) == list(range(8)))

    check(len(caught) == 4, f"four mutants are observable ({caught})")
    check(inplace_gap,
          "and one is not: an in-place partial bank is invisible to O1")
    for m in caught:
        print(f"  caught: {m}")
    print("  NOT caught by O1: an in-place partial bank (see the section 7.3 note)")

    # Controls: the correct implementations must pass the same checks.
    a, b, _ = derive_ab(64, 7)
    check(affine_incremental(a, b, 64) == affine_sequence(a, b, 64),
          "control: the correct incremental form passes")
    s = ShuffledSource(16, 1)
    while not s.ready():
        s.tick()
    s.start_pass()
    check(sorted(s.walk_bank()) == list(range(16)),
          "control: the correct bank passes")
    check(incremental_survives_abort(a, b, 64),
          "control: the correct form survives an abort")
    print("  three controls pass, so the checks are not simply rejecting things")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    tests = [
        test_bijections,
        test_coprime_composite_N,
        test_width_trap,
        test_rejection_stream_against_prng,
        test_draw_order_matters,
        test_fill_and_lead,
        test_startup_readiness_is_latched,
        test_underrun_repeats_a_complete_bank,
        test_partial_banks,
        test_readiness_is_a_contract,
        test_identity_wiring_is_v2a,
        test_ordering_changes_nothing_else,
        test_composition_invariant_reused,
        test_rate_is_a_source_property,
        test_mutants_are_caught,
    ]
    for t in tests:
        t()

    print()
    if FAILURES:
        for f in FAILURES[:10]:
            print(f"  FAIL {f}")
        print(f"test_traversal_sources: FAIL  "
              f"{len(FAILURES)} of {CHECKS} checks failed")
        return 1
    print(f"test_traversal_sources: PASS  {CHECKS} checks, three layers, "
          f"{len(tests)} suites")
    return 0


if __name__ == "__main__":
    sys.exit(main())

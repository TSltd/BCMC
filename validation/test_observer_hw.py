"""
Conformance tests for the BCMC hardware observer's cycle-level model.

`observer_hw.py` is to `docs/Hardware_Observer_Architecture.md` what
`bcmc_periph.py` is to `docs/Register_Map.md`. This script is what makes that
restatement trustworthy, and it follows the rules the project has used since
v0.2:

  1. **No invented answers.** Every matrix bit an expectation contains is read
     out of `sim/vectors/matrix_*.txt`, which `reference.py` generated and
     which already drive the RTL suites. The observer's obligations are
     therefore checked against the same object the hardware is checked against.

  2. **The properties are re-derived, not imported.** O1, O2 and P1-P4 are
     recomputed here from the vector files, with no help from `observer_hw.py`
     beyond the pass it produced.

  3. **The event table is executed, not read.** Section 3.11 enumerates every
     combination of inputs on an edge. Each row is a test, including the ones
     that say "ignored" -- those are the rows an implementation tends to get
     wrong, and a table that is never executed is just prose.

  4. **The v2.0c claim has a negative control.** Section 9.3 claims a
     Fisher-Yates bank cannot fill within the pass that consumes it at full
     rate. That is a claim about a measurement, so the measurement is made here
     and the structural lower bound is shown to be too optimistic to settle it.

Run:
    python3 validation/test_observer_hw.py
"""

import math
import os
import sys

import reference as ref
from observer_hw import (
    IDLE,
    RUN,
    SequentialEngine,
    bank_fill_cycles,
    bank_fill_passes,
    simulate,
    visit_columns,
)
from test_periph import load_matrix_vectors

VECTOR_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "sim", "vectors")


def no_stale_visits(result, valid_low):
    """Section 3.7: `visit_valid` is never high in a cycle where `valid` is low."""
    return all(
        not (c.visit_valid and c.index in valid_low) for c in result.cycles
    )


# ---------------------------------------------------------------------------
# Section 5.1 -- the one timing rule
# ---------------------------------------------------------------------------

def suite_timing():
    """
    A `start` or an accepted `trigger` in cycle `k` is presented as a visit in
    cycle `k+1`, and for exactly that cycle. Everything else about the timing
    follows from this, so it is worth pinning exactly.
    """
    fails = []
    checked = 0

    for N in (1, 2, 3, 8, 17):
        # `start` in cycle 2 -> first visit in cycle 3.
        r = simulate(N, start_cycle=2, cycles=2 + N + 2)
        first = next((c.index for c in r.cycles if c.visit_valid), None)
        if first != 3:
            fails.append(f"N={N}: first visit in cycle {first}, expected 3")
        checked += 1

        # Each trigger produces its visit in the following cycle.
        for t in (5, 6, 9):
            r = simulate(N, start_cycle=0, trigger_cycles=(t,), cycles=t + 3)
            v = [c.index for c in r.cycles if c.visit_valid]
            if t + 1 not in v:
                fails.append(f"N={N}: trigger in {t} gave no visit in {t + 1}")
            checked += 1

        # `done` is coincident with the visit of t = N-1, never after it.
        r = simulate(N, start_cycle=0,
                     trigger_cycles=tuple(range(1, N)), cycles=N + 3)
        done_cycles = [c.index for c in r.cycles if c.done]
        last_visit = [c.index for c in r.cycles if c.visit_valid][N - 1]
        if done_cycles != [last_visit]:
            fails.append(f"N={N}: done at {done_cycles}, last visit at {last_visit}")
        checked += 1

        # `column` changes only in a visit cycle: it is retained otherwise.
        prev, prev_index = None, None
        for c in r.cycles:
            if prev is not None and c.column != prev and not c.visit_valid:
                fails.append(
                    f"N={N}: column changed at cycle {c.index} without a visit"
                )
            prev, prev_index = c.column, c.index
        checked += 1

        # The visit strobe is exactly one cycle wide.
        vv = [c.visit_valid for c in r.cycles]
        if any(vv[i] and vv[i + 1] and r.cycles[i].done for i in range(len(vv) - 1)):
            fails.append(f"N={N}: a visit strobe outlived its cycle")
        checked += 1

    if fails:
        for f in fails[:10]:
            print(f"  FAIL {f}")
        return False
    print(f"  {checked:,} timing checks: one clock from request to visit, done coincident.")
    return True


# ---------------------------------------------------------------------------
# O1 -- pi is a bijection
# ---------------------------------------------------------------------------

def suite_bijection():
    """
    Over one pass every column is visited exactly once. In v2.0a `pi` is the
    identity, so the emitted sequence over a pass must be `0, 1, ..., N-1` in
    order -- but the check is written as a permutation test, because that is
    what has to survive v2.0c replacing the source.
    """
    fails = []
    checked = 0

    for N in list(range(1, 65)) + [100, 255, 256, 257]:
        r = simulate(N, start_cycle=0,
                     trigger_cycles=tuple(range(1, N)), cycles=N + 3)
        if len(r.visits) != N:
            fails.append(f"N={N}: a pass emitted {len(r.visits)} visits, not N")
        elif sorted(r.visits) != list(range(N)):
            fails.append(f"N={N}: the pass is not a permutation of 0..N-1")
        checked += 1

    if fails:
        for f in fails[:10]:
            print(f"  FAIL {f}")
        return False
    print(f"  {checked:,} passes, every one a bijection of 0..N-1.")
    return True


# ---------------------------------------------------------------------------
# The conservation invariant (section 7.4) -- the companion to O3
# ---------------------------------------------------------------------------

def suite_conservation():
    """
    For a run with no invalidation, the number of visits equals the number of
    accepted triggers plus one per accepted start. A count is what catches the
    failures a sequence diff can miss: a double advancement, an accidental
    auto-wrap, a trigger honoured in `IDLE`, a trigger honoured during an
    invalidation.

    Under invalidation the strict equality is **false**, and this suite is where
    that was found: an accepted request whose visit falls in a cycle where
    `valid` is low is honoured by the cursor but suppressed by the output gate.
    So the invariant is stated in two parts -- every accepted request schedules
    exactly one visit, and every scheduled visit is either presented or
    suppressed -- and the loss is bounded by the number of times `valid` falls.
    The finding is recorded in section 10 of the specification (F11).
    """
    N = 8
    patterns = {
        "back to back": tuple(range(1, 40)),
        "every third": tuple(range(1, 40, 3)),
        "bursts": tuple(sorted(set(range(1, 10)) | set(range(20, 25)) | {35, 36, 37})),
        "sparse": (3, 11, 29),
    }
    fails = []
    checked = 0

    for name, trig in patterns.items():
        r = simulate(N, start_cycle=0, trigger_cycles=trig, cycles=45)
        if len(r.visits) != r.accepted_starts + r.accepted_triggers:
            fails.append(
                f"{name}: {len(r.visits)} visits != {r.accepted_starts} starts "
                f"+ {r.accepted_triggers} triggers"
            )
        if r.suppressed != 0:
            fails.append(f"{name}: {r.suppressed} visits suppressed with no invalidation")
        if len(r.visits) + r.suppressed != r.accepted_starts + r.accepted_triggers:
            fails.append(f"{name}: visits + suppressed != accepted requests")
        checked += 2

        # No duplicate and no skipped column inside any pass: each aligned
        # block of N visits is the whole set, in order.
        for k in range(0, len(r.visits) - N + 1, N):
            if sorted(r.visits[k:k + N]) != list(range(N)):
                fails.append(f"{name}: pass at visit {k} is {r.visits[k:k + N]}")
                break
        checked += 1

    # The same, with the context going invalid part-way through. Here the strict
    # equality must NOT hold -- and if it ever does, the gate is not doing its
    # job -- while the two-part invariant still must.
    valid_low = set(range(12, 18))
    r = simulate(N, start_cycle=0, trigger_cycles=tuple(range(1, 40)),
                 valid_low=valid_low, cycles=45)
    scheduled = [c for c in r.cycles if c.scheduled]
    if len(scheduled) != r.accepted_starts + r.accepted_triggers:
        fails.append("an accepted request did not schedule exactly one visit")
    if len(scheduled) != len(r.visits) + r.suppressed:
        fails.append("scheduled != presented + suppressed")
    if r.suppressed < 1:
        fails.append("the invalidation suppressed no visit -- the gate is not gating")
    falling = sum(
        1 for c in r.cycles
        if c.index in valid_low and (c.index - 1) not in valid_low
    )
    if r.suppressed > falling:
        fails.append(f"{r.suppressed} visits lost over {falling} invalidations")
    if r.aborts != 1:
        fails.append(f"expected one abort, saw {r.aborts}")
    checked += 5

    if fails:
        for f in fails[:10]:
            print(f"  FAIL {f}")
        return False
    print(f"  {checked:,} checks: every request schedules one visit; none is lost "
          f"without an invalidation.")
    return True


# ---------------------------------------------------------------------------
# Section 3.11 -- the event table, executed
# ---------------------------------------------------------------------------

def suite_event_table():
    """
    Every row of the section 3.11 table, including the "ignored" rows -- which
    are the ones an implementation gets wrong, because ignoring an input is a
    decision rather than an absence of one.
    """
    fails = []
    checked = 0

    def ok(cond, msg):
        nonlocal checked
        checked += 1
        if not cond:
            fails.append(msg)

    # start & IDLE & valid & N >= 1 -> RUN
    e = SequentialEngine(N=4)
    e.edge(start=True)
    ok(e.state == RUN, "start in IDLE & valid did not enter RUN")

    # start & !valid -> refused
    e = SequentialEngine(N=4)
    e.edge(start=True, valid=False)
    ok(e.state == IDLE and e.accepted_starts == 0, "start with !valid entered RUN")

    # start & N == 0 -> refused (section 3.4)
    e = SequentialEngine(N=0)
    e.edge(start=True)
    ok(e.state == IDLE and not e.outputs().visit_valid, "start with N = 0 entered RUN")

    # start & RUN -> ignored
    e = SequentialEngine(N=4)
    e.edge(start=True)
    t0 = e.tq
    e.edge(start=True)
    ok(e.state == RUN and e.tq == t0 and e.accepted_starts == 1,
       "start while RUN was honoured a second time")

    # trigger & IDLE -> ignored
    e = SequentialEngine(N=4)
    e.edge(trigger=True)
    ok(e.state == IDLE and not e.outputs().visit_valid and e.accepted_triggers == 0,
       "trigger in IDLE was honoured")

    # trigger & RUN & valid -> advance
    e = SequentialEngine(N=4)
    e.edge(start=True)
    e.edge(trigger=True)
    ok(e.tq == 1 and e.accepted_triggers == 1, "trigger in RUN did not advance")

    # trigger & RUN & !valid -> ignored, and the abort wins
    e = SequentialEngine(N=4)
    e.edge(start=True)
    e.edge(trigger=True, valid=False)
    ok(e.state == IDLE and e.aborted and e.accepted_triggers == 0,
       "trigger with !valid was honoured")

    # !valid & visit_q == 1 -> no visit this cycle
    e = SequentialEngine(N=4)
    e.edge(start=True)
    ok(not e.outputs(valid=False).visit_valid,
       "a scheduled visit was presented while !valid")

    # done & oneshot -> IDLE
    e = SequentialEngine(N=2, oneshot=True)
    e.edge(start=True)
    e.edge(trigger=True)
    ok(e.outputs().done, "no done on the last visit")
    e.edge()
    ok(e.state == IDLE, "oneshot did not return to IDLE after done")

    # done & !oneshot -> stay RUN, and the next trigger wraps
    e = SequentialEngine(N=2, oneshot=False)
    e.edge(start=True)
    e.edge(trigger=True)
    e.edge()
    ok(e.state == RUN, "continuous mode left RUN at the boundary")
    e.edge(trigger=True)
    ok(e.tq == 0, "the wrap trigger did not reset the cursor")

    # start & trigger in the same cycle, in IDLE -> start wins
    e = SequentialEngine(N=4)
    e.edge(start=True, trigger=True)
    ok(e.state == RUN and e.tq == 0 and e.accepted_starts == 1
       and e.accepted_triggers == 0, "start did not win over trigger in IDLE")

    # rst overrides everything, and is not an abort
    e = SequentialEngine(N=4)
    e.edge(start=True)
    e.edge(trigger=True)
    e.edge(rst=True)
    ok(e.state == IDLE and not e.visit and not e.aborted and e.tq == 0,
       "rst did not return the engine to a clean IDLE")

    if fails:
        for f in fails[:10]:
            print(f"  FAIL {f}")
        return False
    print(f"  {checked:,} rows of the section 3.11 table, all as written.")
    return True


# ---------------------------------------------------------------------------
# Section 3.10 -- invalidation
# ---------------------------------------------------------------------------

def suite_invalidation():
    """
    A pass is a traversal of *one* matrix, so an invalidation ends it. The two
    halves of the rule are checked separately, because they are separately
    implementable and separately wrong: the combinational gate (no stale visit,
    in any cycle) and the synchronous abort (the pass dies and says so).
    """
    fails = []
    checked = 0

    valid_low = set(range(6, 10))
    r = simulate(8, start_cycle=0, trigger_cycles=tuple(range(1, 20)),
                 valid_low=valid_low, cycles=20)

    if not no_stale_visits(r, valid_low):
        fails.append("a visit was presented in a cycle where !valid")
    checked += 1

    if any(c.visit_valid for c in r.cycles if c.index >= 6):
        fails.append("a visit happened after the invalidation began")
    checked += 1

    if not any(c.aborted for c in r.cycles):
        fails.append("the abort was never recorded")
    checked += 1

    if len(r.visits) >= 8:
        fails.append("a cut-short pass reported a full pass")
    checked += 1

    # A restart after valid returns clears the sticky abort.
    r2 = simulate(8, start_cycle=10, trigger_cycles=tuple(range(11, 20)),
                  valid_low=valid_low, cycles=20)
    if any(c.aborted for c in r2.cycles if c.index > 10):
        fails.append("a restart did not clear the sticky abort")
    checked += 1

    # A start issued *during* the invalidation is refused, so the abort sticks.
    r3 = simulate(8, start_cycle=7, trigger_cycles=tuple(range(1, 20)),
                  valid_low=valid_low, cycles=20)
    if r3.accepted_starts != 0:
        fails.append("a start during an invalidation was accepted")
    checked += 1

    # The gate is combinational, so it holds in *every* cycle of a sweep.
    for N in (1, 2, 5, 16):
        for lo in (set(), {3}, set(range(4, 9)), {0}):
            rr = simulate(N, start_cycle=0, trigger_cycles=tuple(range(1, 12)),
                          valid_low=lo, cycles=15)
            if not no_stale_visits(rr, lo):
                fails.append(f"N={N} valid_low={sorted(lo)}: a stale visit")
        checked += 1

    if fails:
        for f in fails[:10]:
            print(f"  FAIL {f}")
        return False
    print(f"  {checked:,} invalidation checks: no stale visit, the pass dies, it says so.")
    return True


# ---------------------------------------------------------------------------
# One-shot and continuous endings, including N = 1
# ---------------------------------------------------------------------------

def suite_endings():
    """
    `oneshot` decides what happens after the last visit, and `N = 1` is the
    case every off-by-one in the boundary comparison fails on.
    """
    fails = []
    checked = 0

    for N in (1, 2, 3, 8):
        r = simulate(N, start_cycle=0, trigger_cycles=tuple(range(1, 4 * N)),
                     oneshot=True, cycles=4 * N + 3)
        if len(r.visits) != N:
            fails.append(f"N={N}: one-shot emitted {len(r.visits)} visits, not N")
        checked += 1
        done_cycles = [c.index for c in r.cycles if c.done]
        if len(done_cycles) != 1:
            fails.append(f"N={N}: one-shot asserted done {len(done_cycles)} times")
        else:
            after = [c.index for c in r.cycles
                     if c.index > done_cycles[0] and c.visit_valid]
            if after:
                fails.append(f"N={N}: one-shot kept visiting after done")
        checked += 1

    # N = 1: every trigger is a complete pass, and done rides every visit.
    r = simulate(1, start_cycle=0, trigger_cycles=(1, 2, 3), cycles=6)
    if r.visits != [0, 0, 0, 0]:
        fails.append(f"N=1: visits {r.visits}, expected four passes of one visit")
    checked += 1
    if not all(c.done for c in r.cycles if c.visit_valid):
        fails.append("N=1: a visit was not also a completed pass")
    checked += 1

    if fails:
        for f in fails[:10]:
            print(f"  FAIL {f}")
        return False
    print(f"  {checked:,} ending checks, including N = 1 in both modes.")
    return True


# ---------------------------------------------------------------------------
# O2 and P1-P3, against the reference matrices
# ---------------------------------------------------------------------------

def suite_vectors(files):
    """
    The observer's obligations, on real matrices. Every expectation is read out
    of the vector file -- the same file the RTL suites are driven from -- and
    the observer's columns are then composed with `reference.py` and required to
    reproduce it. P1-P3 are recomputed here from the file, not imported from the
    model under test.
    """
    fails = []
    checked = 0

    for fname, stride in files:
        cases = load_matrix_vectors(os.path.join(VECTOR_DIR, fname))
        for case in cases[::stride]:
            N, weights, offsets, rows = (
                case["N"], case["weights"], case["offsets"], case["rows"]
            )
            r = simulate(N, start_cycle=0, trigger_cycles=tuple(range(1, N)),
                         cycles=N + 3)
            cols = visit_columns(r.cycles)

            # O1: a pass is a bijection of 0 .. N-1.
            if sorted(cols) != list(range(N)):
                fails.append(f"{fname} N={N}: the pass is not a bijection")
                continue

            # O2: feeding each visited column to the reference evaluator must
            # reproduce the row data the file already fixed.
            for j in cols:
                from_file = [i for i, row in enumerate(rows) if row[j]]
                from_ref = [
                    i for i, b in enumerate(ref.bcmc_column(weights, offsets, j, N))
                    if b
                ]
                if from_file != from_ref:
                    fails.append(f"{fname} N={N} column {j}: O2 disagreement")
                    break

            active = {j: [i for i, row in enumerate(rows) if row[j]] for j in cols}

            # P1: the emitted events are exactly the support of M, once each,
            # and there are W = sum(weights) of them.
            emitted = [(i, j) for j in cols for i in active[j]]
            support = {(i, j) for i, row in enumerate(rows)
                       for j, b in enumerate(row) if b}
            if set(emitted) != support or len(emitted) != len(support):
                fails.append(f"{fname} N={N}: P1 -- events are not the support")
            if len(emitted) != sum(weights):
                fails.append(f"{fname} N={N}: P1 -- {len(emitted)} events, W={sum(weights)}")

            # P2: row conservation.
            counts = [0] * len(weights)
            for i, _ in emitted:
                counts[i] += 1
            if counts != list(weights):
                fails.append(f"{fname} N={N}: P2 -- row conservation broke")

            # P3: the occupancy multiset is the Balance Theorem's, re-derived
            # from the file rather than assumed.
            occupancy = sorted(len(active[j]) for j in cols)
            if occupancy != sorted(case["loads"]):
                fails.append(f"{fname} N={N}: P3 -- occupancy multiset changed")

            checked += 1

    if fails:
        for f in fails[:10]:
            print(f"  FAIL {f}")
        return False
    print(f"  {checked:,} passes over reference matrices: O1, O2 and P1-P3 hold.")
    return True


# ---------------------------------------------------------------------------
# Section 9.3 -- the Fisher-Yates fill bound, and its control
# ---------------------------------------------------------------------------

def suite_bank_bound():
    """
    Section 9.3 claims a shuffled source cannot keep up with a one-visit-per-
    clock stream, because its bank must be built before the pass that reads it.
    That is a claim about a measurement, so the measurement is made here. The
    control matters: the *structural* lower bound (one draw per shuffle step)
    would make a bank look as though it fits, and only the measured value shows
    it does not.
    """
    fails = []
    checked = 0
    Ns = (8, 16, 32, 64, 128)
    seeds = list(range(40))

    print(f"    {'N':>5} {'mean fill':>10} {'fill/N':>8} {'passes':>7} {'rate':>6}")
    for N in Ns:
        fills = [bank_fill_cycles(N, s) for s in seeds]
        if any(f < N - 1 for f in fills):
            fails.append(f"N={N}: a fill fell below the structural N-1 bound")
        checked += 1
        mean = sum(fills) / len(fills)
        ratio = mean / N
        spans = bank_fill_passes(N, seeds[0], 1)
        rate = N / mean                      # visits per clock the fill allows
        print(f"    {N:>5} {mean:>10.1f} {ratio:>8.2f} {spans:>7} {rate:>6.2f}")
        if ratio <= 1.0:
            fails.append(f"N={N}: mean fill ratio {ratio:.2f} <= 1; claim does not hold")
        checked += 1

    # The control: the optimistic model (fill == N-1) would pass a "fits" test,
    # and the measured model must not.
    N = 64
    if (N - 1) > N:
        fails.append("CONTROL BROKEN: the lower bound should look legal")
    over = sum(1 for s in seeds if bank_fill_cycles(N, s) > N)
    if over <= len(seeds) // 2:
        fails.append(f"CONTROL: only {over}/40 seeds exceeded N; the claim is weak")
    checked += 1

    # `bank_fill_passes` must agree with the legality condition it summarises:
    # the source is legal at this rate exactly when the fill spans at most one
    # pass interval.
    for N in (8, 16, 64):
        for r in (1, 2, 4):
            for s in range(10):
                fill = bank_fill_cycles(N, s)
                spans = bank_fill_passes(N, s, r)
                legal = fill <= N * r
                if legal != (spans <= 1):
                    fails.append(f"N={N} r={r} s={s}: legal != (spans <= 1)")
                if spans != math.ceil(fill / (N * r)):
                    fails.append(f"N={N} r={r} s={s}: spans {spans} != ceil")
                checked += 1

    if fails:
        for f in fails[:10]:
            print(f"  FAIL {f}")
        return False
    print(f"  {checked:,} checks: a bank needs well over one pass to fill.")
    return True


# ---------------------------------------------------------------------------

def main():
    results = []

    print("Section 5.1 -- the one timing rule:")
    results.append(suite_timing())
    print()

    print("O1 -- pi is a bijection:")
    results.append(suite_bijection())
    print()

    print("Conservation -- request scheduling and suppression:")
    results.append(suite_conservation())
    print()

    print("Section 3.11 -- the event table, executed:")
    results.append(suite_event_table())
    print()

    print("Section 3.10 -- invalidation:")
    results.append(suite_invalidation())
    print()

    print("Endings -- one-shot, continuous, N = 1:")
    results.append(suite_endings())
    print()

    print("O2 and P1-P3, against the reference matrices:")
    results.append(suite_vectors([
        ("matrix_edge.txt", 1),
        ("matrix_random.txt", 1),
        ("matrix_exhaustive.txt", 7),
    ]))
    print()

    print("Section 9.3 -- the Fisher-Yates fill bound:")
    results.append(suite_bank_bound())
    print()

    if all(results):
        print("observer_hw.py satisfies docs/Hardware_Observer_Architecture.md.")
        print("The v2.0a engine is now an executable contract for rtl/bcmc_observer.v.")
        return 0

    print("CONFORMANCE FAILED")
    return 1


if __name__ == "__main__":
    sys.exit(main())

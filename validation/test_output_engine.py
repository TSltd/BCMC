"""
Conformance and adversarial tests for the v2.0b output engine's cycle model.

`output_engine.py` is to `docs/Output_Engine.md` what `observer_hw.py` is to the
engine's specification and `observer_periph.py` is to the register map. This
script is what makes the restatement trustworthy, and it follows the same rules
the project has used since v0.2:

  1. **No invented answers.** Every matrix pattern an expectation contains comes
     from `reference.py`, through `output_engine.bits_of`, and the real contexts
     are loaded from `sim/vectors/matrix_*.txt` -- the same files the RTL suites
     are held to.

  2. **The obligations are section 10.2's, one predicate each.** The eleven items
     of that list are the ways the specification could be wrong, so each is
     written as a predicate over a trace rather than as a happy-path assertion.
     A predicate is a one-way invariant: it says what may *never* happen.

  3. **The destroy has its own stimulus.** Item 5 is the one a level-only gate
     fails, and it needs a sequence an ordinary pass never produces: invalidate,
     then revalidate *without a visit*. That sequence is driven explicitly, and
     the suite also asserts that the stimulus was actually exercised -- a test
     that cannot fail is not a test.

  4. **A mutation battery, with a "measures nothing" clause.** Section 10.6's
     mutants are planted through `Attached(stage_factory=...)`, and each must be
     caught by a *named* predicate. The no-destroy mutant must be caught by
     `p_no_dead_matrix` **and by nothing else**, because a mutation everything
     catches measures the suite's redundancy rather than its teeth.

Run:
    python3 validation/test_output_engine.py
"""

import inspect
import os
import sys

from output_engine import (
    REF_MAX_C,
    Attached,
    OutputStage,
    bits_of,
    low_mask,
    pin_changes,
    run_pass,
)
from test_periph import load_matrix_vectors

VECTOR_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                          "..", "sim", "vectors")


# ---------------------------------------------------------------------------
# Reading a trace
# ---------------------------------------------------------------------------

def valids(a):
    """`valid_i` per cycle, from the recorded engine inputs."""
    return [t[5] for t in a.in_trace]


def resets(a):
    """`rst` per cycle, from the recorded engine inputs."""
    return [t[2] for t in a.in_trace]


def visits(a):
    """`visit_valid` per cycle, from the engine's own outputs."""
    return [c.visit_valid for c in a.out_trace]


def gate_events(a):
    """Cycles where the gate closed: `valid` fell, so the pins went idle."""
    v = valids(a)
    return [k for k in range(1, len(v)) if not v[k] and v[k - 1]]


# ---------------------------------------------------------------------------
# The obligations of section 10.2, one predicate each
#
# A predicate returns the list of ways a trace breaks it, so the mutation battery
# can ask "which checks caught this mutant?" as well as "did any?". Every
# predicate is a one-way invariant: it states what may never happen, never what
# must, which is what keeps them true for an all-zero matrix.
# ---------------------------------------------------------------------------

def p_one_clock(a):
    """
    Item 1: the visit presented in cycle `k` is on the pins in cycle `k + 1`.

    Only this half is checkable, and an earlier draft's other half -- "the pins
    must not arrive at the visiting pattern in the visit's own cycle" -- is
    **unsound**: when two consecutive visits present the same pattern (`W = [N]`
    gives a row that is active in every column), the pins legitimately arrive at
    the current bits because the *previous* visit presented that same value, and
    no observation of the pins can separate that from a passthrough. The
    passthrough is caught by this half instead, whenever two consecutive patterns
    differ -- and when they never differ, a passthrough is genuinely
    unobservable on the pins, which is a fact about the specification rather than
    a gap in the suite.
    """
    fails = []
    b, p = a.bits_trace, a.pins_trace
    for k in range(len(p) - 1):
        if visits(a)[k] and valids(a)[k + 1] and p[k + 1] != b[k]:
            fails.append(f"cycle {k}: visit presented {b[k]:#x}, pins in "
                         f"{k + 1} are {p[k + 1]:#x}")
    return fails


def p_hold(a):
    """Item 2: no visit, no reset and no gap -> the pins do not move."""
    fails = []
    for k in range(1, len(a.pins_trace)):
        quiet = (not visits(a)[k - 1] and valids(a)[k] and valids(a)[k - 1]
                 and not resets(a)[k - 1])
        if quiet and a.pins_trace[k] != a.pins_trace[k - 1]:
            fails.append(f"cycle {k}: pins moved with no visit "
                         f"({a.pins_trace[k - 1]:#x} -> {a.pins_trace[k]:#x})")
    return fails


def p_gate(a):
    """Item 4: the gate is combinational -- idle in the very cycle valid is low."""
    fails = []
    for k in range(len(a.pins_trace)):
        if not valids(a)[k] and a.pins_trace[k] != 0:
            fails.append(f"cycle {k}: pins are {a.pins_trace[k]:#x} while valid "
                         f"is low")
    return fails


def p_no_dead_matrix(a):
    """
    Item 9, and the teeth of item 5. A non-idle pattern must be accounted for by
    the most recent visit, with no gap and no reset between that visit and now.
    """
    fails = []
    for k in range(1, len(a.pins_trace)):
        p = a.pins_trace[k]
        if p == 0:
            continue
        j = next((m for m in range(k - 1, -1, -1) if visits(a)[m]), None)
        if j is None:
            fails.append(f"cycle {k}: pins are {p:#x} with no visit behind them")
        elif a.bits_trace[j] != p:
            fails.append(f"cycle {k}: pins are {p:#x}, but the last visit "
                         f"presented {a.bits_trace[j]:#x}")
        elif not all(valids(a)[m] for m in range(j + 1, k)) or \
                any(resets(a)[m] for m in range(j, k)):
            fails.append(f"cycle {k}: pins are {p:#x} across a gap or a reset "
                         f"since the visit in {j}")
    return fails


def p_change_cause(a):
    """Item 7: a change only where a visit, a gate event or a reset allows it."""
    fails = []
    for k in range(1, len(a.pins_trace)):
        if a.pins_trace[k] == a.pins_trace[k - 1]:
            continue
        allowed = (visits(a)[k - 1] or resets(a)[k - 1]
                   or valids(a)[k] != valids(a)[k - 1])
        if not allowed:
            fails.append(f"cycle {k}: pins changed with no cause")
    return fails


def p_counting(a):
    """
    Item 8, corrected. The number of changes is *not* the number of visits: an
    earlier draft of the document said it was, and the counterexample is a
    context whose weights are all zero. What holds is the bound item 7 implies --
    every change needs a visit, a gate event or a reset.
    """
    changes = pin_changes(a.pins_trace)
    bound = sum(visits(a)) + len(gate_events(a)) + sum(resets(a))
    if changes > bound:
        return [f"{changes} changes but only {bound} possible causes"]
    return []


def p_reset_clears(a):
    """Item 6: a reset blanks the pins by the next cycle."""
    fails = []
    for k in range(len(a.pins_trace) - 1):
        if resets(a)[k] and a.pins_trace[k + 1] != 0:
            fails.append(f"cycle {k}: rst asserted, pins in {k + 1} are "
                         f"{a.pins_trace[k + 1]:#x}")
    return fails


def p_done_like_any_visit(a):
    """Item 3: `done` is coincident with a visit and adds no special latency."""
    fails = []
    for k, c in enumerate(a.out_trace):
        if not c.done:
            continue
        if not c.visit_valid:
            fails.append(f"cycle {k}: done without a visit")
        if k + 1 < len(a.pins_trace) and valids(a)[k + 1] and \
                a.pins_trace[k + 1] != a.bits_trace[k]:
            fails.append(f"cycle {k}: done's pattern did not arrive in {k + 1}")
    return fails


def p_lane_above_c(a):
    """Item 10: no lane at or above `C` is ever active, whatever `C` is."""
    bad = [k for k, x in enumerate(a.lane_above_c_seen()) if x]
    return [f"cycle {k}: a lane at or above C was active" for k in bad[:3]]


PREDICATES = (
    ("one_clock", p_one_clock),
    ("hold", p_hold),
    ("gate", p_gate),
    ("no_dead_matrix", p_no_dead_matrix),
    ("change_cause", p_change_cause),
    ("counting", p_counting),
    ("reset_clears", p_reset_clears),
    ("done_like_any_visit", p_done_like_any_visit),
    ("lane_above_c", p_lane_above_c),
)


def which_predicates_fail(a):
    """The names of every predicate this trace breaks."""
    return [name for name, fn in PREDICATES if fn(a)]


# ---------------------------------------------------------------------------
# The stimulus: a pass, a gap, a revalidation with no visit, then a reset
# ---------------------------------------------------------------------------

def stimulate(N=2, C=2, W=(1, 1), O=(0, 0), oneshot=False,
              stage_factory=OutputStage):
    """
    A trace that reaches every case the obligations care about -- including the
    one an ordinary pass cannot produce, which is item 5's: invalidate, then
    revalidate **without a visit**, and the old pattern must not come back.

        start, visit pi(0), hold             the ordinary pass, and item 2
        valid -> 0                           the gate closes with a NON-ZERO
                                             pattern held -- see below
        valid -> 1, no visit                 item 5's hole
        start, visit pi(0), hold             a fresh pass
        trigger, visit pi(1), hold           a second visit
        rst                                  item 6

    Two things about this sequence are not decoration, and both were learned by
    running the mutation battery against an earlier version of it:

    * **The holds stand still on a non-zero pattern.** An earlier draft went
      straight from one visit to the next, so the pattern was never *observed*
      holding, and the `no_hold` mutant survived.
    * **The gap happens while the pattern is non-zero.** An earlier draft put it
      after the `pi(1)` visit, whose pattern is zero for the usual context, so
      dropping the gate and dropping the destroy were both invisible and those
      two mutants survived as well.

    A stimulus that never holds, or that invalidates when the pins are already
    idle, cannot test either property. Item 5 in particular is only visible when
    something is actually being driven at the moment its matrix goes away.
    """
    a = Attached(N=N, C=C, W=list(W), O=list(O), oneshot=oneshot, valid=True,
                 stage_factory=stage_factory)
    a.cycle(start=1)            # the start is accepted
    a.cycle()                   # visit pi(0): the pins are still idle
    a.cycle()                   # the pattern is on the pins
    a.cycle()                   # held, no visit

    a.valid = False
    a.cycle()                   # the gate closes, pattern non-zero
    a.cycle()                   # still invalid

    a.valid = True
    a.cycle()                   # revalidated, no visit: must stay idle
    a.cycle()

    a.cycle(start=1)            # a fresh pass
    a.cycle()                   # visit pi(0)
    a.cycle()                   # its pattern
    a.cycle()                   # held

    a.cycle(trigger=1)          # visit pi(1)
    a.cycle()                   # its pattern
    a.cycle()                   # held

    a.cycle(rst=1)
    a.cycle()
    return a


def suite_obligations():
    """
    Every predicate of section 10.2, over the full stimulus and a range of
    contexts -- with an assertion that the stimulus really is the hard one. A
    trace with no gap and no revalidation would pass this suite while proving
    nothing about item 5, so the coverage of the stimulus is checked too.
    """
    fails = []
    checked = 0
    contexts = [
        (2, 2, [1, 1], [0, 0]),
        (1, 1, [1], [0]),
        (3, 3, [1, 2, 0], [0, 1, 2]),
        (4, 2, [4, 4], [0, 0]),          # every row always active
        (5, 3, [0, 0, 0], [0, 0, 0]),    # item 8's counterexample
        (8, 8, [1, 2, 3, 4, 5, 6, 7, 8], [0, 1, 2, 3, 4, 5, 6, 7]),
    ]
    for N, C, W, O in contexts:
        for oneshot in (False, True):
            a = stimulate(N, C, W, O, oneshot=oneshot)
            checked += 1

            v = valids(a)
            dips = gate_events(a)
            rises = [k for k in range(1, len(v)) if v[k] and not v[k - 1]]
            if not dips or not rises:
                fails.append(f"N={N}: the stimulus has no invalidation")
            if not [k for k in rises if not visits(a)[k]]:
                fails.append(f"N={N}: the stimulus never revalidates without a "
                             f"visit, so item 5 is untested")

            for name in which_predicates_fail(a):
                fails.append(f"N={N}, oneshot={oneshot}: {name}")

    if fails:
        for f in fails[:10]:
            print(f"  FAIL {f}")
        return False
    print(f"  {checked} stimuli x {len(PREDICATES)} obligations: all hold, and "
          f"every stimulus revalidates without a visit.")
    return True


def suite_hole():
    """
    Item 5, asserted directly rather than through a predicate. This is the case a
    level-only gate fails, so it is worth stating in the narrowest possible terms:
    the pins were non-idle, the context went away, the context came back with no
    visit, and the pins must be idle for the whole of it.
    """
    fails = []
    checked = 0
    for N, C, W, O in [(2, 2, [1, 1], [0, 0]),
                       (4, 4, [2, 2, 2, 2], [0, 0, 0, 0])]:
        a = Attached(N=N, C=C, W=W, O=O, valid=True)
        a.cycle(start=1)
        a.cycle()                       # pi(0)
        a.cycle()                       # the pattern is on the pins now
        checked += 1
        if a.pins_trace[-1] == 0:
            fails.append(f"N={N}: the setup never put anything on the pins "
                         f"(a fixture with no active row cannot test item 5)")

        a.valid = False
        a.cycle()
        checked += 1
        if a.pins_trace[-1] != 0:
            fails.append(f"N={N}: pins survived the invalidation "
                         f"({a.pins_trace[-1]:#x})")

        a.valid = True
        a.cycle()
        checked += 1
        if a.pins_trace[-1] != 0:
            fails.append(f"N={N}: pins came back ({a.pins_trace[-1]:#x}) on "
                         f"revalidation with no visit -- the gate-only hole")

        a.cycle()
        checked += 1
        if a.pins_trace[-1] != 0:
            fails.append(f"N={N}: pins are still {a.pins_trace[-1]:#x} a cycle "
                         f"later")

        a.cycle(start=1)                # a fresh pass
        a.cycle()                       # its first visit
        a.cycle()                       # and one clock later, the pattern
        checked += 1
        if a.pins_trace[-1] != a.bits_trace[-2]:
            fails.append(f"N={N}: a new pass did not restore the pins "
                         f"({a.pins_trace[-1]:#x}, wanted "
                         f"{a.bits_trace[-2]:#x})")

    if fails:
        for f in fails[:10]:
            print(f"  FAIL {f}")
        return False
    print(f"  {checked} checks: the pattern does not survive its matrix, and a "
          f"new pass restores the pins.")
    return True


def _weight_vectors(C, N):
    """Every weight vector of length C with 0 <= w_i <= N."""
    if C == 0:
        return [[]]
    out = []
    for head in range(N + 1):
        for tail in _weight_vectors(C - 1, N):
            out.append([head] + tail)
    return out


def _pass_with_gap_at(N, C, W, step):
    """A pass whose context is invalidated at `step`, then revalidated at once."""
    a = Attached(N=N, C=C, W=W, O=[0] * C, valid=True)
    for k in range(N):
        if k == step:
            a.valid = False
            a.cycle()               # the gap: the gate closes, the pattern dies
            a.valid = True
            a.cycle()               # revalidated with no visit
        a.cycle(start=1) if k == 0 else a.cycle(trigger=1)
    for _ in range(2):
        a.cycle()
    return a


def suite_exhaustive():
    """
    Section 10.3: small enough to exhaust. `N` in 1..6, `C` up to 2, every weight
    vector with `0 <= w_i <= N`, one-shot and continuous, and -- the axis the
    engine's own tests found interesting -- an invalidation at **every** step
    index.
    """
    fails = []
    checked = 0
    for N in range(1, 7):
        for C in range(1, min(N, 2) + 1):
            for weights in _weight_vectors(C, N):
                for oneshot in (False, True):
                    a = run_pass(N, C, weights, [0] * C, oneshot=oneshot)
                    checked += 1
                    for name in which_predicates_fail(a):
                        fails.append(f"N={N} C={C} W={weights} "
                                     f"oneshot={oneshot}: {name}")

                    for step in range(N):
                        b = _pass_with_gap_at(N, C, weights, step)
                        checked += 1
                        for name in which_predicates_fail(b):
                            fails.append(f"N={N} C={C} W={weights} gap@{step}: "
                                         f"{name}")

    if fails:
        for f in fails[:10]:
            print(f"  FAIL {f}")
        return False
    print(f"  {checked:,} exhaustive runs and gaps: all obligations hold.")
    return True


def suite_vectors():
    """
    The pattern presented at step `t` is `R(pi(t))` -- against
    `sim/vectors/matrix_*.txt`, not against an expectation invented here. This is
    the obligation section 9.3 names: the pin bits are the matrix's column.
    """
    fails = []
    checked = 0
    for fname, stride in (("matrix_edge.txt", 1), ("matrix_random.txt", 3)):
        path = os.path.join(VECTOR_DIR, fname)
        if not os.path.exists(path):
            fails.append(f"missing {fname}")
            continue
        for case in load_matrix_vectors(path)[::stride]:
            N, C = case["N"], case["C"]
            W, O, rows = case["weights"], case["offsets"], case["rows"]
            if N < 1 or C < 1 or N > 40:
                continue
            a = run_pass(N, C, W, O, oneshot=True)
            for k in range(len(a.pins_trace) - 1):
                if not visits(a)[k] or not valids(a)[k + 1]:
                    continue
                col = a.out_trace[k].column
                want = sum(1 << i for i, row in enumerate(rows)
                           if i < C and row[col])
                checked += 1
                if a.pins_trace[k + 1] != want:
                    fails.append(f"{fname} N={N}: cycle {k}, column {col}, pins "
                                 f"{a.pins_trace[k + 1]:#x} not R(j)={want:#x}")
                    break

    if fails:
        for f in fails[:10]:
            print(f"  FAIL {f}")
        return False
    print(f"  {checked:,} presented patterns equal R(pi(t)) from the reference "
          f"matrices.")
    return True


def suite_composition():
    """
    Section 10.1: attaching the output stage must not change the engine. The
    checker is `observer_periph.replay`, reused rather than restated -- a second
    implementation of the checker would be a second thing to get wrong.
    """
    fails = []
    checked = 0
    contexts = [(1, 1, [1], [0]), (2, 2, [1, 1], [0, 0]),
                (5, 3, [2, 1, 3], [0, 1, 4]), (8, 4, [8, 8, 8, 8], [0, 0, 0, 0])]
    for N, C, W, O in contexts:
        for oneshot in (False, True):
            a = stimulate(N, C, W, O, oneshot=oneshot)
            checked += 1
            if not a.composition_holds():
                fails.append(f"N={N} oneshot={oneshot}: the engine's own trace "
                             f"changed when the output stage was attached")

    if fails:
        for f in fails[:10]:
            print(f"  FAIL {f}")
        return False
    print(f"  {checked} traces: the engine behaves identically with the output "
          f"stage attached.")
    return True


def suite_port_list():
    """
    Item 11, and sections 2.2 and 8: the interface is exactly section 2.1's. The
    stage's inputs are checked by signature -- no sixth input -- and the absences
    by name: no column index, no `N`, no trigger, and no storage beyond one
    register and one edge bit.
    """
    fails = []
    checked = 0

    tick = list(inspect.signature(OutputStage.tick).parameters)[1:]
    pins = list(inspect.signature(OutputStage.pins).parameters)[1:]
    checked += 2
    if tick != ["column_bits", "visit_valid", "valid", "rst"]:
        fails.append(f"tick takes {tick}, expected the four of section 6.5")
    if pins != ["valid"]:
        fails.append(f"pins takes {pins}, expected only the gate's input")

    members = [m for m in dir(OutputStage) if not m.startswith("__")]
    for absent in ("column", "trigger", "start", "seed", "N"):
        checked += 1
        if absent in members:
            fails.append(f"OutputStage exposes `{absent}`, which section 2.2 "
                         f"refuses")

    attrs = set(vars(OutputStage(max_c=8)))
    checked += 1
    if attrs != {"max_c", "pattern", "valid_q"}:
        fails.append(f"the stage stores {sorted(attrs)}, expected one pattern, "
                     f"one edge bit and the geometry")

    if fails:
        for f in fails[:10]:
            print(f"  FAIL {f}")
        return False
    print(f"  {checked} structural checks: four inputs, one register, no column, "
          f"no N, no trigger.")
    return True


# ---------------------------------------------------------------------------
# Section 10.6 -- the mutation battery
#
# Each mutant is a plausibly-wrong stage. What the suite claims is that its
# predicates catch them; the number that matters is how many are caught, because
# that measures the suite rather than the mutants.
# ---------------------------------------------------------------------------

class MutNoGate(OutputStage):
    """The presentation gate is dropped: a stale pattern is driven."""

    def pins(self, valid):
        return self.pattern


class MutNoHold(OutputStage):
    """The pattern is not held: it lasts only the visit's own cycle."""

    def tick(self, column_bits, visit_valid, valid, rst):
        self.pattern = (column_bits & low_mask(self.max_c)) if visit_valid else 0
        self.valid_q = 1 if valid else 0


class MutNoDestroy(OutputStage):
    """
    The destroy on a falling `VALID` is dropped -- section 6.2's subtle one. The
    gate stays, so this passes everything except the revalidate-without-a-visit
    case, which is exactly why that case exists.
    """

    def tick(self, column_bits, visit_valid, valid, rst):
        if rst:
            self.pattern = 0
        elif visit_valid:
            self.pattern = column_bits & low_mask(self.max_c)
        self.valid_q = 1 if valid else 0


class MutDestroyOnRise(OutputStage):
    """
    A **correct variant**, kept as a control: destroying on any cycle where
    `valid` is low is equivalent to destroying on the falling edge alone, because
    the two differ only in cycles where nothing is presented. If the suite
    rejects this, the suite is wrong.
    """

    def tick(self, column_bits, visit_valid, valid, rst):
        if rst or not valid:
            self.pattern = 0
        elif visit_valid:
            self.pattern = column_bits & low_mask(self.max_c)
        self.valid_q = 1 if valid else 0


MUTANTS = (
    ("no_gate", MutNoGate, "gate"),
    ("no_hold", MutNoHold, "hold"),
    ("no_destroy", MutNoDestroy, "no_dead_matrix"),
)


def suite_mutations():
    """
    Plant each mutant and require a *named* predicate to catch it. The
    no-destroy mutant must be caught by `p_no_dead_matrix` **and by nothing
    else**: a mutation that everything catches measures the suite's redundancy
    rather than its teeth, which is section 10.6's own words.
    """
    fails = []
    caught = 0

    control = stimulate(2, 2, [1, 1], [0, 0], stage_factory=MutDestroyOnRise)
    if which_predicates_fail(control):
        fails.append("the correct variant MutDestroyOnRise was rejected by "
                     + ", ".join(which_predicates_fail(control)))

    for name, factory, expected in MUTANTS:
        a = stimulate(2, 2, [1, 1], [0, 0], stage_factory=factory)
        seen = which_predicates_fail(a)
        if not seen:
            fails.append(f"mutant {name} survived every predicate")
            continue
        caught += 1
        if expected not in seen:
            fails.append(f"mutant {name} was caught by {seen}, not by "
                         f"`{expected}`")
        if name == "no_destroy" and seen != [expected]:
            fails.append(f"mutant no_destroy was caught by {seen}; section 10.6 "
                         f"requires `{expected}` alone")

    if fails:
        for f in fails[:10]:
            print(f"  FAIL {f}")
        return False
    print(f"  {caught} mutants caught by their named predicate; the correct "
          f"variant survived.")
    return True


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    results = []

    print("Section 10.2 -- the obligations, as predicates:")
    results.append(suite_obligations())
    print()

    print("Section 6.2 -- the revalidate-without-a-visit hole, directly:")
    results.append(suite_hole())
    print()

    print("Section 10.3 -- exhaustive small contexts, gaps at every step:")
    results.append(suite_exhaustive())
    print()

    print("Sections 3.4 and 9.3 -- the pattern is R(pi(t)):")
    results.append(suite_vectors())
    print()

    print("Section 10.1 -- the composition rule:")
    results.append(suite_composition())
    print()

    print("Sections 2.2, 8 and 10.2 item 11 -- the port list:")
    results.append(suite_port_list())
    print()

    print("Section 10.6 -- the mutation battery:")
    results.append(suite_mutations())
    print()

    if all(results):
        print("output_engine.py satisfies docs/Output_Engine.md.")
        print("The v2.0b output stage is now an executable contract for its RTL.")
        return 0

    print("CONFORMANCE FAILED")
    return 1


if __name__ == "__main__":
    sys.exit(main())

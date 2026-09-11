"""
BCMC hardware observer -- cycle-level reference model (v2.0a).

This module is the *golden model* for `rtl/bcmc_observer.v`, in exactly the
sense that `observers.py` is the golden model for `sw/bcmc_observer.{h,c}`. It
implements `docs/Hardware_Observer_Architecture.md` (sections 3-6), and where the
two disagree the document is right and this model is a bug.

    docs/Hardware_Observer_Architecture.md  ->  observer_hw.py  ->  rtl/bcmc_observer.v

There is no BCMC mathematics here, and no traversal *mathematics* either: the
engine holds a cursor, the ordering is a **source** behind the seam (section
3.3), and v2.0a's only source is `pi(t) = t`. Every matrix bit belongs to
`reference.py`; every ordering belongs to `observers.py`. This file owns the
third thing, and the only one software could not own cheaply: *when* a visit
happens.

Timekeeping
-----------
The model is edge-driven, like the RTL it stands for. `outputs()` gives the
combinational value of each output *during* a cycle; `edge()` applies the rising
edge at the end of that cycle. The one timing rule of the document (section 5.1)
is therefore visible in the code: an input asserted during cycle `k` changes the
registers at the end of cycle `k`, so its effect is seen in cycle `k+1`.

    >>> eng = SequentialEngine(N=3)
    >>> _ = eng.edge(start=True)        # start during cycle 0
    >>> eng.outputs().visit_valid       # the visit appears in cycle 1
    True
    >>> eng.outputs().column
    0
    >>> _ = eng.edge(trigger=True)      # trigger during cycle 1
    >>> eng.outputs().column            # pi(1) appears in cycle 2
    1

A note the code cannot state and the document does not yet
-----------------------------------------------------------
Two rows of the section 3.11 event table can be true at once: `done & oneshot`
(leave `RUN`) and `trigger & RUN & valid` (advance). This model resolves the
overlap by giving **completion priority** -- in the cycle of a one-shot pass's
last visit, a `trigger` is ignored, because the pass has ended and there is no
cursor left to advance. That is a decision this model had to make and the
document did not; it is recorded as a finding so the document can adopt it.
"""

from collections import namedtuple

from observers import SplitMix32   # the one pinned generator; see section 9.3

__all__ = [
    "IDLE",
    "RUN",
    "Cycle",
    "SimResult",
    "SequentialEngine",
    "simulate",
    "identity_source",
    "visit_columns",
    "bank_fill_cycles",
    "bank_fill_passes",
]

IDLE = "IDLE"
RUN = "RUN"


def identity_source(t):
    """
    v2.0a's only traversal source: `pi(t) = t`.

    It is a function, so it needs no storage and no cycle -- the seam of
    section 3.3 costs one wire here and nothing else. The reference observers in
    `observers.py` are the same two `pi` functions in their software form.

    >>> identity_source(7)
    7
    """
    return t


Cycle = namedtuple(
    "Cycle",
    "index state running scheduled column visit_valid done aborted",
)


# `scheduled` is the registered strobe `visit_q` ("a visit is due this cycle"),
# and `visit_valid` is the output ("and it is being presented"). They differ
# exactly when `valid` is low, which is the whole of section 3.7 made visible.
SimResult = namedtuple(
    "SimResult",
    "cycles visits accepted_starts accepted_triggers aborts suppressed",
)


class SequentialEngine:
    """
    The v2.0a engine: two states, one cursor, one output register, no bus.

    Registers (section 3.2): `state`, `tq`, `col`, `visit`, `aborted`.
    Combinational outputs (sections 3.6-3.8): `running`, `column`, `visit_valid`,
    `done`. `visit_valid` and `done` are gated on `valid` at the output rather
    than inside the state machine -- that is the whole of section 3.7, and it is
    what makes the invalidation guarantee same-cycle.

    The engine is deliberately *not* given the matrix: it has no `weights`, no
    `offsets` and no rows. It holds a cursor, and `docs/Observers.md` forbids it
    to hold anything else.

    >>> eng = SequentialEngine(N=4)
    >>> eng.state
    'IDLE'
    >>> eng.outputs().running
    False

    `N = 0` is not an instance, so `start` never admits it (section 3.4):

    >>> eng0 = SequentialEngine(N=0)
    >>> _ = eng0.edge(start=True)
    >>> eng0.state
    'IDLE'
    """

    def __init__(self, N, oneshot=False, source=identity_source):
        if N < 0:
            raise ValueError(f"N must be >= 0, got {N}")
        self.N = N
        self.oneshot = bool(oneshot)
        self.source = source
        self.state = IDLE
        self.tq = 0
        self.col = 0
        self.visit = False
        self.aborted = False
        self.cycle = 0
        # Counters for the conservation invariant (section 7.4). They live in
        # the engine, not in the test, so that "accepted" means exactly what
        # `edge()` decided and cannot drift from it.
        self.accepted_starts = 0
        self.accepted_triggers = 0
        self.aborts = 0

    # -- combinational outputs -------------------------------------------

    def outputs(self, valid=True):
        """
        Every output *during* the current cycle. Pure: it changes nothing.

        >>> eng = SequentialEngine(N=2)
        >>> c = eng.outputs()
        >>> (c.running, c.column, c.visit_valid, c.done, c.aborted)
        (False, 0, False, False, False)

        The gate that matters (section 3.7): a scheduled visit is not presented
        when the context is invalid, in the very cycle it is invalid.

        >>> _ = eng.edge(start=True)
        >>> eng.outputs(valid=True).visit_valid
        True
        >>> eng.outputs(valid=False).visit_valid
        False
        """
        vv = bool(self.visit and valid)
        done = bool(vv and self.N >= 1 and self.tq == self.N - 1)
        return Cycle(
            index=self.cycle,
            state=self.state,
            running=self.state == RUN,
            scheduled=bool(self.visit),
            column=self.col,
            visit_valid=vv,
            done=done,
            aborted=self.aborted,
        )

    # -- the rising edge --------------------------------------------------

    def edge(self, *, start=False, trigger=False, valid=True, rst=False):
        """
        Apply one rising edge. Updates the registers; returns nothing.

        The precedence is the section 3.11 table, in order: `rst` first, then
        the invalid-context abort, then the state-dependent rules. Two things
        are worth reading off the code because they are the two an RTL
        implementation is most likely to get wrong: the abort is tested before
        anything that could advance a pass, and a one-shot completion is tested
        before `trigger`.

        >>> eng = SequentialEngine(N=2)
        >>> _ = eng.edge(trigger=True)      # a trigger in IDLE
        >>> eng.state, eng.outputs().visit_valid
        ('IDLE', False)
        """
        if rst:
            self.state = IDLE
            self.tq = 0
            self.col = 0
            self.visit = False
            self.aborted = False
            self.cycle += 1
            return

        # (1) An invalid context ends a pass. This cannot fire on a cycle whose
        # visit completed the pass: a completing visit requires visit_valid,
        # which requires valid, so "not valid" already excludes it. That is the
        # "(unless the pass had already completed)" clause of 3.10, obtained
        # here for free rather than by a special case.
        if self.state == RUN and not valid:
            self.state = IDLE
            self.visit = False
            self.aborted = True
            self.aborts += 1
            self.cycle += 1
            return

        # (2) Idle: only an accepted `start` does anything.
        if self.state == IDLE:
            if start and valid and self.N >= 1:
                self.state = RUN
                self.tq = 0
                self.col = self.source(0)
                self.visit = True
                self.aborted = False     # a restart acknowledges an abort
                self.accepted_starts += 1
            else:
                self.visit = False
            self.cycle += 1
            return

        # (3) Running and valid. A one-shot pass ends at its last visit...
        if self.visit and self.tq == self.N - 1 and self.oneshot:
            self.state = IDLE
            self.visit = False
            self.cycle += 1
            return

        # ... otherwise a trigger advances (wrapping at the boundary), and the
        # absence of a trigger simply ends the one-cycle strobe.
        if trigger:
            self.tq = self.tq + 1 if self.tq + 1 < self.N else 0
            self.col = self.source(self.tq)
            self.visit = True
            self.accepted_triggers += 1
        else:
            self.visit = False
        self.cycle += 1

    # -- traversal-source seam (section 3.3) ------------------------------

    def set_source(self, source):
        """
        Replace the traversal source behind the seam.

        v2.0c is a source swap, not an engine change, so this is the whole of
        what changes there:

        >>> eng = SequentialEngine(N=4)
        >>> eng.set_source(lambda t: (2 * t) % 4)
        >>> _ = eng.edge(start=True)
        >>> eng.outputs().column
        0
        >>> _ = eng.edge(trigger=True)
        >>> eng.outputs().column
        2
        """
        self.source = source


# ---------------------------------------------------------------------------
# Driving the engine over a whole run
# ---------------------------------------------------------------------------

def simulate(
    N,
    *,
    start_cycle=None,
    trigger_cycles=(),
    valid_low=(),
    rst_cycles=(),
    oneshot=False,
    cycles=None,
    source=identity_source,
):
    """
    Drive the engine for a whole run and return a `SimResult`.

    Cycles are indexed from 0. `start_cycle` is the cycle in which `start` is
    asserted (if any); `trigger_cycles` the cycles in which `trigger` is;
    `valid_low` the cycles in which the context is invalid; `rst_cycles` the
    cycles in which `rst` is. Everything unlisted is well behaved by default, so
    a test states only what it cares about.

        >>> r = simulate(3, start_cycle=0, trigger_cycles=(2, 3), cycles=6)
        >>> r.visits
        [0, 1, 2]
        >>> (r.accepted_starts, r.accepted_triggers)
        (1, 2)
        >>> [c.index for c in r.cycles if c.done]
        [4]

    Conservation, stated correctly
    ------------------------------
    The obvious invariant -- `visits == accepted requests` -- is **false**, and
    finding that is the first thing this model did. A request accepted in one
    cycle schedules its visit for the next, so if the context is invalidated in
    between, the request was honoured (the cursor moved) but its visit is
    suppressed by the output gate. Section 3.11 claimed the count is preserved
    across an invalidation; it is not, by exactly one visit, and the corrected
    statement is:

        accepted_starts + accepted_triggers == scheduled
        scheduled                          == visits + suppressed

    and `suppressed` is **at most one per invalidation**, because a request's
    visit is only ever one cycle away from its acceptance. Both equalities are
    checked in `test_observer_hw.py`.
    """
    triggers = set(trigger_cycles)
    lows = set(valid_low)
    rsts = set(rst_cycles)

    if cycles is None:
        marks = list(triggers) + list(lows) + list(rsts)
        if start_cycle is not None:
            marks.append(start_cycle)
        cycles = (max(marks) if marks else 0) + 3

    eng = SequentialEngine(N, oneshot=oneshot, source=source)
    out = []
    visits = []
    suppressed = 0
    for c in range(cycles):
        valid = c not in lows
        cyc = eng.outputs(valid)
        out.append(cyc)
        if cyc.visit_valid:
            visits.append(cyc.column)
        elif cyc.scheduled:
            suppressed += 1          # due, but the context was gone
        eng.edge(
            start=(start_cycle == c),
            trigger=(c in triggers),
            valid=valid,
            rst=(c in rsts),
        )
    return SimResult(out, visits, eng.accepted_starts, eng.accepted_triggers,
                     eng.aborts, suppressed)


def visit_columns(cycles):
    """
    The columns presented over a list of `Cycle` records, in order.

        >>> r = simulate(3, start_cycle=0, trigger_cycles=(2, 3), cycles=6)
        >>> visit_columns(r.cycles)
        [0, 1, 2]
    """
    return [c.column for c in cycles if c.visit_valid]


# ---------------------------------------------------------------------------
# The bank fill bound (section 9.3)
# ---------------------------------------------------------------------------

def bank_fill_cycles(N, seed):
    """
    Clock cycles a hardware Fisher-Yates bank of `N` entries needs to fill.

    Section 9.3 makes this a *measurement*, because the whole v2.0c decision
    rests on it: `pi(t)` of a shuffled traversal depends on the entire history
    of swaps, so the permutation must be built into a bank before the pass that
    reads it, and the question is whether the bank can be built within the time
    the pass allows.

    The arithmetic is exact and needs no simulation of the RTL, because the
    hardware model of the shuffle is one `SplitMix32.next()` per clock and one
    bank write per accepted draw, with the write overlapping the draw:

        cycles = the number of next() calls the whole shuffle consumes
               = (N - 1) + (rejected draws)

    The lower bound `N - 1` is therefore structural -- one draw per step, at
    least -- and every rejection adds one:

        >>> bank_fill_cycles(64, 1) >= 63
        True

    A pass over the same `N`, at one visit per clock, is `N` cycles. So the
    comparison the caller wants is `bank_fill_cycles(N, seed)` against `N`
    (times the clocks-per-visit rate), and it is checked in the test.
    """
    if N < 1:
        raise ValueError(f"N must be >= 1, got {N}")
    rng = SplitMix32(seed)
    cycles = 0
    p = list(range(N))
    for i in range(N - 1, 0, -1):
        mask = 1
        while mask < i:
            mask = (mask << 1) | 1
        while True:
            cycles += 1                     # one next() per clock
            x = rng.next() & mask
            if x <= i:
                j = x
                break
        p[i], p[j] = p[j], p[i]
    return cycles


def bank_fill_passes(N, seed, clocks_per_visit=1):
    """
    The bank fill expressed in whole **pass intervals**, not in lead.

    `clocks_per_visit` is the trigger period in clocks, so one pass occupies
    `N * clocks_per_visit` clocks. This returns the smallest number of pass
    intervals that covers the fill:

        >>> bank_fill_passes(64, 1) >= 1
        True

    It is a **duration**, and the name says so deliberately. The earlier name
    here was `bank_lead_passes`, which was wrong: how much *lead* a double
    buffer needs is not a property of the fill at all. It depends on when
    generation starts (at the swap, or after it?), whether the generator may
    write the inactive bank while a pass is in flight, and whether a pass may
    begin without a bank ready. Those are scheduling decisions belonging to the
    v2.0c specification. What this function can honestly report is how many pass
    intervals of generation the fill *spans*; converting that into a required
    lead is a separate argument, and making that argument is the point of
    section 9.3 rather than something to bury in a helper.

    One consequence is worth stating: the source is legal at this rate exactly
    when the fill spans at most one pass interval, i.e. when this returns 0 or
    1.
    """
    if clocks_per_visit < 1:
        raise ValueError("clocks_per_visit must be >= 1")
    span = N * clocks_per_visit
    return -(-bank_fill_cycles(N, seed) // span)


# ---------------------------------------------------------------------------
# Self-check
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    import doctest

    failures, tests = doctest.testmod()
    if failures:
        raise SystemExit(f"observer_hw.py: {failures} of {tests} doctests FAILED")
    print(f"observer_hw.py: {tests} doctests passed")

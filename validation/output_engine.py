"""
The v2.0b output engine, as a cycle model.

Derived from `docs/Output_Engine.md`, sections 1 to 6, **in order to falsify that
document**. It is not a design sketch and it is not the RTL's ancestor: it is the
executable interpretation the specification has to survive, exactly as
`observer_hw.py` was for the engine and `observer_periph.py` was for the register
window. Both of those found defects in their document -- F11 and F12, and T1 with
the `ONESHOT` trap -- and this one is written in the hope of doing the same.

What it composes, and what it refuses to reproduce
--------------------------------------------------
It consumes the *declared outputs* of `observer_hw.SequentialEngine`, so it owns
no cursor, no visit strobe and no abort. The matrix bits are taken from
`reference.py`'s `bcmc_column`, which is the golden matrix the observer's own
projection is already held to -- the model does not restate the characteristic
function either.

The five lines (section 6.5), which is the whole of the datapath:

    on rst:                 pattern <= 0
    on falling valid:       pattern <= 0
    on visit_valid:         pattern <= column_bits
    otherwise:              pattern unchanged
    pins                    = pattern, gated by valid

Usage:
    python3 output_engine.py            # doctests
"""

from observer_hw import SequentialEngine, identity_source
from observer_periph import replay
from reference import bcmc_column

# The pattern width. It must match the peripheral the engine hangs off:
# `validation/observer_periph.py`'s REF_MAX_C and `rtl/bcmc_obs_wb.v`'s own
# default MAX_C. The pins are MAX_C wide, and section 2.3 is why no masking
# happens in the datapath.
REF_MAX_C = 32


def low_mask(width):
    """`width` low bits set. `low_mask(0)` is 0, not -1."""
    return (1 << width) - 1 if width > 0 else 0


def bits_of(N, W, O, column, C, max_c=REF_MAX_C):
    """
    M(., column) as a bit vector, from reference.py -- the golden matrix, in the
    same sense `gen_observer_hw_vectors.py` and `gen_observer_wb_vectors.py` use
    it. The projection is not computed here; it is read off the reference model.

    Lanes at or above `C` are zero, which is the claim section 2.3 asserts rather
    than masks.

    >>> bits_of(2, [1, 1], [0, 0], 0, 2)
    3
    >>> bits_of(2, [1, 1], [0, 0], 1, 2)
    0
    >>> bits_of(0, [1, 1], [0, 0], 0, 2)   # N = 0: outside the domain
    0
    """
    if N < 1 or C < 1 or column >= N:
        return 0
    v = 0
    for i, b in enumerate(bcmc_column(W, O, column, N)[:C]):
        if b:
            v |= (1 << i)
    return v


class OutputStage:
    """
    Sections 3 to 6: one pattern register, one presentation gate, and the rule
    that destroys the pattern when its matrix does.

    >>> s = OutputStage(max_c=8)
    >>> s.pins(valid=True)          # nothing latched yet: idle
    0
    >>> s.tick(column_bits=0b101, visit_valid=True, valid=True, rst=False)
    >>> s.pins(valid=True)
    5
    >>> s.pins(valid=False)         # the gate, section 6.2
    0

    The hole the gate does not close (section 6.2): invalidate, then revalidate
    *without* a visit, and the old pattern must not come back.

    >>> s.tick(column_bits=0, visit_valid=False, valid=False, rst=False)
    >>> s.pins(valid=True)          # was 5 before the invalidation
    0
    """

    def __init__(self, max_c=REF_MAX_C):
        self.max_c = max_c
        self.pattern = 0
        self.valid_q = 0            # for the falling-edge detector

    def pins(self, valid):
        """
        The presentation gate (section 6.2): combinational, so the pins are idle
        in the very cycle the context disappears. Mirrors the engine's
        `visit_valid = visit_q & valid` one layer down.
        """
        return self.pattern if valid else 0

    def tick(self, column_bits, visit_valid, valid, rst):
        """
        Close a cycle. The priority is section 6.5's, and each row of it matters:
        `rst` beats a visit, and so does a falling `valid` -- a visit cannot
        coincide with a falling `valid` (`visit_valid` is already gated on it),
        but `rst` can coincide with a visit and must win.

        >>> s = OutputStage(max_c=8)
        >>> s.tick(0b1111, visit_valid=True, valid=True, rst=True)   # rst wins
        >>> s.pattern
        0
        >>> s.tick(0b101, visit_valid=True, valid=True, rst=False)
        >>> s.tick(0, visit_valid=False, valid=True, rst=False)      # hold
        >>> s.pattern
        5
        """
        if rst:
            self.pattern = 0
        elif self.valid_q and not valid:
            self.pattern = 0
        elif visit_valid:
            self.pattern = column_bits & low_mask(self.max_c)
        self.valid_q = 1 if valid else 0

    def lane_above_c(self, column_bits, C):
        """
        Section 2.3's assertion, as a function rather than an `ifdef`: the bits of
        `column_bits` at or above `C`. It must always be zero, and `C` exists as an
        input for no other reason. Returns the offending bits.

        >>> OutputStage().lane_above_c(0b011, 2)
        0
        >>> OutputStage().lane_above_c(0b111, 2)
        4
        """
        if C >= self.max_c:
            return 0
        return column_bits & (low_mask(self.max_c) & ~low_mask(C))


class Attached:
    """
    An observer engine with an output stage on its outputs -- section 8.1's
    wiring -- and section 10.1's composition rule made checkable.

    The rule is that attaching the output stage must not change the engine. The
    engine's input and output traces are recorded exactly as
    `observer_periph.py` records them, and **the same checker is reused rather
    than restated**: `observer_periph.replay` feeds an input trace to a bare
    engine and must reproduce its output trace. A stall, a hidden request or a
    second traversal would appear as a disagreement, and it cannot hide behind
    agreement on the pins.

    >>> a = Attached(N=2, C=2, W=[1, 1], O=[0, 0], valid=True)
    >>> _ = a.cycle(start=1)                # the start is accepted
    >>> _ = a.cycle()                       # pi(0) is presented...
    >>> a.out_trace[-1].visit_valid
    True
    >>> a.pins_trace[-1]                    # ... and the pins are still idle
    0
    >>> _ = a.cycle()                       # one clock later (section 4.1)
    >>> a.pins_trace[-1]                    # M(., 0) for weight 1 rows
    3
    >>> a.composition_holds()
    True
    """

    def __init__(self, N=0, C=0, W=(), O=(), oneshot=False, valid=False,
                 max_c=REF_MAX_C, source=identity_source,
                 stage_factory=OutputStage):
        self.N = N
        self.C = C
        self.W = list(W)
        self.O = list(O)
        self.valid = valid
        self.oneshot = oneshot
        self.max_c = max_c
        self.eng = SequentialEngine(N, source=source)
        # `stage_factory` exists for one reason: the mutation battery of section
        # 10.6 has to be able to plant a bug and show that a *named* check
        # catches it. It is not a configuration hook.
        self.stage = stage_factory(max_c=max_c)

        # The engine's traces, in the format observer_periph.py records, plus
        # the two things the output stage adds.
        self.in_trace = []
        self.out_trace = []
        self.bits_trace = []        # the `column_bits` wire, per cycle
        self.pins_trace = []        # the pins, per cycle

    def cycle(self, rst=0, start=0, trigger=0):
        """
        One cycle: observe, record, then take the edge that closes it. Returns
        `(outputs, pins)` for the cycle just observed.

        The order matters and is the specification's: `pins` is read *before* the
        edge, so it reflects the latch as it stood during the cycle -- which is
        what makes the one-clock rule (section 4.1) checkable rather than assumed.
        """
        valid = bool(self.valid)
        outputs = self.eng.outputs(valid)
        bits = bits_of(self.N, self.W, self.O, outputs.column, self.C,
                       self.max_c)
        pins = self.stage.pins(valid)

        self.in_trace.append((self.N, self.oneshot, bool(rst), bool(start),
                              bool(trigger), valid))
        self.out_trace.append(outputs)
        self.bits_trace.append(bits)
        self.pins_trace.append(pins)

        self.eng.N = self.N
        self.eng.oneshot = self.oneshot
        self.eng.edge(start=start, trigger=trigger, valid=valid, rst=rst)
        self.stage.tick(bits, outputs.visit_valid, valid, rst)
        return outputs, pins

    def composition_holds(self):
        """Section 10.1's rule, using the checker the register window uses."""
        return replay(self.in_trace) == self.out_trace

    def lane_above_c_seen(self):
        """Every cycle's `column_bits`, checked against section 2.3's assertion."""
        return [self.stage.lane_above_c(b, self.C) for b in self.bits_trace]


def run_pass(N, C, W, O, oneshot=True, max_c=REF_MAX_C, idle_tail=3,
             stage_factory=OutputStage):
    """
    One pass, cycled to its end, then a few idle cycles.

    The cadence is §3.4 and §3.8: the **start** schedules the first visit, and
    each later visit is a `trigger`'s. So `N` visits need one start and `N - 1`
    triggers -- the same arithmetic the register map's `T1` audit had to correct,
    where "poll until DONE" only works once a non-software source exists.

    Returns an `Attached` whose traces cover the start, the pass and the tail.
    """
    a = Attached(N=N, C=C, W=W, O=O, oneshot=oneshot, valid=True, max_c=max_c,
                 stage_factory=stage_factory)
    for k in range(N):
        a.cycle(start=1) if k == 0 else a.cycle(trigger=1)
    for _ in range(idle_tail):
        a.cycle()
    return a


def pin_changes(pins):
    """
    The number of application events (section 3.1) in a pins trace.

    Note what this is *not*: it is not the number of visits. Two consecutive
    visits can present the same pattern, and a visit can present zero while the
    pins are already idle, so no equality holds. The equality was in the
    specification and is false; section 10.2 item 8 carries the correction.

    >>> pin_changes([0, 3, 3, 0, 0, 1])
    3
    """
    return sum(1 for x, y in zip(pins, pins[1:]) if x != y)


# ---------------------------------------------------------------------------
# Self-check
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    import doctest
    import sys

    failures, tests = doctest.testmod()
    print(f"output_engine.py: {tests} doctests, {failures} failure(s)")
    sys.exit(1 if failures else 0)

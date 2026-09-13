"""
The source window, as a model: the contract rtl/bcmc_obs_wb.v must implement.

Derived from `docs/Traversal_Sources_Specification.md` section 8.4, which states
what sections 6 and 8.3 leave to the implementation.

It COMPOSES the already-verified source models rather than reimplementing them.
That is the point of this file: the window is a control contract, and a model that
recomputed a bank or a generator draw would be a second implementation of
something already proven, and would falsify nothing about the window. Every
question about a source's *answers* is put to `traversal_sources`.

So what is modelled here is: which source is selected, whether it is ready, what
the mux presented, and which write caused which load or readiness transition.

Usage:
    python3 observer_window.py            # doctests
"""

from traversal_sources import (AffineSource, IdentitySource, ShuffledSource)

SEL_ID, SEL_AFFINE, SEL_SHUFFLED = 0, 1, 2

# OBS_STATUS bit positions this window owns (docs/Observer_Register_Map.md 6.1).
BIT_DONE, BIT_ABORTED, BIT_READY, BIT_UNDERRUN = 0, 1, 3, 4

# OBS_CTRL bit positions (section 6.1): [1:0] START/STEP, 2 ONESHOT, 3 RESET,
# [5:4] the source selector.
CTRL_SEL_SHIFT = 4


def ctrl(sel, step=False, oneshot=False):
    """
    An OBS_CTRL word. A CTRL write sets EVERY field it has, which is why a STEP
    has to carry the rest of the word -- and why a selector change while RUNNING
    is refused rather than silently applied (section 6.3, and T1's audit).

    >>> ctrl(SEL_AFFINE, step=True)
    18
    """
    return ((sel & 0x3) << CTRL_SEL_SHIFT) | (0x2 if step else 0) | \
           (0x8 if oneshot else 0)


class Window:
    """
    The window's registers, the mux, and the readiness/underrun projection.

    >>> w = Window(N=4, seed=7)
    >>> w.write_seed(7, running=False)      # an accepted seed write always loads
    True
    >>> w.loads
    [1, 1, 1]
    """

    def __init__(self, N=1, seed=0, bank_n_max=256):
        self.sel = SEL_ID
        self.oneshot = False   # the window's mode bit; part of the CTRL readback
        # The window's uniform recovery latch (section 7.1). A write to OBS_SEED,
        # the selector or N sets it, and the next clock clears it, so readiness is
        # low for exactly one cycle for EVERY source. It is deliberately NOT set
        # at construction: reset is not among the events section 7.1 lists, so at
        # reset only the sources' own readiness speaks.
        self.recovery_q = False
        self.seed = seed & 0xFFFFFFFF
        self.N = N
        self.n_q = N          # the window's own copy of N: the change detector
        self.sources = [
            IdentitySource(N, self.seed),
            AffineSource(N, self.seed),
            ShuffledSource(N, self.seed, bank_n_max=bank_n_max),
        ]
        # How many load pulses each source has seen. A count, not a flag: the
        # gates are about *whether* a write pulsed.
        self.loads = [0, 0, 0]
        self.ts_t = 0
        # The sticky side. The levels belong to the sources, the latches to the
        # window, and that split is what makes RW1C behave (section 8.4 item 5).
        self.under_lat = False
        self.unready_lat = False
        self.underrun_seen = False

    #--- helpers -------------------------------------------------------------

    def _load_all(self):
        # A seed write, a selector change and a change in N all arrive here, which
        # is why the uniform recovery lives here: section 7.1's "for every source"
        # is then true by construction rather than by three sources agreeing.
        self.recovery_q = True
        for i, s in enumerate(self.sources):
            s.load(self.seed)
            self.loads[i] += 1          # the count is the window's, and is what
                                        # the gates ask about

    def selected(self):
        return self.sources[self.sel]

    def snapshot(self):
        """Everything a refusal must leave untouched, for the E1/E4 gates."""
        return (self.sel, self.seed, self.N, self.n_q, tuple(self.loads),
                [s.ready() for s in self.sources],
                [s.pi(t) for s in self.sources for t in range(self.N)],
                self.under_lat, self.unready_lat)

    #--- the register writes (section 8.4 items 1 and 3) ----------------------

    def write_ctrl(self, value, running):
        """
        False if refused. A refusal changes NOTHING -- not even the other fields
        of OBS_CTRL, which is the frozen rule for refusals and the reason
        `STEP | ONESHOT` must be carried.

        A selector that CHANGES loads all three sources; one that merely restates
        the current value does not, because every STEP carries the selector and a
        pulse would discard a filled bank and stall the stream.

        >>> w = Window(N=4, seed=1)
        >>> _ = w.write_ctrl(ctrl(SEL_ID, step=True), running=False)
        >>> w.loads
        [0, 0, 0]
        """
        sel = (value >> CTRL_SEL_SHIFT) & 0x3
        if sel == 3:
            return False                     # E1: names a source that is not
        if running and sel != self.sel:
            return False                     # E4's shape: not while a pass runs
        if sel != self.sel:
            self.sel = sel
            self._load_all()
        # ONESHOT is a field of the SAME write, so an accepted write stores it
        # whether or not the selector moved. A refusal stores nothing at all --
        # that is the frozen rule the first doctest above is about.
        self.oneshot = bool((value >> 3) & 1)
        return True

    def read_ctrl(self):
        """
        An `OBS_CTRL` read: bits 5:4 `SELECT`, bit 3 `ONESHOT`, everything else 0.

        The three W1S bits read 0 because they have no readable value, and the
        reserved bits read 0 because they are reserved. The readback is therefore a
        function of the selector and ONESHOT alone -- deliberately independent of
        whether a source is ready, whether a pass is running, and which trigger
        source is armed. Readable `SELECT` is a v2.0c addition, and this is where
        its layout is stated; the RTL's read mux and the v2.0c corpus follow it.

        >>> w = Window(N=4, seed=1)
        >>> w.read_ctrl()
        0
        >>> _ = w.write_ctrl(ctrl(SEL_AFFINE, oneshot=True), running=False)
        >>> w.read_ctrl()
        24
        """
        return ((self.sel & 0x3) << CTRL_SEL_SHIFT) | (0x8 if self.oneshot else 0)

    def write_seed(self, seed, running):
        """
        An accepted seed write always loads, even when the value is unchanged:
        the seed is opaque 32 bits and "same value" is not the window's call.

        >>> w = Window(N=4, seed=5)
        >>> _ = w.write_seed(5, running=False)
        >>> w.loads
        [1, 1, 1]
        >>> _ = w.write_seed(9, running=True)
        >>> w.loads
        [1, 1, 1]
        """
        if running:
            return False                     # section 6.3
        self.seed = seed & 0xFFFFFFFF
        self._load_all()
        return True

    def observe_n(self, N):
        """
        N is not a register here; it arrives over the observation sideband, so the
        window keeps a copy and compares. A change restarts every source's
        preparation, because a bank is a permutation of the N it was filled for --
        which is finding 26, and this is its correction.

        >>> w = Window(N=4, seed=3)
        >>> _ = w.observe_n(6)
        >>> w.loads
        [1, 1, 1]
        >>> _ = w.observe_n(6)          # unchanged: no pulse
        >>> w.loads
        [1, 1, 1]
        """
        if N == self.n_q:
            return False
        self.n_q = N
        self.N = N
        for s in self.sources:
            s.N = N
        self._load_all()
        return True

    #--- the mux, and the projection (items 2, 4 and 5) ----------------------

    def tick(self, ts_t):
        """
        One clock: drive the sources, and latch the levels the status bits report.

        >>> w = Window(N=4, seed=1)
        >>> w.tick(0)
        >>> bool(w.status() >> BIT_READY & 1)   # identity: ready after a cycle
        True
        """
        self.ts_t = ts_t
        for s in self.sources:
            s.tick(ts_t)
        self.recovery_q = False             # the recovery is one cycle long
        if not self.ready():
            self.unready_lat = True         # the sticky view, recovery included
        if any(s.underrun_flag for s in self.sources):
            self.underrun_seen = True
            self.under_lat = True

    def pi(self):
        """The mux: the selected source's answer, combinationally. No cycle."""
        return self.selected().pi(self.ts_t)

    def ready(self):
        """
        `SEED_READY`: the selected source's readiness AND the window's recovery
        latch (section 7.1). The window owns the uniform part; a source owns the
        rest -- the affine's derivation, the shuffled source's lead.

        >>> w = Window(N=4, seed=1)
        >>> w.ready()                    # at reset nothing has cleared the latch
        True
        >>> _ = w.write_seed(1, running=False)
        >>> w.ready()                    # a write clears it for one cycle
        False
        >>> w.tick(0)
        >>> w.ready()
        True
        """
        return self.selected().ready() and not self.recovery_q

    def obs_a(self):
        """
        The affine source's `a`, REGARDLESS of the selector, and 0 before it is
        ready. Gating this on the selector would make it useless exactly when a
        driver is deciding whether to select the affine, and reading 0 is how
        "not derived yet" is told from a real value.

        >>> w = Window(N=6, seed=1)
        >>> w.obs_a()
        0
        """
        aff = self.sources[SEL_AFFINE]
        return aff.a if aff.ready() else 0

    def obs_b(self):
        """
        >>> w = Window(N=6, seed=1)
        >>> w.obs_b()
        0
        """
        aff = self.sources[SEL_AFFINE]
        return aff.b if aff.ready() else 0

    def status(self):
        """
        Bit 3 is the SELECTED source's readiness; bit 4 is the OR of all three
        underruns -- a bank was missed somewhere, reported rather than attributed
        to a source the current selector may not even name.

        >>> w = Window(N=4, seed=1)
        >>> bool(w.status() >> BIT_UNDERRUN & 1)
        False
        """
        st = 0
        if self.ready():
            st |= 1 << BIT_READY
        if self.under_lat:
            st |= 1 << BIT_UNDERRUN
        return st

    def clear_status(self, bits):
        """
        RW1C against a LEVEL: clearing a bit whose level is still asserted
        re-asserts it on the next tick, because the latch is set from the level
        and the level has not moved. This is why the sources' flags are levels
        and the stickiness lives here (section 7.3 and 8.4 item 5).

        >>> w = Window(N=4, seed=1)
        >>> w.under_lat = True
        >>> w.clear_status(1 << BIT_UNDERRUN)
        >>> w.under_lat
        False
        """
        if bits >> BIT_UNDERRUN & 1:
            self.under_lat = False
        if bits >> BIT_READY & 1:
            self.unready_lat = False


# ---------------------------------------------------------------------------
# Finding 26, as an experiment rather than an assertion
# ---------------------------------------------------------------------------

def _pass_without_n_observation(N_before, N_after, seed, bank_n_max=256):
    """
    What a pass sees if the window does NOT watch N -- i.e. without item 3.

    A bank is a permutation of the N it was filled for, so a pass over a larger N
    runs off the end of it. Reading past the end is bounded here rather than
    raised, because the point is the *shape* of the answer: a sequence that is not
    a bijection of 0 .. N_after - 1.
    """
    s = ShuffledSource(N_before, seed, bank_n_max=bank_n_max)
    for _ in range(1 + (N_before + 8) * 4):
        if s.ready():
            break
        s.tick()
    s.start_pass()                     # START admitted, as the engine would
    s.N = N_after                      # the core wrote N; no load followed
    bank = s.active if s.active is not None else []
    seen = [bank[t] if t < len(bank) else None for t in range(N_after)]
    return s.ready(), seen, sorted(x for x in seen if x is not None) == \
        list(range(N_after))


def _pass_with_n_observation(N_before, N_after, seed, bank_n_max=256):
    """
    The same with `observe_n` in place, which is what section 8.4 item 3 requires.
    """
    w = Window(N=N_before, seed=seed, bank_n_max=bank_n_max)
    for _ in range(1 + (N_before + 8) * 4):
        w.tick(0)
        if w.ready():
            break
    w.observe_n(N_after)
    r = w.ready()
    return r, None if not r else [], r is False


if __name__ == "__main__":
    import doctest
    import sys
    sys.exit(1 if doctest.testmod().failed else 0)

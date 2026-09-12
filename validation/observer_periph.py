"""
BCMC observer peripheral -- the register window's golden model (v2.0a).

This module is the *golden model* for the decode that
`docs/Observer_Register_Map.md` specifies, in exactly the sense that
`bcmc_periph.py` is the golden model for `rtl/bcmc_wb.v`. It implements the
document, and where the two disagree the document is right and this model is a
bug.

    docs/Observer_Register_Map.md  ->  observer_periph.py  ->  rtl/bcmc_obs_wb.v

What it models, and what it must not
------------------------------------
It models the **window's contract**, not an imagined RTL implementation:

    register state        OBS_CTRL.ONESHOT, the DONE/ABORTED latches, OBS_PASS
    transaction effects   ack/err, and a refused write's total absence of effect
    the trigger mux       OBS_CTRL.STEP is the software source; v2.0a has one
    the reset request     OBS_CTRL.RESET pulses the engine's rst and nothing else
    the status projection RUNNING/DONE/ABORTED, read out of the engine

It must **not** reproduce the engine's state machine. That is
`observer_hw.SequentialEngine`, which stays the sole arbiter of what `START`,
`STEP` and `RESET` do to a pass -- so this file composes one rather than
imitating one. The error condition E4 is *derived* from the engine's own
acceptance rule by asking the engine, not by restating it.

The composition rule
--------------------
    Removing the register window must leave the frozen observer-engine
    behaviour unchanged for the same engine input sequence.

That is checkable, and `replay()` is how: every cycle, `tick()` records the
inputs it gave the engine and the outputs the engine gave back, and
`replay(trace)` feeds the same inputs to a fresh engine. The two must agree
cycle for cycle. A window that quietly gated an input, or massaged an output,
fails that immediately -- which is the point, because the first presumption when
a peripheral test fails should be a defect in the register-map document, not
permission to reopen the engine.

Timekeeping
-----------
Like the engine, this model is edge-driven. A bus access happens *during* a
cycle; `tick()` applies the rising edge that consumes it. So a write in cycle
`k` reaches the engine at the end of cycle `k`, and its effect is visible in
cycle `k + 1` -- the engine's one timing rule (section 5.1 of the architecture
document), carried through the window unchanged.

    >>> p = ObserverPeriph(N=4, valid=True)
    >>> p.write(OBS_CTRL, CTRL_START)      # during cycle 0
    True
    >>> bool(p.read(OBS_STATUS)[1] & ST_RUNNING)   # not yet: the edge is pending
    False
    >>> _ = p.tick()                               # cycle 0 closes
    >>> bool(p.read(OBS_STATUS)[1] & ST_RUNNING)
    True
"""

from observer_hw import IDLE, RUN, Cycle, SequentialEngine, identity_source

__all__ = [
    "OBS_ID", "OBS_VERSION", "OBS_CAPS", "OBS_CTRL", "OBS_STATUS", "OBS_PASS",
    "OBS_TRIG", "OBS_ID_VALUE", "OBS_VERSION_VALUE", "TRIG_SOFTWARE",
    "CTRL_START", "CTRL_STEP", "CTRL_RESET", "CTRL_ONESHOT",
    "ST_RUNNING", "ST_DONE", "ST_ABORTED",
    "ObserverPeriph", "replay",
]

# --- the address map (docs/Observer_Register_Map.md, section 5) -------------

OBS_ID      = 0x000
OBS_VERSION = 0x004
OBS_CAPS    = 0x008
OBS_CTRL    = 0x00C
OBS_STATUS  = 0x010
OBS_PASS    = 0x014
OBS_TRIG    = 0x018

OBS_ID_VALUE      = 0x4F425356          # "OBSV"
OBS_VERSION_VALUE = 0x00000100          # 0.1.0

TRIG_SOFTWARE = 1 << 0
TRIG_TIMER    = 1 << 1                  # v2.0c
TRIG_PIN      = 1 << 2                  # v2.0c

# --- OBS_CTRL ---------------------------------------------------------------

CTRL_START   = 1 << 0                   # W1S
CTRL_STEP    = 1 << 1                   # W1S
CTRL_RESET   = 1 << 2                   # W1S
CTRL_ONESHOT = 1 << 3                   # RW

# --- OBS_STATUS -------------------------------------------------------------

ST_RUNNING = 1 << 0                     # RO
ST_DONE    = 1 << 1                     # RW1C
ST_ABORTED = 1 << 2                     # RW1C

_READ_ONLY = (OBS_ID, OBS_VERSION, OBS_CAPS, OBS_PASS, OBS_TRIG)
_ONE_SHOT_BITS = (CTRL_START, CTRL_STEP, CTRL_RESET)

# The reference build geometry of rtl/bcmc_obs_wb.v, and the geometry this model
# reports in OBS_CAPS: (IDX_W << 24) | (VAL_W << 16) | MAX_C. The geometry is
# baked into what that register answers, so a corpus generated for one geometry
# is not valid for another. These are the defaults of the constructor, so the
# generator and the replayer cannot drift from each other; sim/CMakeLists.txt
# must build rtl/bcmc_obs_wb.v with the same numbers, and the first recording
# reads OBS_CAPS so that a mismatch fails loudly rather than subtly.
#
# They are also rtl/bcmc_obs_wb.v's own default parameters, so that the Icarus
# second opinion -- which elaborates the module without -P -- is held against
# the same geometry as the Verilator path. The document leaves MAX_C free (it is
# a synthesis parameter, reported in OBS_CAPS, not a fixed quantity), so this is
# a choice; it is made once, here, and 32 is the engine's own build geometry.
REF_MAX_C = 32
REF_VAL_W = 16
REF_IDX_W = 16


class ObserverPeriph:
    """
    The v2.0a register window over one observer engine.

    It owns the registers, the decode, the trigger-source mux, the reset request
    and the status projection. It owns nothing about a *pass*: `self.eng` decides
    every one of those questions, and this class asks the engine rather than
    answering on its behalf.

    >>> p = ObserverPeriph(N=4, valid=True)
    >>> p.read(OBS_ID)
    (True, 1329746774)
    >>> p.read(OBS_TRIG)
    (True, 1)
    """

    def __init__(self, N=0, C=0, oneshot=False, valid=False,
                 max_c=REF_MAX_C, val_w=REF_VAL_W, idx_w=REF_IDX_W,
                 source=identity_source):
        if max_c < 1:
            raise ValueError(f"max_c must be >= 1, got {max_c}")

        # --- the observation sideband, as the BCMC peripheral drives it -----
        self.N = N
        self.C = C
        self.valid = valid

        # --- geometry, reported in OBS_CAPS --------------------------------
        self.max_c = max_c
        self.val_w = val_w
        self.idx_w = idx_w

        # --- registers the window owns -------------------------------------
        self.oneshot = bool(oneshot)
        self.done_latch = False
        self.aborted_latch = False
        self.pass_count = 0

        # --- the engine, which owns everything about a pass -----------------
        self.eng = SequentialEngine(N, oneshot=self.oneshot, source=source)

        # --- this cycle's control pulses, from the trigger-source mux -------
        self.start_pulse = False
        self.step_pulse = False
        self.reset_pulse = False

        # --- the composition record (see the module docstring) --------------
        self.in_trace = []
        self.out_trace = []

    # -- projections of the engine -------------------------------------------

    @property
    def running(self):
        """OBS_STATUS.RUNNING is the engine's `running` output, unlatched."""
        return self.eng.state == RUN

    @property
    def advertised_sources(self):
        """OBS_TRIG. v2.0a populates the mux with the software source only."""
        return TRIG_SOFTWARE

    def status(self):
        """OBS_STATUS, read out of the engine and the window's two latches."""
        v = 0
        if self.running:
            v |= ST_RUNNING
        if self.done_latch:
            v |= ST_DONE
        if self.aborted_latch:
            v |= ST_ABORTED
        return v

    # -- decode --------------------------------------------------------------

    def _mapped(self, addr):
        return addr in (OBS_ID, OBS_VERSION, OBS_CAPS, OBS_CTRL, OBS_STATUS,
                        OBS_PASS, OBS_TRIG)

    def _read_value(self, addr):
        if addr == OBS_ID:
            return OBS_ID_VALUE
        if addr == OBS_VERSION:
            return OBS_VERSION_VALUE
        if addr == OBS_CAPS:
            return (((self.idx_w & 0xFF) << 24) | ((self.val_w & 0xFF) << 16)
                    | (self.max_c & 0xFFFF))
        if addr == OBS_CTRL:
            return CTRL_ONESHOT if self.oneshot else 0
        if addr == OBS_STATUS:
            return self.status()
        if addr == OBS_PASS:
            return self.pass_count & 0xFFFFFFFF
        if addr == OBS_TRIG:
            return self.advertised_sources
        raise ValueError(f"unmapped address {addr:#05x}")

    # -- the bus -------------------------------------------------------------

    def read(self, addr, sel=0xF):
        """
        One bus read: `(acked, data)`. A refused read is `(False, 0)` and, like a
        refused write, changes nothing.

        >>> ObserverPeriph(N=4, valid=True).read(0x048)
        (False, 0)
        >>> ObserverPeriph(N=4, valid=True).read(OBS_ID, sel=0x1)
        (False, 0)
        """
        if sel != 0xF:                              # E3
            return (False, 0)
        if not self._mapped(addr):                  # E1
            return (False, 0)
        return (True, self._read_value(addr))

    def start_acceptable(self):
        """
        The acceptance condition for `START`, **asked of the engine** rather than
        restated (section 3.4): IDLE, and VALID, and `N >= 1`. This is what makes
        E4 a derivation instead of a second specification of the engine's rules.

        >>> ObserverPeriph(N=0, valid=True).start_acceptable()
        False
        >>> ObserverPeriph(N=4, valid=False).start_acceptable()
        False
        >>> ObserverPeriph(N=4, valid=True).start_acceptable()
        True
        """
        return (not self.running) and self.valid and self.N >= 1

    def write(self, addr, data, sel=0xF):
        """
        One bus write: `True` for `ack`, `False` for `err`. A refused write has
        **no side effect at all**, which is why every E4 test happens before
        anything is applied.

        >>> p = ObserverPeriph(N=4, valid=True)
        >>> p.write(OBS_CTRL, CTRL_STEP)          # STEP while IDLE: E4
        False
        >>> p.write(OBS_CTRL, CTRL_START)
        True
        >>> _ = p.tick()
        >>> p.write(OBS_CTRL, CTRL_START)         # START while RUNNING: E4
        False
        >>> p.write(OBS_CTRL, CTRL_ONESHOT)       # a mode bit: legal in any state
        True
        >>> p.write(OBS_ID, 0)                    # read-only: E2
        False
        >>> p.write(0x048, 0)                     # unmapped: E1
        False
        """
        if sel != 0xF:                              # E3
            return False
        if not self._mapped(addr):                  # E1
            return False
        if addr in _READ_ONLY:                      # E2
            return False

        if addr == OBS_STATUS:
            # RW1C and nothing else. Reserved bits are ignored, and clearing a
            # bit that is already 0 is legal.
            if data & ST_DONE:
                self.done_latch = False
            if data & ST_ABORTED:
                self.aborted_latch = False
            return True

        # OBS_CTRL. E4 first, because a refused write must not half-apply: if
        # START is refused, an ONESHOT written in the same word is refused too.
        start = bool(data & CTRL_START)
        step = bool(data & CTRL_STEP)
        if start and not self.start_acceptable():   # E4
            return False
        if step and not self.running:               # E4
            return False

        # Accepted: hand the frozen engine its own inputs and nothing else.
        if start:
            self.start_pulse = True
        if step:
            self.step_pulse = True
        if data & CTRL_RESET:
            self.reset_pulse = True
        self.oneshot = bool(data & CTRL_ONESHOT)
        return True

    # -- the rising edge -----------------------------------------------------

    def tick(self):
        """
        Close the current cycle: the engine consumes this cycle's control pulses,
        and the window's latches follow the engine's outputs.

        Returns the engine's outputs for the cycle being closed.

        A one-shot pass is driven here to its end, one `STEP` per visit -- which
        is the only way it *can* end while the software source is the only one in
        the mux (section 4 of the register map).

        >>> p = ObserverPeriph(N=2, valid=True)
        >>> _ = p.write(OBS_CTRL, CTRL_ONESHOT | CTRL_START)
        >>> _ = p.tick()                    # cycle 0: START consumed
        >>> _ = p.write(OBS_CTRL, CTRL_ONESHOT | CTRL_STEP)
        >>> _ = p.tick()                    # cycle 1: pi(0) is presented
        >>> closing = p.tick()              # cycle 2: pi(1), the closing visit
        >>> closing.done
        True
        >>> bool(p.status() & ST_DONE)
        True
        >>> bool(p.status() & ST_RUNNING)   # a one-shot pass is over
        False
        >>> p.pass_count
        1

        Note the `CTRL_ONESHOT | CTRL_STEP` above: a `CTRL` write sets every
        field it has, so a bare `STEP` would have cleared `ONESHOT` and the pass
        would never have ended. That is standard register semantics and it is a
        trap for a driver, so it is stated rather than left implicit.
        """
        rst, start, step, valid = (self.reset_pulse, self.start_pulse,
                                   self.step_pulse, self.valid)
        self.in_trace.append((self.N, self.oneshot, rst, start, step, valid))

        # The engine's outputs *for this cycle*, before the edge changes them.
        outputs = self.eng.outputs(valid)
        was_aborted = self.eng.aborted

        self.eng.N = self.N
        self.eng.oneshot = self.oneshot
        self.eng.edge(start=start, trigger=step, valid=valid, rst=rst)

        self.out_trace.append(outputs)

        # --- the latches, which are projections of the engine's own state ----
        if rst:
            # RESET is the engine's rst row and nothing else: it clears the
            # observer, and only the observer.
            self.done_latch = False
            self.aborted_latch = False
            self.pass_count = 0
        else:
            if outputs.done:
                self.done_latch = True
                self.pass_count = (self.pass_count + 1) & 0xFFFFFFFF
            if self.eng.aborted and not was_aborted:
                self.aborted_latch = True
            if was_aborted and not self.eng.aborted:
                self.aborted_latch = False      # the engine cleared it: a restart

        self.start_pulse = False
        self.step_pulse = False
        self.reset_pulse = False
        return outputs


def replay(trace, source=identity_source):
    """
    Feed a recorded engine input sequence to a **bare** engine and return what it
    produced, cycle by cycle. This is the composition rule of the module
    docstring made executable: a window that gated an input, or massaged an
    output, fails it immediately.

    >>> p = ObserverPeriph(N=2, valid=True)
    >>> _ = p.write(OBS_CTRL, CTRL_START); _ = p.tick()
    >>> _ = p.write(OBS_CTRL, CTRL_STEP);  _ = p.tick()
    >>> replay(p.in_trace) == p.out_trace
    True
    """
    eng = SequentialEngine(0, source=source)
    out = []
    for (N, oneshot, rst, start, step, valid) in trace:
        eng.N = N
        eng.oneshot = oneshot
        out.append(eng.outputs(valid))
        eng.edge(start=start, trigger=step, valid=valid, rst=rst)
    return out


# ---------------------------------------------------------------------------
# Self-check
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    import doctest

    failures, tests = doctest.testmod()
    if failures:
        raise SystemExit(
            f"observer_periph.py: {failures} of {tests} doctests FAILED")
    print(f"observer_periph.py: {tests} doctests passed")

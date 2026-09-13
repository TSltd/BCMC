"""
The v2.0c register window, **composed over** the v2.0a one.

    ObserverPeriphV2C  =  ObserverPeriph  +  observer_window.Window

This module adds and delegates; it reimplements nothing. `super()` answers every
v2.0a question -- the bus, the decode, the error model, the engine, the status
projection -- and the `Window` answers every question about a *traversal source*.
The v2.0c layer itself holds no engine state, computes no traversal answer, and
knows no register semantics that are not additions the register map records
(`docs/Observer_Register_Map.md`, section 5, v2.0c).

Versioned by composition, deliberately
--------------------------------------
`docs/Observer_Register_Map.md` is the normative contract and this file is its
executable form for v2.0c. The alternative -- editing observer_periph.py -- would
mean the v2.0a artifact that generated `obswb_edge.txt` no longer exists to be
compared against, and finding 27's whole discipline depends on it existing. So
the v2.0a model stays frozen and v2.0c is expressed as a layer above it.

The composition is one wire, not an adapter
-------------------------------------------
`observer_hw.SequentialEngine` already takes `source=`: a one-argument callable,
`source(t) -> pi(t)`, where `t` is the step the engine is about to present. So the
window goes behind the existing seam, and the engine stays exactly what it was:

    engine asks t   ->   window.ts_t = t   ->   window.pi()   ->   the answer

with the window clocked on the same edge as the engine's traversal step. Two
details make that faithful rather than approximate, and both were read out of the
models rather than assumed:

* `Window.pi()` is combinational and reads the window's own `ts_t` (section 8.2's
  seam, where `ts_t` is the engine's output and the source's input).
* The sources' cursors advance on **clocks**, not on asks -- `AffineSource.tick`
  moves `r` only when `ts_t` differs from its last value, which is the RTL's
  `moved` detector. So the window is ticked once per cycle with the engine's
  `tq`, and a cycle in which the engine does not advance advances nothing.

That ordering is `traversal_sources.walk_pass`'s -- ask, then clock -- and it is
what makes `pi(t)` an answer about the *current traversal position* rather than a
random-access lookup. **Asking a source out of order is not valid stimulus**; see
the suite, which asserts the stimulus discipline rather than leaving it implicit.

What v2.0c adds
---------------
`OBS_SEED` (0x020, RW), `OBS_A`/`OBS_B` (0x024/0x028, RO), `SELECT` in
`OBS_CTRL[5:4]` (readable), `SEED_READY`/`SEED_UNDERRUN` in `OBS_STATUS[3]`/`[4]`,
and `SEED_READY` as a conjunct of `START`'s acceptance condition. The refusals are
the existing classes: a selector of `3` names no source (**E1**), a write to
`OBS_A`/`OBS_B` is a write to a read-only register (**E2**), and a selector change
or seed write while `RUNNING` is **E4**.

Generic Python 3. No dependency beyond the two models it composes.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from observer_periph import (OBS_CTRL, OBS_STATUS, REF_IDX_W, REF_MAX_C,
                             REF_VAL_W, ObserverPeriph)
from observer_window import CTRL_SEL_SHIFT, SEL_ID, Window

OBS_SEED = 0x020
OBS_A    = 0x024
OBS_B    = 0x028

ST_SEED_READY    = 1 << 3
ST_SEED_UNDERRUN = 1 << 4

__all__ = [
    "OBS_SEED", "OBS_A", "OBS_B", "ST_SEED_READY", "ST_SEED_UNDERRUN",
    "ObserverPeriphV2C",
]


class ObserverPeriphV2C(ObserverPeriph):
    """
    The v2.0c window: the v2.0a one, plus a traversal source behind the seam.

    >>> p = ObserverPeriphV2C(N=4, valid=True)
    >>> p.read(OBS_SEED)
    (True, 0)
    >>> p.read(OBS_A)                      # nothing derived yet
    (True, 0)
    >>> p.write(OBS_A, 1)                  # read-only: E2
    False
    """

    def __init__(self, N=0, C=0, oneshot=False, valid=False,
                 max_c=REF_MAX_C, val_w=REF_VAL_W, idx_w=REF_IDX_W, seed=0):
        # The window exists only where a source can. N >= 1 is the sources'
        # domain (section 3.4); at N < 1 there is no source and nothing to be
        # ready -- and START is refused for N < 1 whatever SEED_READY says.
        self.window = Window(N=N, seed=seed) if N >= 1 else None
        super().__init__(N=N, C=C, oneshot=oneshot, valid=valid, max_c=max_c,
                         val_w=val_w, idx_w=idx_w, source=self._source)

    # -- the composition: the window behind the engine's seam ----------------

    def _source(self, t):
        """
        The ask, and the pass boundary it implies.

        `SequentialEngine.edge` calls this at most once per cycle, and its own
        transition is what makes `t == 0` a precise boundary detector: the IDLE
        branch asks `source(0)` when it accepts a START, and the trigger branch
        passes `0` only when it wraps from `N-1`. So `t == 0` means "a pass is
        starting" and nothing else does -- an ordinary trigger is always `t != 0`.

        | event            | engine passes | source action |
        | ---------------- | ------------- | ------------- |
        | accepted START   | `0`           | `start_pass()` |
        | ordinary trigger | next `t` != 0 | none          |
        | wrap at N-1      | `0`           | `start_pass()` |
        | refused START    | not called    | none          |
        | invalidation     | not called    | none          |

        The boundary is signalled *here*, at the composition layer, because the
        RTL derives it internally -- `rtl/bcmc_src_shuffled.v` has no
        `start_pass` port -- while the Python source model exposes it as an
        explicit method. That is a model-interface asymmetry and nothing more:
        test-model plumbing, not a hardware input. It follows
        `traversal_sources.bind_pass`'s idiom (ready first, then `start_pass()`
        if the source has one) rather than inventing a second one, and the
        precondition agrees with `start_acceptable` by construction, since only
        an accepted START reaches the IDLE branch that asks `source(0)`.
        """
        if self.window is None:
            return t                  # unreachable: START is refused at N < 1
        if t == 0:
            src = self.window.selected()
            if hasattr(src, "start_pass"):
                src.start_pass()
        self.window.ts_t = t
        return self.window.pi()

    def tick(self):
        """
        One clock: the engine's edge, then the window's -- in that order.

        The ask happens *inside* `super().tick()` (the engine's edge calls the
        source), so the cursor must move *after* it, with the step the engine
        just advanced to. Reversing the two would answer every visit with the
        next one's column.
        """
        out = super().tick()
        if self.window is not None:
            self.window.tick(self.eng.tq)
        return out

    # -- the registers v2.0c adds -------------------------------------------

    def _mapped(self, addr):
        return addr in (OBS_SEED, OBS_A, OBS_B) or super()._mapped(addr)

    def _read_value(self, addr):
        if addr == OBS_SEED:
            return self.window.seed if self.window else 0
        if addr == OBS_A:
            return self.window.obs_a() if self.window else 0
        if addr == OBS_B:
            return self.window.obs_b() if self.window else 0
        if addr == OBS_CTRL:
            sel = self.window.sel if self.window else SEL_ID
            return super()._read_value(addr) | (sel << CTRL_SEL_SHIFT)
        if addr == OBS_STATUS:
            extra = self.window.status() if self.window else 0
            return super()._read_value(addr) | extra
        return super()._read_value(addr)

    def seed_ready(self):
        """
        `SEED_READY`: the SELECTED source can serve the current context. At
        N < 1 no source exists, so nothing is ready -- which is also the safe
        reading, since START is refused there anyway.
        """
        return self.window.ready() if self.window is not None else False

    def start_acceptable(self):
        """
        The v2.0a condition, plus `SEED_READY` (section 6.2): a `START` the
        selected source cannot serve is one the engine would ignore.
        """
        return super().start_acceptable() and self.seed_ready()

    def write(self, addr, data, sel=0xF):
        # Everything the v2.0a decode rejects stays rejected, by delegation.
        if sel != 0xF or not self._mapped(addr):
            return super().write(addr, data, sel)

        if addr in (OBS_A, OBS_B):
            return False                                  # E2: read-only

        if addr == OBS_SEED:
            if self.running:
                return False                              # E4: not mid-pass
            if self.window is not None:
                self.window.write_seed(data, running=False)
            return True

        if addr == OBS_CTRL:
            new_sel = (data >> CTRL_SEL_SHIFT) & 0x3
            if new_sel == 3:
                return False                              # E1: names no source
            cur_sel = self.window.sel if self.window else SEL_ID
            if self.running and new_sel != cur_sel:
                return False                              # E4: not mid-pass
            # Any other refusal is the v2.0a one, and a refused write must not
            # half-apply, so this is asked BEFORE the window is told.
            if not super().write(addr, data, sel):
                return False
            if self.window is not None:
                self.window.write_ctrl(data, running=self.running)
            return True

        if addr == OBS_STATUS:
            if not super().write(addr, data, sel):
                return False
            if self.window is not None:
                self.window.clear_status(data)             # RW1C, both layers
            return True

        return super().write(addr, data, sel)

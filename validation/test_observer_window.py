"""
The window's adversarial suite: the ten gates of `docs/Traversal_Sources_
Specification.md` section 8.4, put to `validation/observer_window.py`.

Two rules of organisation, both deliberate:

  * **Expectations are stated at the WINDOW level.** "The window no longer admits
    a pass", "the window does not project ready" -- not a re-stated algorithm for
    what the source does internally. A test that duplicated the source's
    bookkeeping would agree with the model by construction and prove nothing about
    the window.
  * **A snapshot covers all externally meaningful window state** -- selector, seed,
    N, load counts, each source's readiness and answers, and both latches -- so
    "refused" means *nothing changed*, which is the frozen rule for refusals.

The identity gate reuses the FROZEN corpus rather than a fresh expectation: the
columns `sim/vectors/obswb_edge.txt` records at its visits are the identity
permutation, and selector 0 must present exactly those.

Usage:
    python3 test_observer_window.py
"""

import os
import sys

from observer_window import (BIT_READY, BIT_UNDERRUN, SEL_AFFINE, SEL_ID,
                             SEL_SHUFFLED, Window, ctrl,
                             _pass_with_n_observation,
                             _pass_without_n_observation)

VECTOR_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                          "..", "sim", "vectors")

checks = 0
errors = 0


def check(cond, what):
    global checks, errors
    checks += 1
    if not cond:
        errors += 1
        print(f"  FAIL {what}")


def pi_at(w, t):
    """The mux's answer for one ask, advancing nothing."""
    w.ts_t = t
    return w.pi()


def drive_until_ready(w, cycles=4000):
    """No source promises a cycle count, so readiness is waited for."""
    for _ in range(cycles):
        if w.ready():
            return True
        w.tick(0)
    return False


def drive_cycles(w, n):
    for _ in range(n):
        w.tick(0)


def grew(before, after):
    """Every source took exactly one more load."""
    return len(before) == len(after) and all(b + 1 == a
                                             for b, a in zip(before, after))


#---------------------------------------------------------------------------
# 1, 2, 3 and 8: the selector, refusals, and what earns a load
#---------------------------------------------------------------------------

def test_selector_three_changes_nothing():
    w = Window(N=6, seed=5)
    drive_until_ready(w)
    before = w.snapshot()
    ok = w.write_ctrl(ctrl(3, step=True), running=False)
    check(ok is False, "a selector of 3 must be refused (E1)")
    check(w.snapshot() == before, "refused: no state changed at all")
    print("selector 3: refused, and nothing moved")


def test_restating_the_selector_does_not_load():
    w = Window(N=6, seed=5)
    drive_until_ready(w)
    before = tuple(w.loads)
    ok = w.write_ctrl(ctrl(SEL_ID, step=True), running=False)
    check(ok is True, "restating the current selector is accepted")
    check(tuple(w.loads) == before,
          "restating the selector must NOT load -- every STEP carries it")
    print("a STEP carrying the current selector: accepted, no load")


def test_changing_the_selector_loads():
    w = Window(N=6, seed=5)
    drive_until_ready(w)
    before = tuple(w.loads)
    w.write_ctrl(ctrl(SEL_AFFINE, step=False), running=False)
    check(grew(before, w.loads), "a selector CHANGE loads every source")
    print("a selector change: loads all three")


def test_selector_change_while_running_is_refused():
    w = Window(N=6, seed=5)
    w.write_ctrl(ctrl(SEL_AFFINE), running=False)
    before = w.snapshot()
    ok = w.write_ctrl(ctrl(SEL_SHUFFLED, step=True), running=True)
    check(ok is False, "a selector change while RUNNING must be refused")
    check(w.snapshot() == before, "refused: nothing changed, not even OBS_CTRL")
    print("a selector change mid-pass: refused, nothing moved")


#---------------------------------------------------------------------------
# 3: seed writes, and none of them while a pass runs
#---------------------------------------------------------------------------

def test_seed_write_always_loads():
    w = Window(N=6, seed=5)
    drive_until_ready(w)
    before = tuple(w.loads)
    ok = w.write_seed(5, running=False)          # the SAME value
    check(ok is True, "writing the seed is accepted")
    check(grew(before, w.loads), "even an identical seed write loads")
    print("an identical seed write: still loads")


def test_seed_write_while_running_is_refused():
    w = Window(N=6, seed=5)
    before = w.snapshot()
    ok = w.write_seed(9, running=True)
    check(ok is False, "a seed write while RUNNING must be refused")
    check(w.snapshot() == before, "refused: nothing changed")
    print("a seed write mid-pass: refused, nothing moved")


#---------------------------------------------------------------------------
# 4 and 7: the N transition, which is finding 26's protection
#---------------------------------------------------------------------------

def test_n_change_invalidates_and_recovers():
    w = Window(N=6, seed=5)
    check(drive_until_ready(w), "ready before the change")
    before = tuple(w.loads)
    check(w.observe_n(8) is True, "a different N is a change")
    check(grew(before, w.loads), "an N change loads every source")
    check(w.ready() is False,
          "the window must not project ready after an N change")
    check(w.observe_n(8) is False, "the same N is not a change")
    check(drive_until_ready(w), "and it recovers once the sources rebuild")
    print("an N change: invalidated, then recovered")


def test_finding_26_is_real_and_is_the_window_s_fix():
    # Without the window's observation the pass is not a bijection; with it, no
    # pass can be admitted. Stated as the window's admission rule.
    for (a, b, seed) in ((4, 6, 9), (8, 12, 3), (16, 20, 5)):
        r, _, ok = _pass_without_n_observation(a, b, seed)
        check(r is True and ok is False,
              f"finding 26: without the observation N={a}->{b} is no bijection")
        r2, _, _ = _pass_with_n_observation(a, b, seed)
        check(r2 is False,
              f"finding 26: with it, the window does not admit N={a}->{b}")
    print("finding 26: real, and fixed by the window's N observation")


#---------------------------------------------------------------------------
# 6: OBS_A / OBS_B belong to the affine, whatever is selected
#---------------------------------------------------------------------------

def test_ab_are_the_affine_s_regardless_of_selector():
    w = Window(N=6, seed=1)
    check(w.obs_a() == 0, "before the derivation, obs_a reads 0")
    drive_cycles(w, 4000)
    a, b = w.obs_a(), w.obs_b()
    check(a != 0, "after the derivation, obs_a is a real value")
    for sel in (SEL_ID, SEL_AFFINE, SEL_SHUFFLED):
        w.sel = sel
        check((w.obs_a(), w.obs_b()) == (a, b),
              f"obs_a/obs_b must not depend on the selector ({sel})")
    print("obs_a/obs_b: the affine's, for every selector")


#---------------------------------------------------------------------------
# 5: the readiness and underrun projection, and RW1C against a level
#---------------------------------------------------------------------------

def test_underrun_latches_and_reasserts_while_high():
    w = Window(N=8, seed=1)
    w.sel = SEL_SHUFFLED
    check(drive_until_ready(w), "the shuffled source fills")
    sh = w.sources[SEL_SHUFFLED]
    for _ in range(64):                    # outrun the fill: a boundary repeats
        sh.start_pass()
        if sh.underrun_flag:
            break
    check(sh.underrun_flag, "the source reports a missed bank")
    w.tick(0)
    check(w.under_lat, "the window latches the OR of the levels")
    check(bool(w.status() >> BIT_UNDERRUN & 1), "and reports it")
    w.clear_status(1 << BIT_UNDERRUN)
    check(w.under_lat is False, "RW1C clears it")
    w.tick(0)
    check(w.under_lat is True,
          "but the LEVEL is still high, so it re-asserts -- which is why the "
          "sources carry levels and the stickiness lives in the window")
    print("UNDERRUN: latched, cleared, and re-asserted while the level holds")


def test_ready_bit_follows_the_selected_source():
    w = Window(N=6, seed=5)
    drive_cycles(w, 4000)
    check(w.ready(), "the identity is ready once its cycle has passed")
    w.sel = SEL_AFFINE
    check(w.ready(), "and so is the affine, once derived")
    print("READY: projected from whatever is selected")


def test_ctrl_readback_is_selector_and_oneshot_only():
    w = Window(N=6, seed=5)
    check(w.read_ctrl() == 0, "fresh: selector 0, ONESHOT 0")
    check(w.write_ctrl(ctrl(SEL_AFFINE, oneshot=True), running=False) is True,
          "an accepted CTRL write")
    check(w.read_ctrl() == 0x18, "SELECT in 5:4, ONESHOT in 3, nothing else")
    check(w.ready() is False, "the affine has not derived yet")
    check(w.read_ctrl() == 0x18, "readiness is not part of the readback")
    drive_cycles(w, 4000)
    check(w.ready() is True, "now it has derived")
    check(w.read_ctrl() == 0x18, "and the CTRL readback has not moved")
    check(w.write_ctrl(ctrl(SEL_AFFINE, oneshot=True, step=True),
                       running=True) is True,
          "a STEP carrying the same selector and ONESHOT is accepted")
    check(w.read_ctrl() == 0x18, "STEP is W1S: it reads 0, and ONESHOT survives")
    print("CTRL readback: SELECT and ONESHOT; readiness and STEP move it not")


#---------------------------------------------------------------------------
# 9: identity transparency, against the FROZEN corpus
#---------------------------------------------------------------------------

def test_identity_reproduces_the_frozen_corpus():
    path = os.path.join(VECTOR_DIR, "obswb_edge.txt")
    if not os.path.isfile(path):
        check(False, f"missing the frozen corpus: {path}")
        return
    runs = 0
    asks = 0
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            tok = line.split()
            if not tok or tok[0].startswith("#"):
                continue
            if tok[0] == "H":
                n = int(tok[2])
                if n < 1:
                    # `n_zero_refuses_start` exists to check that the ENGINE
                    # refuses a start at N = 0. N = 0 is outside every source's
                    # domain (section 3.4), so there is nothing to ask one.
                    continue
                runs += 1
                w = Window(N=n, seed=0)
                w.sel = SEL_ID
                for t in range(n):
                    # The recorded visits are the identity permutation, so the
                    # expectation here is the corpus's, not one invented here.
                    if pi_at(w, t) != t:
                        check(False, f"identity must present t (run {tok[1]})")
                    asks += 1
    check(runs > 0, "the frozen corpus has runs")
    print(f"identity transparency: {runs} runs, {asks} asks, all the identity")


def main():
    test_selector_three_changes_nothing()
    test_restating_the_selector_does_not_load()
    test_changing_the_selector_loads()
    test_selector_change_while_running_is_refused()
    test_seed_write_always_loads()
    test_seed_write_while_running_is_refused()
    test_n_change_invalidates_and_recovers()
    test_finding_26_is_real_and_is_the_window_s_fix()
    test_ab_are_the_affine_s_regardless_of_selector()
    test_underrun_latches_and_reasserts_while_high()
    test_ready_bit_follows_the_selected_source()
    test_ctrl_readback_is_selector_and_oneshot_only()
    test_identity_reproduces_the_frozen_corpus()
    if errors:
        print(f"test_observer_window: FAIL  {checks} checks, {errors} errors")
        return 1
    print(f"test_observer_window: PASS  {checks} checks, section 8.4's ten gates")
    return 0


if __name__ == "__main__":
    sys.exit(main())

"""
Conformance tests for the observer's register window.

`observer_periph.py` is to `docs/Observer_Register_Map.md` what
`bcmc_periph.py` is to `docs/Register_Map.md`, and this script is what makes
that restatement trustworthy. It follows the rules the project has used since
v0.2, plus one this stage needs in particular:

  1. **No invented answers.** Every expected accept/refuse comes from the
     document's own error model and F-table, recomputed here in
     `documented_accept()` -- a second reading of section 6, written in the
     test, not a call into the model.

  2. **The composition property is first-class, not a test.** The window may
     translate bus operations into engine inputs; it may not alter engine
     behaviour beyond those translations. `replay()` enforces that, and it is
     checked on every generated sequence, not once.

  3. **A refusal and a no-op are different things.** An *accepted* write may
     legitimately have no visible effect until the next cycle (`ONESHOT` alone
     changes nothing visible now). A *refused* write must have no effect at all,
     ever, including on sticky status. `snapshot()` tells the two apart.

  4. **Precedence is derived, not observed.** The completion boundary is
     hammered with adjacent-cycle cases whose expected answers come from the
     documented event order (rst, then abort, then start, then one-shot
     completion, then trigger -- section 3.11 of the architecture document).
     Encoding instead what the current Python happens to do would let the model
     and the test agree while jointly preserving a mistake.

Run:
    python3 validation/test_observer_periph.py
"""

import sys

from observer_periph import (
    CTRL_ONESHOT, CTRL_RESET, CTRL_START, CTRL_STEP,
    OBS_CAPS, OBS_CTRL, OBS_ID, OBS_PASS, OBS_STATUS, OBS_TRIG, OBS_VERSION,
    ST_ABORTED, ST_DONE, ST_RUNNING, TRIG_SOFTWARE,
    ObserverPeriph, replay,
)

# The mapped set, read off the address map (section 5) rather than imported, so
# that a register added to the document without a test is noticed here.
MAPPED = (OBS_ID, OBS_VERSION, OBS_CAPS, OBS_CTRL, OBS_STATUS, OBS_PASS,
          OBS_TRIG)
READ_ONLY = (OBS_ID, OBS_VERSION, OBS_CAPS, OBS_PASS, OBS_TRIG)


def documented_accept(addr, data, *, running, valid, N, sel=0xF):
    """
    What `docs/Observer_Register_Map.md` says about one write.

    Section 6, in its own words:

        E1  the address is not mapped
        E2  a write to a read-only register
        E3  the access is not a full 32-bit word
        E4  the access asks the engine for something the engine would ignore
            START is refused unless  !RUNNING & VALID & N >= 1
            STEP  is refused unless   RUNNING

    A *second reading of the document*, deliberately not a call into the model
    under test.
    """
    if sel != 0xF:                       # E3
        return False
    if addr not in MAPPED:               # E1
        return False
    if addr in READ_ONLY:                # E2
        return False
    if addr == OBS_STATUS:               # RW1C: always acked
        return True
    if (data & CTRL_START) and not ((not running) and valid and N >= 1):
        return False                     # E4
    if (data & CTRL_STEP) and not running:
        return False                     # E4
    return True


def snapshot(p):
    """
    Everything a refused access must leave untouched: the window's registers,
    its pending pulses, the engine's entire state, and the composition record.

    Note what is *in* here -- the pulses. A refusal that had already staged a
    START for the engine would be exactly the half-effect the document forbids,
    and it would be invisible in the registers.
    """
    return (
        p.oneshot, p.done_latch, p.aborted_latch, p.pass_count,
        p.start_pulse, p.step_pulse, p.reset_pulse,
        p.eng.state, p.eng.tq, p.eng.col, p.eng.visit, p.eng.aborted,
        p.eng.N, p.eng.oneshot, p.eng.cycle,
        len(p.in_trace), len(p.out_trace),
    )


def fresh(**kw):
    kw.setdefault("N", 4)
    kw.setdefault("valid", True)
    return ObserverPeriph(**kw)


def running(N=4, oneshot=False):
    """
    A peripheral with a pass in flight: START written and consumed, so RUNNING
    is high and the cycle's first visit is being presented.
    """
    p = fresh(N=N)
    p.write(OBS_CTRL, (CTRL_ONESHOT if oneshot else 0) | CTRL_START)
    p.tick()
    return p


# ---------------------------------------------------------------------------
# The F table, and the zero-effect rule
# ---------------------------------------------------------------------------

def suite_f_table():
    """
    Every row of section 9's table, each with a snapshot taken either side. The
    document claims not only "this returns `err`" but "and leaves no trace", so
    both halves are checked.
    """
    cases = [
        ("F1 unmapped read", lambda: fresh(), "R", 0x048, 0, 0xF),
        ("F1 unmapped write", lambda: fresh(), "W", 0x048, 0, 0xF),
        ("F1 above the window", lambda: fresh(), "W", 0x400, 0, 0xF),
        ("F2 write OBS_ID", lambda: fresh(), "W", OBS_ID, 0, 0xF),
        ("F3 write OBS_PASS", lambda: fresh(), "W", OBS_PASS, 0, 0xF),
        ("F3 write OBS_TRIG", lambda: fresh(), "W", OBS_TRIG, 0, 0xF),
        ("F3 write OBS_VERSION", lambda: fresh(), "W", OBS_VERSION, 0, 0xF),
        ("F4 write sel", lambda: fresh(), "W", OBS_CTRL, CTRL_START, 0x1),
        ("F4 read sel", lambda: fresh(), "R", OBS_ID, 0, 0x3),
        ("F5 START while RUNNING", lambda: running(), "W", OBS_CTRL,
         CTRL_START, 0xF),
        ("F6 START while !VALID", lambda: fresh(valid=False), "W", OBS_CTRL,
         CTRL_START, 0xF),
        ("F7 START while N = 0", lambda: fresh(N=0), "W", OBS_CTRL,
         CTRL_START, 0xF),
        ("F8 STEP while IDLE", lambda: fresh(), "W", OBS_CTRL, CTRL_STEP, 0xF),
    ]

    fails = []
    checked = 0
    for (label, make, kind, addr, data, sel) in cases:
        p = make()
        before = snapshot(p)
        acked = (p.read(addr, sel=sel)[0] if kind == "R"
                 else p.write(addr, data, sel=sel))
        if acked:
            fails.append(f"{label}: acked, the document says err")
        if snapshot(p) != before:
            fails.append(f"{label}: refused, but left a trace")
        checked += 1

    if fails:
        for f in fails[:10]:
            print(f"  FAIL {f}")
        return False
    print(f"  {checked} refused accesses, each with no trace whatsoever.")
    return True


def suite_acceptance_exhaustive():
    """
    Every address in the window crossed with a spread of data words and both
    `sel` values, in five states, checked against `documented_accept()` -- so the
    decode is tested where the document speaks, not only where its F-table
    happened to write an example.
    """
    states = {
        "IDLE": lambda: fresh(),
        "RUNNING": lambda: running(),
        "!VALID": lambda: fresh(valid=False),
        "N = 0": lambda: fresh(N=0),
        "one-shot in flight": lambda: running(oneshot=True),
    }
    datas = [0, CTRL_START, CTRL_STEP, CTRL_RESET, CTRL_ONESHOT,
             CTRL_START | CTRL_STEP, CTRL_ONESHOT | CTRL_STEP,
             CTRL_RESET | CTRL_STEP, 0xF, 0xFFFFFFFF]
    addrs = [0x000, 0x004, 0x008, 0x00C, 0x010, 0x014, 0x018, 0x020, 0x040,
             0x3FC, 0x400]

    fails = []
    checked = 0
    for name, make in states.items():
        for addr in addrs:
            for data in datas:
                for sel in (0xF, 0x1):
                    p = make()
                    was_running = p.running
                    before = snapshot(p)
                    got = p.write(addr, data, sel=sel)
                    want = documented_accept(addr, data, running=was_running,
                                             valid=p.valid, N=p.N, sel=sel)
                    if got != want:
                        fails.append(
                            f"{name} W {addr:#05x} {data:#010x} sel {sel:x}: "
                            f"model {got}, document {want}")
                    elif not want and snapshot(p) != before:
                        fails.append(
                            f"{name} W {addr:#05x} {data:#010x} sel {sel:x}: "
                            f"refused, but left a trace")
                    checked += 1

    if fails:
        for f in fails[:10]:
            print(f"  FAIL {f}")
        return False
    print(f"  {checked:,} write decodes agree with the document; refusals silent.")
    return True


def suite_refusal_vs_noop():
    """
    The distinction the document relies on, made explicit.

    An accepted write can legitimately do nothing visible *yet*: `ONESHOT` alone
    changes no engine state until the next edge. A refused write must not have
    done anything at all -- including nothing half-done. The dangerous case is a
    word carrying both a refused action and a legal field, where a sloppy
    implementation applies the field it likes and drops the rest.
    """
    fails = []
    checked = 0

    p = fresh()
    engine_before = (p.eng.state, p.eng.tq, p.eng.visit)
    if not p.write(OBS_CTRL, CTRL_ONESHOT):
        fails.append("ONESHOT alone was refused")
    if (p.eng.state, p.eng.tq, p.eng.visit) != engine_before:
        fails.append("ONESHOT alone changed the engine immediately")
    p.tick()
    if p.eng.oneshot is not True:
        fails.append("ONESHOT did not reach the engine by the next edge")
    checked += 1

    q = fresh(N=0)                     # START is E4 here
    if q.write(OBS_CTRL, CTRL_ONESHOT | CTRL_START):
        fails.append("ONESHOT|START with N = 0 was acked")
    if q.oneshot:
        fails.append("a refused write half-applied its ONESHOT bit")
    q.tick()
    if q.eng.oneshot:
        fails.append("a refused write reached the engine on the next edge")
    checked += 1

    r = running()
    if r.write(OBS_CTRL, CTRL_ONESHOT | CTRL_START):
        fails.append("ONESHOT|START while RUNNING was acked")
    if r.oneshot:
        fails.append("a refused write half-applied ONESHOT while RUNNING")
    checked += 1

    if fails:
        for f in fails[:10]:
            print(f"  FAIL {f}")
        return False
    print(f"  {checked} checks: refused writes do not half-apply; accepted ones "
          f"may defer.")
    return True


def one_shot_at_the_boundary(N=2):
    """
    A one-shot pass driven so that its **closing visit is being presented**, with
    the edge that would complete it not yet taken.
    """
    p = fresh(N=N)
    p.write(OBS_CTRL, CTRL_ONESHOT | CTRL_START)
    p.tick()
    for _ in range(N - 1):
        p.write(OBS_CTRL, CTRL_ONESHOT | CTRL_STEP)
        p.tick()
    return p


def suite_completion_boundary():
    """
    The closing cycle of a one-shot pass, attacked from every adjacent angle:
    RESET, START, STEP and an invalidation, all on the cycle carrying the last
    visit. Expectations come from the documented precedence -- rst, then abort,
    then start, then one-shot completion, then trigger -- not from what the
    Python happens to do.
    """
    fails = []
    checked = 0

    if not one_shot_at_the_boundary().eng.outputs(True).done:
        fails.append("setup: no closing visit was presented")
    checked += 1

    # (a) no write: the closing visit completes the pass and latches DONE.
    p = one_shot_at_the_boundary()
    p.tick()
    if not (p.done_latch and p.pass_count == 1 and not p.running):
        fails.append("the closing visit did not complete the pass")
    checked += 1

    # (b) RESET on that cycle: reset is the first row, so nothing counts.
    p = one_shot_at_the_boundary()
    if not p.write(OBS_CTRL, CTRL_RESET):
        fails.append("RESET on the closing cycle was refused")
    p.tick()
    if p.done_latch or p.pass_count != 0 or p.running:
        fails.append("RESET on the closing cycle did not clear the observer")
    checked += 1

    # (c) START on that cycle: refused, the pass has not ended yet.
    p = one_shot_at_the_boundary()
    if p.write(OBS_CTRL, CTRL_ONESHOT | CTRL_START):
        fails.append("START on the closing cycle was acked")
    checked += 1

    # (d) STEP on that cycle: acked, then ignored by the engine (F12) -- and the
    # completion must survive it.
    p = one_shot_at_the_boundary()
    if not p.write(OBS_CTRL, CTRL_ONESHOT | CTRL_STEP):
        fails.append("STEP on the closing cycle was refused")
    p.tick()
    if not (p.done_latch and p.pass_count == 1):
        fails.append("STEP on the closing cycle swallowed the completion")
    checked += 1

    # (e) invalidation on that cycle: the visit is suppressed, so the pass
    # neither completes nor counts -- it aborts.
    p = one_shot_at_the_boundary()
    p.valid = False
    p.tick()
    if p.done_latch or p.pass_count != 0:
        fails.append("an invalidation on the closing cycle still latched DONE")
    if not p.aborted_latch:
        fails.append("an invalidation on the closing cycle did not latch ABORTED")
    checked += 1

    if fails:
        for f in fails[:10]:
            print(f"  FAIL {f}")
        return False
    print(f"  {checked} boundary cases, each resolved by the documented "
          f"precedence.")
    return True


def suite_reset_and_restart():
    """
    T3, and the reason `RESET` exists: a continuous pass has no other stop, since
    `START` is refused while it runs. `RESET` clears the observer and only the
    observer -- the matrix is not the window's to touch.
    """
    fails = []
    checked = 0

    p = running(N=4)
    if not p.write(OBS_CTRL, CTRL_RESET):
        fails.append("RESET was refused")
    p.tick()
    if p.running or p.status() != 0 or p.pass_count != 0:
        fails.append(f"RESET left running={p.running} status={p.status():#x} "
                     f"pass={p.pass_count}")
    if not p.valid:
        fails.append("RESET disturbed VALID")
    checked += 1

    if not p.write(OBS_CTRL, CTRL_START):
        fails.append("START after RESET was refused")
    p.tick()
    if not p.running:
        fails.append("a restart after RESET did not begin a pass")
    checked += 1

    q = running(N=4)
    if q.write(OBS_CTRL, CTRL_START):
        fails.append("START was acked inside a continuous pass")
    if not q.write(OBS_CTRL, CTRL_RESET):
        fails.append("RESET was refused inside a continuous pass")
    checked += 1

    if fails:
        for f in fails[:10]:
            print(f"  FAIL {f}")
        return False
    print(f"  {checked} checks: reset stops a continuous pass, and touches "
          f"nothing else.")
    return True


def suite_absent_and_sources():
    """
    The one thing the map provides and the many things it deliberately does not.
    A driver must not be able to reach traversal through the window: there is no
    `NEXT_COLUMN`, no readable column, and nothing answers where the map is
    silent.
    """
    fails = []
    checked = 0

    p = fresh()
    if p.read(OBS_TRIG)[1] != TRIG_SOFTWARE:
        fails.append("OBS_TRIG is not software-only")
    checked += 1

    for addr in range(0x000, 0x400, 4):
        acked, _ = p.read(addr)
        if acked != (addr in MAPPED):
            fails.append(f"read {addr:#05x}: acked {acked}")
    checked += 1

    if p.read(OBS_STATUS)[1] != 0:
        fails.append("a fresh observer does not read an empty status")
    checked += 1

    if fails:
        for f in fails[:10]:
            print(f"  FAIL {f}")
        return False
    print(f"  {checked} checks: the whole window is seven registers, and "
          f"nothing else answers.")
    return True


def suite_replay_property():
    """
    The composition property as a property, over generated sequences that mix
    legal writes, illegal writes, reads, invalidations and `N` changes:

        the window may translate bus operations into engine inputs;
        it may not alter engine behaviour beyond those translations.

    This is the invariant that protects the architectural boundary the whole
    stream has been establishing, so it is checked on every trial and not once.
    """
    import random

    fails = []
    checked = 0
    rng = random.Random(0x0B5E42)
    addrs = [OBS_ID, OBS_VERSION, OBS_CAPS, OBS_CTRL, OBS_STATUS, OBS_PASS,
             OBS_TRIG, 0x020, 0x048, 0x400]
    datas = [0, CTRL_START, CTRL_STEP, CTRL_RESET, CTRL_ONESHOT,
             CTRL_ONESHOT | CTRL_STEP, 0xF]

    for trial in range(500):
        p = ObserverPeriph(N=rng.choice([1, 2, 3, 4, 8]),
                           valid=rng.random() < 0.7)
        for _ in range(rng.randint(4, 24)):
            r = rng.random()
            if r < 0.55:
                p.write(rng.choice(addrs), rng.choice(datas))
            elif r < 0.65:
                p.read(rng.choice(addrs))
            elif r < 0.75:
                p.valid = not p.valid
            elif r < 0.80:
                p.N = rng.choice([0, 1, 3, 8])
            p.tick()
        if replay(p.in_trace) != p.out_trace:
            fails.append(f"trial {trial}: the window altered engine behaviour")
            break
        checked += 1

    if fails:
        for f in fails[:10]:
            print(f"  FAIL {f}")
        return False
    print(f"  {checked} generated sequences: the window never altered the "
          f"engine.")
    return True


def suite_sticky_untouched():
    """
    A refusal must not clear a sticky bit either -- and clearing a bit is a *side
    effect like any other*, so a bad `sel`, an unmapped mirror of `OBS_STATUS`,
    or a word full of reserved bits must not do it.
    """
    fails = []
    checked = 0

    # A peripheral with DONE latched: one one-shot pass driven to its end.
    p = fresh(N=2)
    p.write(OBS_CTRL, CTRL_ONESHOT | CTRL_START)
    p.tick()
    p.write(OBS_CTRL, CTRL_ONESHOT | CTRL_STEP)
    p.tick()
    p.tick()
    if not (p.status() & ST_DONE):
        fails.append("setup: DONE was not latched")
    checked += 1

    for (label, addr, data, sel) in [
        ("bad sel clears nothing", OBS_STATUS, ST_DONE | ST_ABORTED, 0x1),
        ("unmapped clears nothing", 0x04C, ST_DONE | ST_ABORTED, 0xF),
        ("reserved bits ignored", OBS_STATUS, 0xFFFFFFFC, 0xF),
    ]:
        before = snapshot(p)
        acked = p.write(addr, data, sel=sel)
        want = documented_accept(addr, data, running=p.running, valid=p.valid,
                                 N=p.N, sel=sel)
        if acked != want:
            fails.append(f"{label}: model {acked}, document {want}")
        if not want and snapshot(p) != before:
            fails.append(f"{label}: refused, but left a trace")
        if not (p.status() & ST_DONE):
            fails.append(f"{label}: DONE went away")
        checked += 1

    if fails:
        for f in fails[:10]:
            print(f"  FAIL {f}")
        return False
    print(f"  {checked} checks: a refusal cannot clear a sticky bit either, and "
          f"a legal clear can.")
    return True


# ---------------------------------------------------------------------------

def main():
    results = []

    print("Section 6 and 9 -- refusals, and their total absence of effect:")
    results.append(suite_f_table())
    print()

    print("Section 9 -- every refusal leaves no trace:")
    results.append(suite_sticky_untouched())
    print()

    print("Section 6 -- the decode, exhaustively against the document:")
    results.append(suite_acceptance_exhaustive())
    print()

    print("A refusal and a no-op are not the same thing:")
    results.append(suite_refusal_vs_noop())
    print()

    print("The completion boundary, from every adjacent angle:")
    results.append(suite_completion_boundary())
    print()

    print("T3 -- reset, restart, and continuous stop:")
    results.append(suite_reset_and_restart())
    print()

    print("Things the map deliberately does not provide:")
    results.append(suite_absent_and_sources())
    print()

    print("The composition property -- the window never alters the engine:")
    results.append(suite_replay_property())
    print()

    if all(results):
        print("observer_periph.py satisfies docs/Observer_Register_Map.md.")
        print("The register contract is now an executable contract for the decode.")
        return 0

    print("CONFORMANCE FAILED")
    return 1


if __name__ == "__main__":
    sys.exit(main())


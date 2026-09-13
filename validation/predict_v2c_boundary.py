"""
The v2.0c prediction: run the v2.0c model against the **frozen** corpus.

This is finding 27's proof obligation, executed before any v2.0c corpus exists.
It drives `sim/vectors/obswb_edge.txt` -- unmodified, and never regenerated --
through `ObserverPeriphV2C` using the *existing* replayer, and requires the set of
disagreements to be exactly the documented register extension:

    Reads: at most 12 at OBS_STATUS (0x010), each frozen -> frozen | 0x8, and 4 at
    OBS_CTRL (0x00C), each unchanged. Every other observation: unchanged.

Differences are **classified, not counted**. Each one is checked for its address,
the frozen expected value, the v2.0c value, and the bit delta; a count-only
assertion could accept the wrong twelve observations. The `OBS_CTRL` reads are
required to be *present and equal*, rather than merely absent from a difference
list, so "the frozen value is unchanged" is actually verified.

A failure here does not mean the corpus needs updating. It means the composition
or the model contract disagrees with the register map, and that gets classified.

Usage:
    python3 predict_v2c_boundary.py
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from observer_periph_v2c import (OBS_A, OBS_B, OBS_SEED, ObserverPeriphV2C)
from replay_observer_wb import F_ADR, parse, replay

VECTOR_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                          "..", "sim", "vectors")

OBS_CTRL = 0x00C
OBS_STATUS = 0x010

# The documented extension: bit 3 SEED_READY, and bit 4 SEED_UNDERRUN where the
# source reports a missed bank. Nothing else may move.
ST_READY = 1 << 3
ST_UNDERRUN = 1 << 4
EXPECT_STATUS_DIFFS = 12
EXPECT_CTRL_READS = 4


def main():
    path = os.path.join(VECTOR_DIR, "obswb_edge.txt")
    if not os.path.isfile(path):
        print(f"FAIL  no such file: {path}")
        return 1

    seen = []                       # every rdata comparison: (addr, frozen, v2c)

    def on_rdata(name, cycle, addr, frozen, model_value, is_read):
        # Only a READ is an observation of the readback contract. A write's
        # response carries rdata = 0, and counting those is what made "4 OBS_CTRL
        # reads" read as 20 the first time this check ran.
        if is_read:
            seen.append((addr, frozen, model_value))

    fails, checks, cycles, runs = replay(path, model=ObserverPeriphV2C,
                                         on_rdata=on_rdata)

    # The replayer's non-rdata failures are not "differences to classify": they
    # are the bus shape, the engine's fields, or an ack/err disagreement, and
    # none of those is part of the documented extension.
    other = [f for f in fails if "rdata is" not in f]

    per_addr = {}
    for addr, frozen, got in seen:
        per_addr.setdefault(addr, []).append((frozen, got))

    diffs = [(a, f, g) for (a, f, g) in seen if f != g]
    by_addr = {}
    for addr, frozen, got in diffs:
        by_addr.setdefault(addr, []).append((frozen, got))

    problems = []

    # 1. the difference set, by address
    for addr in sorted(by_addr):
        if addr != OBS_STATUS:
            problems.append(f"a difference at {addr:#05x}, which is not the "
                            f"documented extension: "
                            f"{[(hex(f), hex(g)) for f, g in by_addr[addr]]}")

    status_diffs = by_addr.get(OBS_STATUS, [])
    if len(status_diffs) > EXPECT_STATUS_DIFFS:
        problems.append(f"{len(status_diffs)} OBS_STATUS differences, of which "
                        f"at most {EXPECT_STATUS_DIFFS} can be admissible")
    for frozen, got in status_diffs:
        # The documented extension and nothing else: the acknowledged response
        # gains bit 3, and bit 4 where the underrun level is present. The frozen
        # value is whatever the RESPONSE row carries -- it is not assumed to be
        # 0, and it is not (the twelve are 0x0, 0x1, 0x2 and 0x4).
        if got not in (frozen | ST_READY, frozen | ST_READY | ST_UNDERRUN):
            problems.append(f"OBS_STATUS moved {frozen:#x} -> {got:#x}, a delta "
                            f"of {got ^ frozen:#x}, which is not the documented "
                            f"SEED_READY extension")
    # A STATUS read is NOT required to differ: bit 3 is a level, and a read landing
    # inside the one-cycle recovery after a write correctly sees it low, which is
    # bit-for-bit the frozen value. So the claim is "these twelve may differ, and
    # only in this way" -- not "these twelve must differ".

    # 2. the OBS_CTRL reads must be present AND unchanged -- not merely absent
    #    from the difference list, which an empty list would satisfy too.
    ctrl_reads = per_addr.get(OBS_CTRL, [])
    if len(ctrl_reads) != EXPECT_CTRL_READS:
        problems.append(f"{len(ctrl_reads)} OBS_CTRL reads observed, expected "
                        f"{EXPECT_CTRL_READS}")
    for frozen, got in ctrl_reads:
        # Readable SELECT is a v2.0c addition, and the frozen cases select the
        # identity, so bits 5:4 read 0 and the value must be UNCHANGED. The
        # frozen value is the response row's, whatever it is -- and it is not
        # always 0: three of the four expect ONESHOT set.
        if frozen != got:
            problems.append(f"an OBS_CTRL read moved {frozen:#x} -> {got:#x}")

    # 3. nothing else may fail, and the new registers must not be touched by a
    #    corpus that predates them.
    problems.extend(other)
    for addr, reg in ((OBS_SEED, "OBS_SEED"), (OBS_A, "OBS_A"),
                      (OBS_B, "OBS_B")):
        if addr in per_addr:
            problems.append(f"the frozen corpus accesses {reg}")

    print(f"predict_v2c_boundary: {runs} runs, {cycles} cycles, {checks} "
          f"comparisons, {len(seen)} read comparisons")
    print(f"  differences: {len(diffs)} "
          f"({', '.join(f'{a:#05x}: {len(v)}' for a, v in sorted(by_addr.items()))}"
          f"{' none' if not diffs else ''})")
    for frozen, got in status_diffs[:4]:
        print(f"    OBS_STATUS {frozen:#x} -> {got:#x}  (delta {got ^ frozen:#x})")
    if len(status_diffs) > 4:
        print(f"    ... and {len(status_diffs) - 4} more, all the same shape")
    for frozen, got in ctrl_reads:
        print(f"    OBS_CTRL   {frozen:#x} -> {got:#x}  (unchanged)")

    if problems:
        print("predict_v2c_boundary: FAIL -- the boundary is not the documented "
              "one; classify before generating anything")
        for p in problems[:12]:
            print(f"  {p}")
        return 1

    print("predict_v2c_boundary: PASS -- the entire frozen v2.0a peripheral "
          "trace composes through the new layer, and the ONLY predicted "
          "incompatibility is the documented OBS_STATUS extension.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

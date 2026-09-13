"""
The v2.0c prediction: run the v2.0c model against the **frozen** corpus.

This is finding 27's proof obligation, executed before any v2.0c corpus exists.
It drives `sim/vectors/obswb_edge.txt` -- unmodified, and never regenerated --
through `ObserverPeriphV2C` using the *existing* replayer, and requires the set of
disagreements to be exactly the documented register extension:

    12 reads at OBS_STATUS (0x010): 0 -> 0x8, or 0x18 with the underrun level
     4 reads at OBS_CTRL   (0x00C): unchanged, and specifically still 0
    every other observation:       unchanged

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

# The documented extension: bit 3 SEED_READY, bit 4 SEED_UNDERRUN.
ALLOWED_DELTAS = (1 << 3, (1 << 3) | (1 << 4))
EXPECT_STATUS_DIFFS = 12
EXPECT_CTRL_READS = 4


def main():
    path = os.path.join(VECTOR_DIR, "obswb_edge.txt")
    if not os.path.isfile(path):
        print(f"FAIL  no such file: {path}")
        return 1

    seen = []                       # every rdata comparison: (addr, frozen, v2c)

    def on_rdata(name, cycle, addr, frozen, model_value):
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
    if len(status_diffs) != EXPECT_STATUS_DIFFS:
        problems.append(f"{len(status_diffs)} OBS_STATUS differences, expected "
                        f"{EXPECT_STATUS_DIFFS}")
    for frozen, got in status_diffs:
        if frozen != 0:
            problems.append(f"an OBS_STATUS read expected {frozen:#x}, not 0")
        if (got ^ frozen) not in ALLOWED_DELTAS:
            problems.append(f"OBS_STATUS moved {frozen:#x} -> {got:#x}, a delta "
                            f"of {got ^ frozen:#x}, which is not SEED_READY")

    # 2. the OBS_CTRL reads must be present AND unchanged -- not merely absent
    #    from the difference list, which an empty list would satisfy too.
    ctrl_reads = per_addr.get(OBS_CTRL, [])
    if len(ctrl_reads) != EXPECT_CTRL_READS:
        problems.append(f"{len(ctrl_reads)} OBS_CTRL reads observed, expected "
                        f"{EXPECT_CTRL_READS}")
    for frozen, got in ctrl_reads:
        if frozen != got:
            problems.append(f"an OBS_CTRL read moved {frozen:#x} -> {got:#x}")
        if frozen != 0:
            problems.append(f"an OBS_CTRL read expects {frozen:#x}, but the "
                            f"frozen cases select identity and leave ONESHOT "
                            f"clear, so it must be 0")

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

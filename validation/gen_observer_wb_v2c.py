"""
Generate `sim/vectors/obswb_v2c_edge.txt` as a TRANSCRIPTION of a proven boundary.

What the boundary is, and what it is not
----------------------------------------
`sim/vectors/obswb_edge.txt` is the oracle for **v2.0a** behaviour and stays
byte-identical. Findings 27 and 28 have established the only thing v2.0c may
change in it: an acknowledged `OBS_STATUS` read gains bit 3 (`SEED_READY`), and
bit 4 where the underrun level is present --

    STATUS_v2c = STATUS_v2a | SEED_READY

**"Eleven differences" is not the contract.** Eleven is simply what this
particular stimulus happens to observe: one of the twelve `OBS_STATUS` reads lands
inside the one-cycle recovery after a write, where bit 3 is correctly low, and so
reads bit-for-bit the frozen value. The contract is the transformation above,
subject to the level semantics; a generator that asserted "eleven" would be
encoding an accident.

So this does not invent stimuli, and does not regenerate the v2.0a sequence. It
reads the frozen corpus, drives the **validated** v2c model over the same rows, and
rewrites ONLY the acknowledged read data the model disagrees with, leaving every
other byte of every line alone. That is what keeps the request sequence and the
cycle structure provably identical.

The result is then checked by `diff_v2c_boundary.py` -- which is independent of this
file and can fail -- and replayed through the model before any RTL is touched.

Usage:
    python3 gen_observer_wb_v2c.py
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from observer_periph_v2c import ObserverPeriphV2C
from replay_observer_wb import replay

VECTOR_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                          "..", "sim", "vectors")

SRC_NAME = "obswb_edge.txt"
DST_NAME = "obswb_v2c_edge.txt"

# field 10 of a C record is `rdata`; with the leading tag that is token index 11
RDATA_TOKEN = 11


def main():
    src = os.path.join(VECTOR_DIR, SRC_NAME)
    dst = os.path.join(VECTOR_DIR, DST_NAME)
    if not os.path.isfile(src):
        print(f"FAIL  no such file: {src}")
        return 1

    # 1. drive the validated model over the frozen rows, and collect exactly the
    #    read data it does not agree with. Anything else disagreeing is not a
    #    transcription problem -- it is the boundary being wrong, and the gate
    #    that proves otherwise is predict_v2c_boundary.py.
    patches = {}

    def on_rdata(name, cycle, addr, frozen, model_value, is_read):
        if is_read and model_value != frozen:
            patches[(name, cycle)] = (addr, frozen, model_value)

    fails, checks, cycles, runs = replay(src, model=ObserverPeriphV2C,
                                         on_rdata=on_rdata)
    # An acknowledged read whose data the model disagrees with is NOT a failure
    # here: that difference IS the transcription, and its shape is enforced by
    # diff_v2c_boundary.py, which is independent of this file. Anything else
    # disagreeing -- an ack, an err, an engine observable -- means the model and
    # the frozen corpus have parted company where the boundary does not reach,
    # and the gate that proves otherwise is predict_v2c_boundary.py.
    other = [f for f in fails if "rdata is" not in f]
    if other:
        print(f"FAIL  the model disagrees with the frozen corpus in ways the "
              f"boundary does not allow ({len(other)}):")
        for f in other[:8]:
            print(f"  {f}")
        return 1

    # 2. transcribe. Line by line, so headers, comments, spacing and every
    #    unpatched field survive exactly as the frozen corpus wrote them.
    out = []
    run_name = None
    idx = 0
    patched = 0
    with open(src, encoding="utf-8") as fh:
        for raw in fh:
            tok = raw.split()
            if tok and tok[0] == "H":
                run_name = tok[1]
                idx = 0
                out.append(raw)
                continue
            if tok and tok[0] == "C":
                hit = patches.get((run_name, idx))
                if hit is None:
                    out.append(raw)
                else:
                    addr, frozen, new = hit
                    body = raw.split()
                    if int(body[RDATA_TOKEN], 16) != frozen:
                        print(f"FAIL  {run_name} row {idx}: the file's rdata is "
                              f"not what replay() reported")
                        return 1
                    body[RDATA_TOKEN] = format(new, "x")
                    out.append(" ".join(body) + "\n")
                    patched += 1
                idx += 1
                continue
            out.append(raw)

    if patched != len(patches):
        print(f"FAIL  collected {len(patches)} patches but applied {patched}")
        return 1

    with open(dst, "w", encoding="utf-8") as fh:
        fh.writelines(out)

    print(f"gen_observer_wb_v2c: wrote {os.path.relpath(dst)}")
    print(f"  {runs} runs, {cycles} cycles, {checks} comparisons")
    print(f"  {patched} transcribed read-data values, {len(out)} lines written")
    print(f"  every other byte is the frozen corpus's. Check it independently:")
    print(f"    python3 diff_v2c_boundary.py")
    return 0


if __name__ == "__main__":
    sys.exit(main())

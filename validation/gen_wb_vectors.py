"""
Generate bus transaction vectors from the executable specification.

This is to `bcmc_periph.py` what `gen_vectors.py` is to `reference.py`: it
drives the model, records what happened, and writes it down. Nothing here
decides what the right answer is.

    docs/Register_Map.md  ->  bcmc_periph.py  ->  wb_*.txt  ->  rtl/bcmc_wb.v

The recorded conversations are the ones docs/Transaction_Sequences.md
specifies -- S1 to S8 and F1 to F11 -- plus a randomised soak in which whole
matrices are read back through the bus a second time.

Why record instead of assert
----------------------------
`test_periph.py` already asserts that the model obeys the register map. The
question this file answers is a different one: does the *hardware* behave like
the model, access for access. A recording is the only artefact that can be
replayed against both a Verilator harness and an Icarus testbench, and the only
one that guarantees the two are asking the same question.

The one thing that cannot be recorded
-------------------------------------
Time. The model has no clock: a transform completes between one access and the
next, so a STATUS read recorded straight after START would carry a value the
RTL does not reach for another sixty cycles. Software does not read STATUS
once, it polls, and it is the polling that both sides share. So a wait is
recorded as a `P` op and nothing else assumes when completion happens.

    python3 gen_wb_vectors.py            # the two standard files
    python3 gen_wb_vectors.py --out DIR  # write somewhere else
"""

import argparse
import os
import random

from bcmc_periph import (
    COLUMN_BASE,
    CTRL_IRQ_EN,
    CTRL_START,
    OFFSET_BASE,
    REG_C,
    REG_CAPS,
    REG_CELL,
    REG_CELL_COL,
    REG_CELL_ROW,
    REG_CTRL,
    REG_ID,
    REG_N,
    REG_STATUS,
    REG_VERSION,
    STATUS_IRQ,
    STATUS_VALID,
    WEIGHT_BASE,
    BcmcPeripheral,
    BusError,
)

# The reference build of rtl/bcmc_wb.v. The geometry is baked into the
# addresses a recording contains -- F2 lives at WEIGHT_BASE + 4*MAX_C -- so a
# file generated for one geometry is not valid for another. Every recording
# therefore begins by reading CAPS, which fails loudly rather than subtly if a
# harness ever builds the RTL with different parameters.
MAX_C = 64
VAL_W = 16
IDX_W = 16


class Recorder:
    """
    A peripheral that writes down what was asked of it and what it answered.

    Every method mirrors one in `BcmcPeripheral`, so a sequence written against
    the model reads the same here; the only difference is that an access which
    the model refuses is recorded rather than raised, because a refusal is an
    expected outcome half the time.
    """

    def __init__(self, dev):
        self.dev = dev
        self.lines = []
        self.n_ops = 0

    # -- emitting ---------------------------------------------------------

    def _emit(self, op, addr, sel, data, exp, rdata):
        self.lines.append(
            "%-2s %03X %X %08X %-3s %08X" % (op, addr, sel, data, exp, rdata)
        )
        self.n_ops += 1

    def label(self, text):
        self.lines.append("")
        self.lines.append("L " + text)

    def comment(self, text):
        self.lines.append("# " + text)

    # -- the bus ----------------------------------------------------------

    def reset(self):
        self.dev.reset()
        self.lines.append("Z")
        self.n_ops += 1

    def read(self, addr, sel=0xF):
        try:
            value = self.dev.read(addr, sel)
        except BusError:
            self._emit("R", addr, sel, 0, "ERR", 0)
            return None
        self._emit("R", addr, sel, 0, "ACK", value)
        return value

    def write(self, addr, data, sel=0xF):
        try:
            self.dev.write(addr, data, sel)
        except BusError:
            self._emit("W", addr, sel, data, "ERR", 0)
            return False
        self._emit("W", addr, sel, data, "ACK", 0)
        return True

    def poll(self, addr, mask, value):
        """
        Software's wait loop: read `addr` until (read & mask) == value.

        The model reaches the condition in at most one access; the hardware
        takes as long as it takes. Only the condition is recorded.
        """
        for _ in range(1000):
            if (self.dev.read(addr) & mask) == value:
                break
        else:
            raise RuntimeError("the model never reached the polled condition")
        self._emit("P", addr, 0xF, mask, "ACK", value)

    # -- convenience, mirroring bcmc_periph's client helpers ---------------

    def program(self, n, weights, irq=False):
        self.write(REG_N, n)
        self.write(REG_C, len(weights))
        for i, w in enumerate(weights):
            self.write(WEIGHT_BASE + 4 * i, w)
        self.write(REG_CTRL, CTRL_START | (CTRL_IRQ_EN if irq else 0))
        self.poll(REG_STATUS, STATUS_VALID, STATUS_VALID)


def write_file(path, title, rec):
    with open(path, "w") as f:
        f.write("# %s\n" % title)
        f.write("#\n")
        f.write("# Generated by validation/gen_wb_vectors.py from\n")
        f.write("# validation/bcmc_periph.py. Do not edit: regenerate.\n")
        f.write("#\n")
        f.write("# Geometry: MAX_C = %d, VAL_W = %d, IDX_W = %d\n" % (MAX_C, VAL_W, IDX_W))
        f.write("#\n")
        f.write("# op adr sel data     exp rdata\n")
        for line in rec.lines:
            f.write(line.rstrip() + "\n")
    print("%-28s %6d ops" % (os.path.basename(path), rec.n_ops))


#---------------------------------------------------------------------------
# The specified sequences
#---------------------------------------------------------------------------


def record_sequences():
    dev = BcmcPeripheral(max_c=MAX_C, val_w=VAL_W, idx_w=IDX_W)
    r = Recorder(dev)
    r.reset()

    # -- S1 Probe ---------------------------------------------------------
    r.label("S1 Probe")
    r.read(REG_ID)
    r.read(REG_VERSION)
    r.read(REG_CAPS)
    r.read(REG_STATUS)

    # -- F10, F11: nothing has been computed yet --------------------------
    r.label("F10, F11 -- the matrix does not exist yet")
    r.read(REG_CELL)
    r.read(COLUMN_BASE)
    r.read(OFFSET_BASE)

    # -- F1 .. F6 ---------------------------------------------------------
    r.label("F1 unmapped, F2 index >= MAX_C, E1 unaligned")
    r.read(0x048)
    r.read(WEIGHT_BASE + 4 * MAX_C)
    r.read(OFFSET_BASE + 4 * MAX_C)
    r.read(REG_N + 2)
    r.read(0x3FC)

    r.label("F3, F4 -- writing what may only be read")
    r.write(REG_ID, 0xDEADBEEF)
    r.write(REG_VERSION, 0)
    r.write(REG_CAPS, 0)
    r.write(REG_CELL, 1)
    r.write(COLUMN_BASE, 0xFFFFFFFF)
    r.write(OFFSET_BASE, 7)

    r.label("F6 -- sel is not a whole word")
    r.read(REG_N, sel=0x3)
    r.read(REG_ID, sel=0x1)
    r.write(REG_N, 8, sel=0xE)
    r.write(REG_CTRL, CTRL_START, sel=0x1)

    r.label("F5 -- reading CTRL is NOT an error; START reads back 0")
    r.write(REG_CTRL, CTRL_IRQ_EN)
    r.read(REG_CTRL)
    r.write(REG_CTRL, 0)
    r.read(REG_CTRL)

    # -- S2 Load, polling -------------------------------------------------
    # The worked example of docs/BCMC.md: N = 8, weights (6, 3, 5).
    r.label("S2 Load N=8, weights 6 3 5, poll for VALID")
    r.program(8, [6, 3, 5])
    r.read(REG_STATUS)

    # -- S6 Read the offsets back -----------------------------------------
    r.label("S6 Read the offsets back")
    for i in range(4):
        r.read(OFFSET_BASE + 4 * i)  # index 3 is beyond C: an inactive lane

    # -- E1 once more, this time INSIDE the mapped windows ----------------
    # The probes above are unaligned AND unmapped, so they cannot tell the two
    # halves of E1 apart: a decoder that had forgotten alignment altogether
    # would still refuse them, for the wrong reason. A single register cannot
    # be probed this way -- it is decoded by equality, so any unaligned
    # address near it is unmapped as well -- but the three windows can: these
    # addresses are unaligned and yet their word index falls squarely on
    # COLUMN[0], WEIGHT[0], OFFSET[0]. They are recorded with the matrix
    # valid, so that no other condition could account for the refusal.
    r.label("E1 unaligned inside a mapped window")
    r.read(COLUMN_BASE + 2)
    r.read(WEIGHT_BASE + 2)
    r.read(OFFSET_BASE + 2)
    # And an unaligned write, which a decoder missing the check would not
    # merely acknowledge but obey: WEIGHT[0] would become 1 and VALID would
    # fall, both of which the two accesses after it would see.
    r.write(WEIGHT_BASE + 2, 1)
    r.read(WEIGHT_BASE)
    r.read(REG_STATUS)

    # -- S4 Read one cell -------------------------------------------------
    r.label("S4 Read every cell of the 3 x 8 matrix, one at a time")
    for row in range(4):  # row 3 is beyond C, and is not an error
        r.write(REG_CELL_ROW, row)
        for col in range(8):
            r.write(REG_CELL_COL, col)
            r.read(REG_CELL)
    r.write(REG_CELL_ROW, MAX_C - 1)
    r.write(REG_CELL_COL, 0)
    r.read(REG_CELL)

    # -- S5 Read one column -----------------------------------------------
    r.label("S5 Read the same matrix again, by column")
    for col in range(8):
        r.write(REG_CELL_COL, col)
        r.read(COLUMN_BASE)
        r.read(COLUMN_BASE + 4)
    r.label("F1 -- COLUMN words above ceil(MAX_C/32) are not mapped")
    r.read(COLUMN_BASE + 8)

    # -- S3 Interrupt-driven ----------------------------------------------
    r.label("S3 Load N=12, weights 5 5 5 5, interrupt-driven")
    r.write(REG_STATUS, STATUS_IRQ)  # clear the flag S2 raised
    r.read(REG_STATUS)
    r.program(12, [5, 5, 5, 5], irq=True)
    r.read(REG_STATUS)
    r.write(REG_STATUS, STATUS_IRQ)
    r.read(REG_STATUS)
    r.write(REG_STATUS, STATUS_IRQ)  # clearing a clear flag is not an error
    r.read(REG_STATUS)
    r.write(REG_CTRL, 0)  # IRQ_EN off again
    for i in range(4):
        r.read(OFFSET_BASE + 4 * i)

    # -- S7 Re-run with a modified weight ---------------------------------
    r.label("S7 Change one weight: VALID falls, the old matrix is gone")
    r.write(WEIGHT_BASE + 4, 2)
    r.read(REG_STATUS)
    r.read(REG_CELL)        # F10 again: the matrix is stale, so this errs
    r.read(OFFSET_BASE)
    r.write(REG_CTRL, CTRL_START)
    r.poll(REG_STATUS, STATUS_VALID, STATUS_VALID)
    for i in range(4):
        r.read(OFFSET_BASE + 4 * i)

    # -- S8 The empty instance --------------------------------------------
    r.label("S8 C = 0: a matrix with no rows is still a matrix")
    r.write(REG_C, 0)
    r.write(REG_CTRL, CTRL_START)
    r.poll(REG_STATUS, STATUS_VALID, STATUS_VALID)
    r.read(REG_STATUS)
    r.write(REG_CELL_ROW, 0)
    r.write(REG_CELL_COL, 0)
    r.read(REG_CELL)
    r.read(COLUMN_BASE)
    r.read(OFFSET_BASE)

    return r


#---------------------------------------------------------------------------
# The busy window
#
# F7, F8 and F9 can only be recorded while a transform is running, which in the
# model means giving it a positive `latency`: the number of subsequent accesses
# that will observe BUSY. Set it to exactly the length of the block, so that
# the poll which follows the block is the access that completes the transform.
#
# In hardware the window is real: C = MAX_C takes about MAX_C + 4 clocks, and
# each replayed access costs two, so the block has roughly three times the room
# it needs. If the RTL ever finishes early the recorded ERRs become ACKs and
# the replay fails -- which is the correct outcome, because a BUSY flag that
# drops too soon is a bug.
#---------------------------------------------------------------------------


def record_busy():
    dev = BcmcPeripheral(max_c=MAX_C, val_w=VAL_W, idx_w=IDX_W)
    r = Recorder(dev)
    r.reset()

    r.label("A large instance, so that the transform is long enough to catch")
    weights = [(i % 5) + 1 for i in range(MAX_C)]
    n = MAX_C
    r.write(REG_N, n)
    r.write(REG_C, MAX_C)
    for i, w in enumerate(weights):
        r.write(WEIGHT_BASE + 4 * i, w)
    r.write(REG_CELL_ROW, 0)
    r.write(REG_CELL_COL, 0)

    # What will be attempted while BUSY, in order. Counted before the start so
    # that the model's clock can be set to match.
    def block(rec):
        rec.read(REG_STATUS)                       # BUSY = 1, VALID = 0
        rec.write(REG_N, 1)                        # F7
        rec.write(REG_C, 1)                        # F7, for C as well as N
        rec.write(WEIGHT_BASE, 1)                  # F7, for the window
        rec.write(REG_CTRL, CTRL_START)            # F8
        rec.read(REG_CELL)                         # F9
        rec.read(COLUMN_BASE)                      # F9
        rec.read(OFFSET_BASE)                      # F9
        rec.read(REG_ID)                           # ... but this still works
        rec.read(WEIGHT_BASE)                      # and so does this
        rec.write(REG_CELL_ROW, 3)                 # a query index is not state
        rec.write(REG_CELL_COL, 5)
        rec.write(REG_CTRL, 0)                     # CTRL without START is fine
        rec.read(REG_STATUS)                       # still BUSY

    counter = Recorder(BcmcPeripheral(max_c=MAX_C, val_w=VAL_W, idx_w=IDX_W))
    block(counter)
    dev.latency = counter.n_ops

    r.label("F7, F8, F9 -- what may not be done while BUSY, and what may")
    r.write(REG_CTRL, CTRL_START)
    block(r)

    r.label("... and then it finishes")
    r.poll(REG_STATUS, STATUS_VALID, STATUS_VALID)
    r.read(REG_STATUS)
    for i in range(0, MAX_C, 7):
        r.read(OFFSET_BASE + 4 * i)
    for col in range(0, n, 5):
        r.write(REG_CELL_COL, col)
        r.read(COLUMN_BASE)
        r.read(COLUMN_BASE + 4)

    return r


#---------------------------------------------------------------------------
# The randomised soak
#---------------------------------------------------------------------------


def record_random(count, seed):
    rng = random.Random(seed)
    dev = BcmcPeripheral(max_c=MAX_C, val_w=VAL_W, idx_w=IDX_W)
    r = Recorder(dev)
    r.reset()

    # The edge shapes first, then random ones. Every instance is read back
    # twice, by cell and by column, so the two projections are compared over
    # the bus exactly as sim/bcmc_row_test.cpp compares them in the fabric.
    cases = [
        (1, [0]),
        (1, [1]),
        (2, [2, 0]),
        (5, [5, 5, 5]),
        (7, [0, 0, 0, 0]),
    ]
    for _ in range(count):
        n = rng.randint(1, 24)
        c = rng.randint(1, 12)
        cases.append((n, [rng.randint(0, n) for _ in range(c)]))

    for n, weights in cases:
        c = len(weights)
        r.label("N = %d, C = %d, weights %s" % (n, c, " ".join(str(w) for w in weights)))
        r.program(n, weights)

        for i in range(c):
            r.read(OFFSET_BASE + 4 * i)

        for col in range(n):
            r.write(REG_CELL_COL, col)
            r.read(COLUMN_BASE)

        for row in range(c):
            r.write(REG_CELL_ROW, row)
            for col in range(n):
                r.write(REG_CELL_COL, col)
                r.read(REG_CELL)

    return r


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--out",
        default=os.path.join(os.path.dirname(__file__), "..", "sim", "vectors"),
        help="directory to write the vector files into",
    )
    parser.add_argument("--count", type=int, default=12, help="random instances")
    parser.add_argument("--seed", type=int, default=20240404)
    args = parser.parse_args()

    out = os.path.abspath(args.out)
    os.makedirs(out, exist_ok=True)

    write_file(
        os.path.join(out, "wb_sequences.txt"),
        "S1 to S8 and F1 to F11 of docs/Transaction_Sequences.md",
        record_sequences(),
    )
    write_file(
        os.path.join(out, "wb_busy.txt"),
        "The BUSY window: F7, F8, F9, and what remains legal",
        record_busy(),
    )
    write_file(
        os.path.join(out, "wb_random.txt"),
        "Random instances, read back by cell and by column",
        record_random(args.count, args.seed),
    )


if __name__ == "__main__":
    main()

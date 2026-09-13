"""
Replay the observer peripheral bus corpus against the model.

`sim/vectors/obswb_edge.txt` is treated as a FIXED, EXTERNAL artifact: this
replayer does not regenerate it, and it does not ask `observer_periph.py` what
the answers should be. It drives the model with the transactions the corpus
encodes and compares the model's **declared observable outputs** with the fields
the corpus already carries:

    generator  ->  corpus  ->  model

and not

    generator  ->  model  ->  corpus  ->  the same model

which compares the model with itself and proves nothing.

What it checks
--------------
Values: `ack`, `err`, `rdata`, `running`, `visit_valid`, `column`, `done`,
`aborted`, on every recorded cycle.

Shape, which is half the contract:

  * a request is presented in the cycle the corpus says it is;
  * the response lands in the documented response cycle -- the one after
    `cyc & stb`, never later;
  * the following cycle really is idle, with no response;
  * `ack` and `err` are never both asserted, and a refused access is never
    acknowledged;
  * `rdata` is compared only when the corresponding read was actually accepted,
    because a refused read has no meaningful data.

What it deliberately does not check, and why
--------------------------------------------
**`column_bits`.** The cycle model owns no projection: `observer_hw.py` holds a
cursor, and the matrix is `bcmc_column.v`'s. The corpus carries `column_bits`
because it is a *declared output of the RTL*, and it is checked there -- by the
differential harness against this same corpus. Re-deriving it here would mean
calling `reference.py` a second time, which is the generator's own arithmetic.

**The held-`stb` collision.** `observer_periph.py` is transaction-level and has
no per-cycle accept logic, so it cannot say whether a slave that re-asserted
`stb` through a response would answer twice. That is a bus-protocol property and
only the RTL can be held to it. Here the replayer checks the *shape* of the
recorded collision -- exactly one response, then a genuinely idle cycle -- and
leaves the protocol claim to the differential harness.

**Bus reset.** The corpus records `rst = 0` throughout; resetting the device
between runs is the harness's job, not a transaction.

Usage:
    python3 replay_observer_wb.py
"""

import os
import sys

from observer_periph import ObserverPeriph

VECTOR_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                          "..", "sim", "vectors")

# field order, as gen_observer_wb_vectors.py writes it
F_VALID, F_RST, F_CYC, F_STB, F_WE, F_ADR, F_SEL, F_DAT = range(8)
F_ACK, F_ERR, F_RDATA = 8, 9, 10
F_RUN, F_VISIT, F_COL, F_DONE, F_ABORT, F_BITS = 11, 12, 13, 14, 15, 16
FIELDS = 17


def parse(path):
    """Read the corpus as text. Nothing here is recomputed."""
    runs = []
    cur = None
    with open(path, encoding="utf-8") as fh:
        for raw in fh:
            line = raw.strip()
            if not line or line.startswith("#") or line.startswith("---"):
                continue
            tok = line.split()
            if tok[0] == "H":
                if len(tok) != 5:
                    raise ValueError(f"malformed H line: {line!r}")
                cur = {"name": tok[1], "N": int(tok[2]), "C": int(tok[3]),
                       "cycles": int(tok[4]), "rows": []}
                runs.append(cur)
            elif tok[0] == "C":
                if cur is None:
                    raise ValueError("C line before any H line")
                vals = [int(t, 16) for t in tok[1:]]
                if len(vals) != FIELDS:
                    raise ValueError(f"C line has {len(vals)} fields, want {FIELDS}")
                cur["rows"].append(vals)
            else:
                raise ValueError(f"unknown tag {tok[0]!r}")
    return runs


def engine_view(p):
    """The declared observables the model is able to speak for."""
    c = p.eng.outputs(p.valid)
    return {F_RUN: int(c.running), F_VISIT: int(c.visit_valid), F_COL: c.column,
            F_DONE: int(c.done), F_ABORT: int(c.aborted)}



def replay(path, model=ObserverPeriph, on_rdata=None):
    """
    Drive the corpus through `model` and compare.

    `model` is a factory called as `model(N=..., C=...)`, so this same driver can
    hold a v2.0c model to the same corpus without a second implementation of the
    driving protocol. `on_rdata`, if given, is called for **every** read-data
    comparison -- matches included -- so a caller can classify differences rather
    than only count them.
    """
    fails = []
    checks = 0
    cycles = 0
    runs = 0

    def compare(name, idx, p, row):
        """The declared observables, checked where they are declared."""
        nonlocal checks
        for f, got in engine_view(p).items():
            checks += 1
            if row[f] != got:
                fails.append(f"{name} cycle {idx}: field {f} is "
                             f"{row[f]:#x} in the corpus, {got:#x} in the model")

    for run in parse(path):
        name = run["name"]
        rows = run["rows"]
        if len(rows) != run["cycles"]:
            fails.append(f"{name}: H says {run['cycles']} cycles, "
                         f"file has {len(rows)}")
            continue
        runs += 1
        p = model(N=run["N"], C=run["C"])

        i = 0
        while i < len(rows):
            row = rows[i]
            p.valid = bool(row[F_VALID])

            checks += 1
            if row[F_ACK] and row[F_ERR]:
                fails.append(f"{name} cycle {i}: ack and err together")

            if not (row[F_CYC] and row[F_STB]):
                # A standalone idle cycle -- the response cycles of ordinary
                # transactions are consumed below, so a stb=0 row reaching here
                # is an idle op, and must carry no response.
                checks += 1
                if row[F_ACK] or row[F_ERR]:
                    fails.append(f"{name} cycle {i}: idle but answered")
                compare(name, i, p, row)
                p.tick()
                cycles += 1
                i += 1
                continue

            # A request. It must not be answered in this cycle: the response
            # belongs to the next one. Its recorded observables are those
            # *before* the access took effect, so compare before performing it.
            checks += 1
            if row[F_ACK] or row[F_ERR]:
                fails.append(f"{name} cycle {i}: the request cycle is answered")
            compare(name, i, p, row)

            if row[F_WE]:
                acked = p.write(row[F_ADR], row[F_DAT], sel=row[F_SEL])
                data, is_read = 0, False
            else:
                acked, data = p.read(row[F_ADR], sel=row[F_SEL])
                is_read = True
            p.tick()
            cycles += 1

            if i + 1 >= len(rows):
                fails.append(f"{name} cycle {i}: a request with no response")
                break
            resp = rows[i + 1]
            p.valid = bool(resp[F_VALID])
            held = bool(resp[F_CYC] and resp[F_STB])

            checks += 1
            if resp[F_ACK] != int(acked):
                fails.append(f"{name} cycle {i+1}: ack is {resp[F_ACK]:#x}, "
                             f"the model says {int(acked)}")
            checks += 1
            if resp[F_ERR] != int(not acked):
                fails.append(f"{name} cycle {i+1}: err is {resp[F_ERR]:#x}, "
                             f"the model says {int(not acked)}")
            checks += 1
            want = data if (is_read and acked) else 0
            if resp[F_RDATA] != want:
                fails.append(f"{name} cycle {i+1}: rdata is "
                             f"{resp[F_RDATA]:#x}, expected {want:#x}")
            if on_rdata is not None:
                on_rdata(name, i + 1, row[F_ADR], resp[F_RDATA], want)
            compare(name, i + 1, p, resp)
            p.tick()
            cycles += 1
            i += 2

            if held:
                # The request was held through its response. The slave must not
                # answer twice; the corpus records a third, idle cycle.
                if i >= len(rows):
                    fails.append(f"{name}: a held-stb run with no idle cycle")
                    break
                idle = rows[i]
                p.valid = bool(idle[F_VALID])
                checks += 1
                if idle[F_CYC] or idle[F_STB]:
                    fails.append(f"{name} cycle {i}: the held-stb run does not "
                                 f"end in an idle cycle")
                checks += 1
                if idle[F_ACK] or idle[F_ERR]:
                    fails.append(f"{name} cycle {i}: a second response")
                compare(name, i, p, idle)
                p.tick()
                cycles += 1
                i += 1

    return fails, checks, cycles, runs


def main():
    path = os.path.join(VECTOR_DIR, "obswb_edge.txt")
    if not os.path.isfile(path):
        print(f"FAIL  no such file: {path}")
        print("      (cd validation && python3 gen_observer_wb_vectors.py)")
        return 1

    fails, checks, cycles, runs = replay(path)
    if fails:
        for f in fails[:10]:
            print(f"  FAIL {f}")
        print(f"replay_observer_wb.py: FAIL  {len(fails)} disagreement(s)")
        return 1

    print(f"replay_observer_wb.py: PASS  {runs} runs, {cycles} cycles, "
          f"{checks} comparisons  (obswb_edge.txt)")
    print("  the corpus describes the frozen contract, and the model agrees.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

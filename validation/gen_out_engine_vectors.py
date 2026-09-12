"""
Generate the output engine's cycle vectors.

`sim/vectors/outeng_edge.txt` is the artifact the RTL is replayed against, by two
independent simulators, from an external stimulus record. It is not a
serialization of `output_engine.py`'s internals: what it carries is the block's
**inputs per cycle** and its one **observable output**, `pins_o`. `pattern_q` and
`valid_q` are model state and do not appear.

The corpus is built from a table of mandatory cases and then *read back and
checked*, so a case that stops contributing a run fails generation. On top of
that, and specific to this layer, the generator checks that the corpus
**discriminates**: the five wiring faults of `docs/Output_Engine.md` section 10.6
that the model cannot plant -- polarity applied before the gate, `rst` ignored,
the pattern cleared on `done`, `done` acted on one cycle late, and a lane above
`C` presented -- are each simulated here against the same stimulus, and the
corpus must differ from every one of them somewhere. A corpus that cannot tell a
wrong implementation from a right one has no teeth, however many cases it holds.

Two of the mandatory cases are there because the model's own mutation battery
proved they are load-bearing: the stimulus must **hold a non-zero pattern**, and
it must be **invalidated while something is actually being driven**. An earlier
stimulus did neither, and `no_hold`, `no_gate` and `no_destroy` all survived it.

Usage:
    python3 gen_out_engine_vectors.py
"""

import os
import sys

from output_engine import REF_MAX_C, Attached, low_mask

VECTOR_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                          "..", "sim", "vectors")
OUT_PATH = os.path.join(VECTOR_DIR, "outeng_edge.txt")

MAX_C = REF_MAX_C


def drive(N, C, W, O, ops, oneshot=False):
    """
    Build one run from a list of operations. Each op is a tuple:

        ('start',)      accept a start
        ('trigger',)    a trigger
        ('idle', n=1)   n cycles with nothing asserted
        ('rst', n=1)    n cycles with rst asserted
        ('low', n=1)    n cycles with `valid` low
        ('high', n=1)   n cycles with `valid` high

    The stimulus this produces is the RTL's stimulus: the engine's `visit_valid`
    and the projection's `column_bits`, cycle by cycle, plus `valid` and `rst`.
    """
    a = Attached(N=N, C=C, W=W, O=O, oneshot=oneshot, valid=True, max_c=MAX_C)
    for op in ops:
        kind = op[0]
        n = op[1] if len(op) > 1 else 1
        for _ in range(n):
            if kind == 'start':
                a.cycle(start=1)
            elif kind == 'trigger':
                a.cycle(trigger=1)
            elif kind == 'idle':
                a.cycle()
            elif kind == 'rst':
                a.cycle(rst=1)
            elif kind == 'low':
                a.valid = False
                a.cycle()
            elif kind == 'high':
                a.valid = True
                a.cycle()
            else:
                raise ValueError(f"unknown op {op!r}")
    return a


# ---------------------------------------------------------------------------
# The mandatory cases. build() constructs the corpus FROM this table, so a case
# that stops contributing a run fails generation.
# ---------------------------------------------------------------------------

ONES = [1, 1]
FULL = [2, 2]          # every row active in every column
NONE = [0, 0]          # the all-zero context: item 8's counterexample
WIDE = [1] * MAX_C     # C = MAX_C


def cases():
    c = {}

    c["visit_latched"] = (2, 2, ONES, [0, 0], [
        ('start',), ('idle', 2),
    ])
    # A non-zero pattern, observed standing still. Load-bearing: the model's
    # mutation battery found `no_hold` survives any stimulus that never holds.
    c["hold_nonzero"] = (2, 2, ONES, [0, 0], [
        ('start',), ('idle', 4),
    ])
    # Invalidate while the pins are actually driven, then revalidate WITH NO
    # VISIT. This is the case a level-only gate fails.
    c["invalid_while_driving"] = (2, 2, ONES, [0, 0], [
        ('start',), ('idle', 2), ('low', 3), ('high', 3),
    ])
    c["reset_while_driving"] = (2, 2, ONES, [0, 0], [
        ('start',), ('idle', 2), ('rst',), ('idle', 2),
    ])
    # Two consecutive visits presenting the SAME pattern: W = [N] makes every row
    # active in every column, so a passthrough is invisible here and the hold is
    # what distinguishes the visits.
    c["identical_consecutive"] = (2, 2, FULL, [0, 0], [
        ('start',), ('idle',), ('trigger',), ('idle',), ('trigger',), ('idle', 2),
    ])
    # Visits presenting zero: item 8's counterexample, a whole pass of nothing.
    c["zero_patterns"] = (3, 2, NONE, [0, 0], [
        ('start',), ('idle',), ('trigger',), ('idle',), ('trigger',), ('idle', 2),
    ])
    # Differing consecutive patterns, which is where a passthrough shows up.
    c["differing_consecutive"] = (2, 2, ONES, [0, 0], [
        ('start',), ('idle',), ('trigger',), ('idle', 2),
    ])
    # C = 0: an empty matrix. Every pattern is zero, and the section 2.3 check
    # requires exactly that.
    c["c_zero"] = (2, 0, [], [], [
        ('start',), ('idle',), ('trigger',), ('idle', 2),
    ])
    # C = MAX_C: the whole pattern width in use.
    c["c_max"] = (2, MAX_C, WIDE, [0] * MAX_C, [
        ('start',), ('idle',), ('trigger',), ('idle', 2),
    ])
    # A pass's final visit, with a NON-ZERO pattern staying on the pins after it
    # -- so that a block which cleared on `done` would be caught.
    c["final_visit"] = (2, 2, FULL, [0, 0], [
        ('start',), ('idle',), ('trigger',), ('idle', 3),
    ])
    # Visits separated by an invalidation, which ends the pass and needs a new
    # start: after an abort the engine is IDLE and a trigger is ignored.
    c["visits_across_a_gap"] = (2, 2, ONES, [0, 0], [
        ('start',), ('idle',), ('low',), ('high', 2),
        ('start',), ('idle',), ('trigger',), ('idle', 2),
    ])
    # Reset and invalidation around the pass boundaries.
    c["boundaries"] = (2, 2, ONES, [0, 0], [
        ('start',), ('rst',), ('idle',), ('start',), ('idle',),
        ('low',), ('rst',), ('high',), ('idle', 2),
    ])
    # A continuous pass wrapping into the next, with no gap.
    c["continuous_wrap"] = (2, 2, ONES, [0, 0], [
        ('start',), ('idle',), ('trigger',), ('idle',),
        ('trigger',), ('idle',), ('trigger',), ('idle', 2),
    ])
    return c


def run_for(spec):
    N, C, W, O, ops = spec
    return drive(N, C, W, O, ops)


# ---------------------------------------------------------------------------
# The five wiring faults the model cannot plant (section 10.6), each simulated
# against the same stimulus. The corpus must differ from every one of them
# somewhere -- otherwise it cannot catch that fault, however many cases it holds.
# These belong to the corpus layer rather than to output_engine.py's mutation
# battery, which owns the three faults that are the *stage's own* behaviour.
# ---------------------------------------------------------------------------

FAULTS = (
    "polarity_before_gate",
    "rst_ignored",
    "clear_on_done",
    "done_delayed",
    "lane_above_c",
)


def faulty_pins(kind, a):
    """What one wiring fault would have driven, given the same stimulus."""
    pins = []
    pattern = 0
    valid_q = 0
    done_prev = False

    for k, t in enumerate(a.in_trace):
        rst, valid = bool(t[2]), bool(t[5])
        vv = a.out_trace[k].visit_valid
        bits = a.bits_trace[k]
        done = a.out_trace[k].done

        # -- this cycle's pins, under the fault -----------------------------
        shown = pattern if valid else 0
        if kind == "polarity_before_gate":
            shown = (~pattern) & low_mask(MAX_C) if valid else low_mask(MAX_C)
        elif kind == "lane_above_c":
            if valid:
                shown |= (1 << min(a.C, MAX_C - 1))
        pins.append(shown)

        # -- the edge ------------------------------------------------------
        if kind == "rst_ignored":
            if valid_q and not valid:
                pattern = 0
            elif vv:
                pattern = bits
        elif kind == "clear_on_done":
            if rst:
                pattern = 0
            elif valid_q and not valid:
                pattern = 0
            elif done:
                pattern = 0
            elif vv:
                pattern = bits
        elif kind == "done_delayed":
            if rst:
                pattern = 0
            elif valid_q and not valid:
                pattern = 0
            elif done_prev:
                pattern = 0
            elif vv:
                pattern = bits
        else:
            # the specified edge: only the pins above are faulty
            if rst:
                pattern = 0
            elif valid_q and not valid:
                pattern = 0
            elif vv:
                pattern = bits

        valid_q = 1 if valid else 0
        done_prev = done

    return pins


def check_discriminates(runs):
    """Which faults the corpus can tell apart from the specification."""
    caught = {name: False for name in FAULTS}
    blind = {name: [] for name in FAULTS}
    for run_name, a in runs.items():
        for fault in FAULTS:
            if faulty_pins(fault, a) != a.pins_trace:
                caught[fault] = True
            else:
                blind[fault].append(run_name)
    return caught, blind


# ---------------------------------------------------------------------------
# Writing, and reading back
# ---------------------------------------------------------------------------

def write(runs, path):
    lines = [
        "# Output-engine cycle vectors -- generated by gen_out_engine_vectors.py",
        "# from validation/output_engine.py. Do not edit by hand.",
        "#",
        "# H <name> <N> <C> <cycles>",
        "# C <rst> <valid> <visit_valid> <column_bits> <pins>",
        "#",
        "# `pins` is the whole observable contract: this block has one output.",
        "# `pattern_q` and `valid_q` are model state and are deliberately absent.",
        "",
    ]
    for name, a in runs.items():
        lines.append(f"H {name} {a.N} {a.C} {len(a.pins_trace)}")
        for k, t in enumerate(a.in_trace):
            lines.append("C %d %d %d %x %x" % (
                int(t[2]), int(t[5]), int(a.out_trace[k].visit_valid),
                a.bits_trace[k], a.pins_trace[k]))
        lines.append("---")
        lines.append("")
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines) + "\n")


def read_back(path):
    """The run names actually present in the file, read from the file."""
    names = []
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            tok = line.split()
            if tok and tok[0] == "H":
                names.append(tok[1])
    return names


def main():
    table = cases()
    runs = {name: run_for(spec) for name, spec in table.items()}

    os.makedirs(VECTOR_DIR, exist_ok=True)
    write(runs, OUT_PATH)

    names = set(read_back(OUT_PATH))
    missing = [n for n in table if n not in names]
    if missing:
        print(f"FAIL  the corpus is missing these mandatory cases: {missing}")
        return 1

    if not any(any(a.pins_trace) for a in runs.values()):
        print("FAIL  no run ever drives a pin, so the corpus cannot test a latch")
        return 1

    caught, blind = check_discriminates(runs)
    uncaught = [f for f in FAULTS if not caught[f]]
    if uncaught:
        print("FAIL  the corpus cannot catch these wiring faults:")
        for f in uncaught:
            print(f"        {f}  (indistinguishable in: {blind[f]})")
        return 1

    cycles = sum(len(a.pins_trace) for a in runs.values())
    print(f"outeng_edge.txt: {len(runs)} runs, {cycles} cycles, "
          f"{len(table)} mandatory cases all present")
    print(f"  discriminates all {len(FAULTS)} wiring faults the model cannot see")
    return 0


if __name__ == "__main__":
    sys.exit(main())

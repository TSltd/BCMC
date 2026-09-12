"""
Generate the affine source's stimulus corpus, and prove it has teeth.

    validation/gen_src_affine_vectors.py  ->  sim/vectors/srcaff_edge.txt

Why this corpus is a script and not a table
-------------------------------------------
Every other corpus in this project is a flat table of one line per cycle. This one
cannot be, and the reason is a contract rather than convenience: section 4.4
promises **no cycle count** for `(a, b)`'s derivation -- the number of rejection
draws is a random variable -- so a table that pinned `ready` to a cycle would be
testing a schedule the specification disclaims. The corpus therefore records a
*script* -- reset, wait for ready, drive this walk, change `N`, wait again -- and
the bench polls the handshake instead of counting cycles.

What the expected values come from
----------------------------------
Not from the RTL's rule. `ts_pi` is expected to be
`affine_sequence(a, b, N)[ts_t]` -- the **closed form**, computed with a real
multiply and a real modulus -- while the RTL computes it with an accumulator. The
corpus is a check against the mathematics, not a restatement of the arithmetic
under test. `(a, b)` come from `derive_ab`, the pinned reference.

The two guards
--------------
The mandatory-category guard is the one the other generators carry. The second is
the one this module needs: the corpus must be able to tell the two seam mutants
apart from the truth, because both were live bugs that only a *non-contiguous*
`ts_t` sequence exposes. A corpus of back-to-back visits would pass a
free-running accumulator, and a corpus with no restart would pass `ts_t = t_next`
in IDLE.
"""

import os
import sys
from math import gcd

from traversal_sources import affine_sequence, derive_ab

VECTOR_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                          "..", "sim", "vectors")


# ---------------------------------------------------------------------------
# Engine-like walks
#
# A walk is the `ts_t` sequence the engine drives. Two properties make it
# engine-like, and both are enforced:
#
#   * it **begins at 0**, because a pass begins at step 0 -- that is the ask the
#     source uses to synchronise its accumulator, and section 8.2 makes the engine
#     drive 0 in IDLE for exactly this reason;
#   * each later value is either a repeat of the previous one (a *gap* -- no
#     trigger that cycle) or the next step, wrapping to 0.
# ---------------------------------------------------------------------------

def next_step(t, N):
    return 0 if t == N - 1 else t + 1


def walk(N, steps, gap_every=0, gap_len=2):
    """A walk of `steps` cycles, holding every `gap_every`-th value over."""
    out = [0]
    t = 0
    i = 1
    while len(out) < steps:
        if gap_every and i % gap_every == 0:
            out.extend([t] * (gap_len - 1))
            i += gap_len - 1
            if len(out) >= steps:
                break
        t = next_step(t, N)
        out.append(t)
        i += 1
    return out[:steps]


def check_walk(N, seq):
    assert seq[0] == 0, "a walk must begin at step 0"
    for prev, t in zip(seq, seq[1:]):
        assert t == prev or t == next_step(prev, N), \
            f"not engine-like: {prev} -> {t} (N={N})"
    return seq


def Wline(seq):
    """A walk record, with its length: a bench reading tokens needs the count."""
    return "W " + str(len(seq)) + " " + " ".join(str(x) for x in seq)


def find_seed_with_a(N, want, limit=20000):
    for s in range(limit):
        a, _, _ = derive_ab(N, s)
        if a == want:
            return s
    raise SystemExit(f"no seed under {limit} gives a={want} for N={N}")


def ab(N, seed):
    a, b, _ = derive_ab(N, seed)
    return a, b


# ---------------------------------------------------------------------------
# The runs
#
# Each record is a line the bench executes in order:
#
#   R <name> <N> <seed> <rst> <A> <B>   reset, wait for ready, check (a, b)
#   P <at> <hold>                       a reset pulse `hold` cycles long, `at`
#                                       cycles after the derivation begins
#   W <t0> <t1> ...                     drive these ts_t values, one per cycle
#   C <load> <N> <seed> <A> <B>         change N (and seed), require ready to
#                                       drop, then wait and check the new (a, b)
# ---------------------------------------------------------------------------

def build_runs():
    runs = []
    cats = {}

    def add(name, recs, *tags):
        runs.append((name, recs, tags))
        for t in tags:
            cats.setdefault(t, name)

    # -- the small cases, and the two seeds that are easiest to get wrong -----
    a, b = ab(1, 0)
    add("n1_zero_seed",
        [f"R n1_zero_seed 1 0 2 {a} {b}",
         Wline(check_walk(1, walk(1, 6)))],
        "N=1", "seed_0")

    a, b = ab(2, 0)
    add("n2_seed_0_gaps",
        [f"R n2_seed_0_gaps 2 0 2 {a} {b}",
         Wline(check_walk(2, walk(2, 12, 3)))],
        "gaps", "seed_0", "wrap")

    a, b = ab(3, 1)
    add("n3_gaps",
        [f"R n3_gaps 3 1 2 {a} {b}",
         Wline(check_walk(3, walk(3, 15, 4)))],
        "gaps", "wrap")

    s1 = find_seed_with_a(4, 1)
    a, b = ab(4, s1)
    add("n4_a_is_one",
        [f"R n4_a_is_one 4 {s1:X} 2 {a} {b}",
         Wline(check_walk(4, walk(4, 12, 5)))],
        "a=1", "gaps")

    a, b = ab(7, 0xFFFFFFFF)
    add("n7_allones_seed",
        [f"R n7_allones_seed 7 FFFFFFFF 2 {a} {b}",
         Wline(check_walk(7, walk(7, 18, 4)))],
        "seed_all_ones", "gaps", "wrap")

    # -- restart after a completed pass -------------------------------------
    # The second walk begins at 0 again, which is what the engine drives when a
    # pass starts: the source must reload its accumulator from b rather than
    # derive it, whatever it was holding before.
    a, b = ab(8, 3)
    w1 = check_walk(8, walk(8, 9))
    w2 = check_walk(8, walk(8, 9))
    add("n8_restart",
        [f"R n8_restart 8 3 2 {a} {b}",
         Wline(w1),
         Wline(w2)],
        "restart", "wrap")

    # -- composite and adversarial N ----------------------------------------
    a, b = ab(210, 0x2A)
    add("n210_composite",
        [f"R n210_composite 210 2A 2 {a} {b}",
         Wline(check_walk(210, walk(210, 20, 6)))],
        "composite_N", "gaps")

    a, b = ab(2310, 0x3039)
    add("n2310_adversarial",
        [f"R n2310_adversarial 2310 3039 2 {a} {b}",
         Wline(check_walk(2310, walk(2310, 16, 5)))],
        "adversarial_N", "gaps")

    # -- the VAL_W + 1 case -------------------------------------------------
    # (40000, seed 1) is the configuration the model searched for: its `a` makes
    # `cur_q + a` exceed 2^16 partway through the walk, so a sum reduced in VAL_W
    # bits is right for the first few steps and wrong after. A short walk is
    # enough, because the divergence was measured to start at step 6.
    a, b = ab(40000, 1)
    add("n40000_width",
        [f"R n40000_width 40000 1 2 {a} {b}",
         Wline(check_walk(40000, walk(40000, 14)))],
        "width_17bit")

    # -- N changes, both directions -----------------------------------------
    # Adversarial: the derived `a` shares a factor with the new N, so reusing the
    # stale pair would be a silent non-bijection. Readiness must drop first.
    s_bad = find_seed_with_a(12, 11)
    a1, b1 = ab(12, s_bad)
    n_bad = a1 * 2                      # gcd(11, 22) = 11
    a2b, b2b = ab(n_bad, 7)
    add("n12_change_load_shares_factor",
        [f"R n12_change_load_shares_factor 12 {s_bad:X} 2 {a1} {b1}",
         Wline(check_walk(12, walk(12, 8))),
         f"C 1 {n_bad} 7 {a2b} {b2b}",
         Wline(check_walk(n_bad, walk(n_bad, 8)))],
        "N_change_load", "N_shares_factor", "restart")

    # The same with no `load` pulse at all: N is not a register in this window, so
    # the source must notice for itself. This is correction 18's test.
    a2n, b2n = ab(n_bad, s_bad)
    add("n12_change_noload_shares_factor",
        [f"R n12_change_noload_shares_factor 12 {s_bad:X} 2 {a1} {b1}",
         Wline(check_walk(12, walk(12, 8))),
         f"C 0 {n_bad} {s_bad:X} {a2n} {b2n}",
         Wline(check_walk(n_bad, walk(n_bad, 8)))],
        "N_change_noload", "N_shares_factor", "restart")

    # The converse: a compatible change still invalidates, and still recovers with
    # a *newly derived* pair rather than the stale one.
    a3, b3 = ab(13, 0xBEEF)
    add("n12_change_to_compatible",
        [f"R n12_change_to_compatible 12 {s_bad:X} 2 {a1} {b1}",
         Wline(check_walk(12, walk(12, 6))),
         f"C 1 13 BEEF {a3} {b3}",
         Wline(check_walk(13, walk(13, 10, 4)))],
        "N_change_load", "N_compatible", "restart")

    # -- reset during preparation -------------------------------------------
    # The pulse lands one cycle after the derivation starts, and the shortest
    # derivation measured is nine cycles, so it is mid-preparation for every N.
    a, b = ab(9, 5)
    add("n9_reset_during_prep",
        ["P 1 1",
         f"R n9_reset_during_prep 9 5 2 {a} {b}",
         Wline(check_walk(9, walk(9, 10, 4)))],
        "reset_during_prep", "gaps")

    # -- a seed write in the middle of a walk -------------------------------
    a, b = ab(6, 8)
    a4, b4 = ab(6, 0x5EED)
    add("n6_load_mid_walk",
        [f"R n6_load_mid_walk 6 8 2 {a} {b}",
         Wline(check_walk(6, walk(6, 8))),
         f"C 1 6 5EED {a4} {b4}",
         Wline(check_walk(6, walk(6, 10, 3)))],
        "seed_write_mid_walk", "restart", "gaps")

    return runs, cats


# ---------------------------------------------------------------------------
# The seam rule, and the mutants the corpus must be able to tell apart
#
# This mirrors rtl/bcmc_src_affine.v's combinational answer exactly -- including
# the single conditional subtract, not a modulus -- so that a mutant can be
# modelled by changing one thing. The expected values, by contrast, come from the
# *closed form*: the corpus checks the mathematics, not this rule.
# ---------------------------------------------------------------------------

def src_walk(N, a, b, seq, mutant=None):
    """The source's answer on each cycle of a walk."""
    cur = b % N
    ts_q = seq[0]              # the bench holds ts_t before the walk begins
    out = []
    for t in seq:
        moved = (t != ts_q)
        inc = ((cur + a) & 0xFFFF) if mutant == "narrow" else (cur + a)
        nxt = inc - N if inc >= N else inc
        if t == 0:
            pi = nxt if mutant == "no_step0" else (b % N)
        elif mutant == "free_running" or moved:
            pi = nxt
        else:
            pi = cur
        out.append(pi)
        cur, ts_q = pi, t
    return out


def expected_walk(N, a, b, seq):
    """The mathematics: the closed form, computed the expensive way."""
    table = affine_sequence(a, b, N)
    return [table[t] for t in seq]


def phases_with_walks(recs):
    """Each walk paired with the (N, seed, a, b) in force when it is driven."""
    out = []
    cur = None
    for r in recs:
        tok = r.split()
        if tok[0] == "R":
            # <name> <N> <seed hex> <rst> <A> <B>
            cur = (int(tok[2]), int(tok[3], 16), int(tok[5]), int(tok[6]))
        elif tok[0] == "C":
            # <load> <N> <seed hex> <A> <B>
            cur = (int(tok[2]), int(tok[3], 16), int(tok[4]), int(tok[5]))
        elif tok[0] == "W":
            out.append((cur, [int(x) for x in tok[2:]]))
    return out


def guard(runs):
    """The correct rule must equal the closed form, and every mutant must differ."""
    caught = {}
    problems = []
    for name, recs, _tags in runs:
        for (N, seed, a, b), seq in phases_with_walks(recs):
            want = expected_walk(N, a, b, seq)
            if src_walk(N, a, b, seq) != want:
                problems.append(f"{name}: the seam rule disagrees with the "
                                f"closed form for N={N}")
            for m in ("free_running", "no_step0", "narrow"):
                if src_walk(N, a, b, seq, mutant=m) != want:
                    caught.setdefault(m, name)
    return caught, problems


MANDATORY = [
    "N=1", "a=1", "seed_0", "seed_all_ones", "composite_N", "adversarial_N",
    "width_17bit", "gaps", "restart", "wrap", "N_change_load", "N_change_noload",
    "N_shares_factor", "N_compatible", "reset_during_prep",
    "seed_write_mid_walk",
]


def main():
    runs, cats = build_runs()
    caught, problems = guard(runs)

    if problems:
        for p in problems[:8]:
            print(f"  FAIL {p}")
        print("gen_src_affine_vectors: FAIL  the corpus does not describe the "
              "mathematics")
        return 1

    wanted = ("free_running", "no_step0", "narrow")
    missing = [m for m in wanted if m not in caught]
    if missing:
        print(f"  FAIL  the corpus cannot distinguish these mutants: {missing}")
        print("        a corpus that cannot tell a wrong implementation from a")
        print("        right one has no teeth, however many runs it holds")
        return 1

    head = [
        "# Affine source vectors -- generated by gen_src_affine_vectors.py",
        "# from validation/traversal_sources.py. Do not edit by hand.",
        "#",
        "# A script, not a table: section 4.4 promises no cycle count for the",
        "# derivation, so the bench POLLS `ready` instead of counting cycles.",
        "#",
        "# R <name> <N> <seed> <rst> <A> <B>  reset, wait for ready, check (a, b)",
        "# P <at> <hold>                      reset pulse, offset from derivation",
        "# W <t0> <t1> ...                    ts_t per cycle, applied once ready",
        "# C <load> <N> <seed> <A> <B>        change N/seed; ready must drop first",
        "#",
        "# ts_pi is expected to be affine_sequence(a, b, N)[ts_t] -- the CLOSED",
        "# FORM -- while the RTL computes it with an accumulator. a_out, b_out and",
        "# ts_pi are compared only while ready is high; before that they are not",
        "# admitted to the engine and are don't-care by construction.",
        "#",
    ]
    lines = list(head)
    for name, recs, rt in runs:
        lines.append(f"# {name} : {' '.join(sorted(rt))}")
        lines.extend(recs)

    path = os.path.join(VECTOR_DIR, "srcaff_edge.txt")
    os.makedirs(VECTOR_DIR, exist_ok=True)
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines) + "\n")

    with open(path, encoding="utf-8") as fh:
        back = fh.read()
    absent = [t for t in MANDATORY if t not in back]
    if absent:
        print(f"  FAIL  the written corpus is missing categories: {absent}")
        return 1

    walks = sum(1 for _, recs, _t in runs for r in recs if r.startswith("W "))
    cycles = sum(len(r.split()) - 2 for _, recs, _t in runs
                 for r in recs if r.startswith("W "))

    print(f"srcaff_edge.txt: {len(runs)} runs, {walks} walks, {cycles} walk cycles")
    print(f"  {len(MANDATORY)} mandatory categories all present")
    for m in wanted:
        print(f"  would catch {m:14s} (first: {caught[m]})")
    print("  and the correct rule equals the closed form on every walk")
    return 0


if __name__ == "__main__":
    sys.exit(main())

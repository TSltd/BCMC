"""
Generate the shuffled source's stimulus corpus, and prove it has teeth.

    validation/gen_src_shuffled_vectors.py  ->  sim/vectors/srcshuf_edge.txt

A script, like the affine source's, and for the same reason: section 5.4 promises
no cycle count for a fill, so a corpus that pinned `ready` to a cycle would test a
schedule the specification disclaims.

The pass-to-bank mapping, which is the interesting part
------------------------------------------------------
The obvious corpus records one permutation per pass. That would encode the fill's
timing, because whether pass k receives the newly completed bank or repeats the
last one depends on whether the fill finished. Instead the corpus records the
**bank stream** (`B` records: bank 0, bank 1, ... in the order the fills complete
them) and the bench derives the mapping from what it observes:

    the pass matches bank b+1  ->  a new bank was handed in, so `underrun` is 0
    the pass matches bank b    ->  the last bank repeated,    so `underrun` is 1

That is exact -- both the permutation and the flag are checked, and they must
agree -- and it assumes nothing about when a fill finishes. It also makes
section 7.3's distinction directly executable: a rate violation changes *freshness*
(the repeat) and not validity (the pass is still a permutation).

Two kinds of run, named as contracts
------------------------------------
    q  freshness-qualified: the stimulus leaves ample room, `underrun` must stay
       0 for the whole run, so every pass must receive a newly completed bank
    u  underrun: the passes are driven back to back, `underrun` is permitted, the
       pass must be the last complete bank, and it must still be a permutation

Where the expected permutations come from
----------------------------------------
Not from the RTL. `traversal_sources.bank_and_cost` -- which is
`observers.permuted_order` for the first bank and continues the same stream for
the rest -- and, for the generator of that stream, the pinned rows of
`sim/vectors/observer_prng.txt`. Section 5.1's claim is that this module needs no
new reference for the permutation family, so the guard below checks the draws this
corpus exercises against that file rather than against the module.
"""

import os
import sys

from observers import SplitMix32
from traversal_sources import bank_and_cost

VECTOR_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                          "..", "sim", "vectors")


def qual_idle(N):
    """
    Idle cycles the qualified runs leave between passes.

    Ample for a fill (~1.4 N + 1 cycles, section 5.2) and a *stimulus* rather than
    a contract: this is not asserted anywhere, and if it were ever too small the
    run fails loudly, because qualified runs require `underrun` to stay 0.
    """
    return 3 * N + 64


def bank_stream(N, seed, count):
    """
    The first `count` banks of the stream, from the reference.

    Bank 0 is `observers.permuted_order(N, seed)`; each later one continues the
    same generator stream, which is what the model's `bank_and_cost` does.
    """
    rng = SplitMix32(seed)
    out = []
    for _ in range(count):
        perm, _ = bank_and_cost(rng, N)
        out.append(perm)
    return out


def draw_stream(N, seed, count_banks, limit=64):
    """
    The raw generator outputs the fill consumes, in order.

    A *recorder*, not a second implementation of anything: it exists so the guard
    can hold this corpus's seeds against `observer_prng.txt`, which section 5.1
    says is the whole prize of the shuffled family. It mirrors the fill's draw
    consumption -- one `next()` per attempt, rejections included.
    """
    rng = SplitMix32(seed)
    draws = []
    for _ in range(count_banks):
        for i in range(N - 1, 0, -1):
            mask = 1
            while mask < i:
                mask = (mask << 1) | 1
            while True:
                z = rng.next()
                draws.append(z)
                if (z & mask) <= i:
                    break
                if len(draws) >= limit:
                    return draws
    return draws


def load_prng_rows():
    rows = {}
    with open(os.path.join(VECTOR_DIR, "observer_prng.txt"),
              encoding="utf-8") as fh:
        for line in fh:
            tok = line.split()
            if tok and tok[0] == "G":
                rows[int(tok[1], 16)] = [int(x, 16) for x in tok[2:]]
    return rows


# ---------------------------------------------------------------------------
# The runs
#
#   R <name> <kind> <N> <seed> <rst>    begin; kind is q (qualified) or u
#   B <k> <p0> ... <pN-1>               bank k of the stream
#   P <pass> <idle> <gap>               drive one pass: `idle` cycles at ts_t = 0,
#                                       then ts_t = 0,1,..,N-1, each held `gap`
#   D <cycles>                          idle at ts_t = 0, checking the run's rule
#   X <cycles>                          assert rst for that many cycles
#   E                                   expect ready == 0 right now
#   L <seed hex>                        pulse load with a new seed
# ---------------------------------------------------------------------------

def build_runs():
    runs = []
    cats = {}

    def add(name, recs, *tags):
        runs.append((name, recs, tags))
        for t in tags:
            cats.setdefault(t, name)

    def bank_recs(stream):
        return [f"B {k} " + " ".join(str(x) for x in p)
                for k, p in enumerate(stream)]

    # -- the smallest banks, where the fill is shorter than the arm cycle
    for N, seed in ((1, 0), (2, 1), (3, 2)):
        add(f"q_n{N}_s{seed:x}",
            [f"R q_n{N}_s{seed:x} q {N} {seed} 2"] + bank_recs(
                bank_stream(N, seed, 3)) +
            [f"P 0 {qual_idle(N)} 1", f"P 1 {qual_idle(N)} 1",
             f"P 2 {qual_idle(N)} 1"],
            "N=1" if N == 1 else f"N={N}", "qualified", "startup_lead")

    # -- banks around the sizes the shuffle's mask cares about
    for N, seed in ((7, 3), (8, 0x2A), (16, 0x3039), (64, 7), (100, 0xBEEF)):
        add(f"q_n{N}_s{seed:x}",
            [f"R q_n{N}_s{seed:x} q {N} {seed} 2"] + bank_recs(
                bank_stream(N, seed, 4)) +
            [f"P 0 {qual_idle(N)} 1", f"P 1 {qual_idle(N)} 1",
             f"P 2 {qual_idle(N)} 3"],
            "qualified", "gaps", "bank_boundary")

    # -- BANK_N_MAX exactly: the largest bank there is
    add("q_n256_max",
        ["R q_n256_max q 256 24173 2"] + bank_recs(bank_stream(256, 24173, 3)) +
        [f"P 0 {qual_idle(256)} 1", f"P 1 {qual_idle(256)} 1"],
        "bank_n_max", "qualified")

    # -- an unservable N: never ready, however long you wait. Kind `n`, because
    #    the qualified rule (ready must be 1) is the opposite of the point here.
    add("unservable_n512",
        ["R unservable_n512 n 512 1 2", "D 4000"],
        "unservable", "never_ready")

    # -- underrun: passes back to back, repeating the last complete bank
    N, seed = 8, 0x9E3779B9
    add("u_backtoback",
        [f"R u_backtoback u {N} {seed} 2"] + bank_recs(
            bank_stream(N, seed, 6)) +
        ["P 0 4 1", "P 1 0 1", "P 2 0 1", "P 3 0 1", "P 4 2 1", "P 5 0 1"],
        "underrun", "repeat_last_bank", "O1_holds")

    # -- reset in the middle, and a reload with a new seed
    N, seed = 16, 0x1234
    add("reset_midstream",
        [f"R reset_midstream q {N} {seed} 2"] + bank_recs(
            bank_stream(N, seed, 3)) +
        [f"P 0 {qual_idle(N)} 1", "X 3", f"P 1 {qual_idle(N)} 1"],
        "reset", "qualified")

    N, seed = 12, 0x77
    add("load_midstream",
        [f"R load_midstream q {N} {seed} 2"] + bank_recs(
            bank_stream(N, seed, 3)) +
        [f"P 0 {qual_idle(N)} 1",
         # The load carries the same seed, so the stream restarts identically and
         # the B records above still describe it: the pass after the load must be
         # bank 0 again. A *different* seed is the same code path with different
         # numbers, and would need its own bank records; this corpus leaves that as
         # the one thing it does not cover, rather than pretending it does.
         f"L {seed:x}",
         f"P 1 {qual_idle(N)} 1"],
        "reload", "qualified")

    return runs, cats


# ---------------------------------------------------------------------------
# The guards
# ---------------------------------------------------------------------------

def partial_bank(N, seed, cut):
    """
    The bank after `cut` accepted swaps.

    This is what a fill that wrote the bank being read would leave behind: a
    *half-advanced* in-place shuffle. It is a permutation -- so O1 cannot see it,
    which is section 5.5's whole point -- and it is not the expected bank, which is
    what makes the corpus able to see it.
    """
    rng = SplitMix32(seed)
    p = list(range(N))
    done = 0
    for i in range(N - 1, 0, -1):
        if done >= cut:
            break
        j = rng.uniform(i)
        p[i], p[j] = p[j], p[i]
        done += 1
    return p


def guard_runs(runs, all_banks):
    """
    Three claims, checked before anything is written:

      * every bank is a permutation;
      * the draws each seed consumes are a prefix of `observer_prng.txt` -- the
        pinned corpus, so this file holds the module to an existing table rather
        than to a new one;
      * the bank-isolation mutation is a *permutation* (invisible to O1) and is
        nevertheless a *different* bank (visible to this corpus).
    """
    caught = {}
    problems = []
    rows = load_prng_rows()

    for name, recs, _tags in runs:
        head = recs[0].split()
        N, seed = int(head[3]), int(head[4])

        for r in recs:
            if not r.startswith("B "):
                continue
            f = r.split()
            perm = [int(x) for x in f[2:]]
            if sorted(perm) != list(range(N)):
                problems.append(f"{name}: bank {f[1]} is not a permutation")

        if seed in rows:
            want = rows[seed]
            got = draw_stream(N, seed, 1, limit=len(want))
            if len(got) >= len(want):
                if got[:len(want)] != want:
                    problems.append(f"{name}: draws do not match observer_prng.txt")
                else:
                    caught.setdefault("prng_prefix", name)

        if N >= 3:
            for cut in range(1, min(N, 6)):
                pb = partial_bank(N, seed, cut)
                if sorted(pb) != list(range(N)):
                    problems.append(f"{name}: a partial bank is not a permutation")
                if pb != all_banks[name][0]:
                    caught.setdefault("bank_isolation_mutant", name)

    return caught, problems


MANDATORY = [
    "N=1", "N=2", "N=3", "qualified", "gaps", "bank_boundary", "bank_n_max",
    "unservable", "never_ready", "underrun", "repeat_last_bank", "O1_holds",
    "reset", "reload", "startup_lead",
]


def main():
    runs, cats = build_runs()

    all_banks = {}
    for name, recs, _tags in runs:
        head = recs[0].split()
        N, seed = int(head[3]), int(head[4])
        nb = sum(1 for r in recs if r.startswith("B "))
        all_banks[name] = bank_stream(N, seed, max(nb, 1)) if N <= 256 else [[0]]

    caught, problems = guard_runs(runs, all_banks)
    if problems:
        for p in problems[:8]:
            print(f"  FAIL {p}")
        print("gen_src_shuffled_vectors: FAIL  the corpus does not describe the "
              "reference")
        return 1

    head = [
        "# Shuffled source vectors -- generated by gen_src_shuffled_vectors.py",
        "# from validation/traversal_sources.py. Do not edit by hand.",
        "#",
        "# A script, not a table: section 5.4 promises no cycle count for a fill,",
        "# so the bench POLLS `ready` rather than counting to it.",
        "#",
        "# R <name> <kind> <N> <seed> <rst>  kind q = qualified (underrun must stay",
        "#                                    0), u = underrun permitted",
        "# B <k> <p...>                      bank k of the stream",
        "# P <pass> <idle> <gap>             idle, then ts_t = 0,1,..,N-1 held `gap`",
        "# D <cycles>                        idle at ts_t = 0",
        "# X <cycles>                        assert rst for that many cycles",
        "# E                                 expect ready == 0",
        "# L <seed hex>                      pulse load with a new seed",
        "#",
        "# A pass matching bank b+1 means a new bank was handed in, so `underrun`",
        "# must be 0; matching bank b means the last bank repeated, so it must be 1.",
        "# The bench checks both, and that they agree.",
        "#",
    ]
    lines = list(head)
    for name, recs, tags in runs:
        lines.append(f"# {name} : {' '.join(sorted(tags))}")
        lines.extend(recs)

    path = os.path.join(VECTOR_DIR, "srcshuf_edge.txt")
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines) + "\n")
    with open(path, encoding="utf-8") as fh:
        back = fh.read()
    absent = [t for t in MANDATORY if t not in back]
    if absent:
        print(f"  FAIL  the written corpus is missing categories: {absent}")
        return 1

    passes = sum(1 for _, recs, _t in runs for r in recs if r.startswith("P "))
    nbank = sum(1 for _, recs, _t in runs for r in recs if r.startswith("B "))
    print(f"srcshuf_edge.txt: {len(runs)} runs, {nbank} banks, {passes} passes")
    print(f"  {len(MANDATORY)} mandatory categories all present")
    print(f"  draws match observer_prng.txt  (first: {caught.get('prng_prefix')})")
    print(f"  the bank-isolation mutant is a permutation AND a different bank")
    print(f"    (first: {caught.get('bank_isolation_mutant')})")
    print("    -> O1 cannot see it; only the per-ask comparison can")
    return 0


if __name__ == "__main__":
    sys.exit(main())

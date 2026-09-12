# BCMC Traversal Sources — opening v2.0c

> **Status: opened, not specified.** This document opens the v2.0c phase and fixes
> what the phase owns. It does not yet specify a source: section 5 is the agenda
> of decisions v2.0c has to take, and the specification that answers them is the
> next artefact — followed by the model that exists to falsify *it*, and only then
> RTL. That ladder has now been walked three times, and each time the model found
> defects in the document rather than in an implementation.
>
> Three checkpoints are frozen, and this document moves none of them: `v2.0a`
> (221f976, the engine), `v2.0a-periph` (587933d, its register window) and `v2.0b`
> (5323eb0, the output engine). The seam v2.0c fills was built *for* this phase
> (`docs/Hardware_Observer_Architecture.md` §3.3), so filling it should be a
> source swap rather than an engine change. This phase's job is to make that true
> and checkable rather than to assume it.

---

## 0. Scope, and what the phase owns

v2.0c supplies **bodies behind the traversal-source seam**. Two new sources are
planned beside the identity source that is already there:

- a **streaming affine source**, `pi(t) = (a t + b) mod N`;
- a **buffered seeded Fisher–Yates source**, reproducing the family
  `docs/Observers.md` already pins.

The phase owns four things:

1. **The rate question.** What cadence a source may impose, and what it costs the
   contracts above it (§2). This is the phase's sharpest subject and it is new:
   v2.0a's identity source had no rate to speak of.
2. **The two decisions §11 of the architecture document left to a v2.0c
   specification**: how `(a, b)` are derived from a 32-bit seed with the
   `gcd(a, N) = 1` guarantee, and whether Fisher–Yates must be index-for-index
   with `sw/bcmc_observer.c`.
3. **The register-window additions** §9.4 of that document sketched: a source
   selector and `OBS_SEED`/`OBS_A`/`OBS_B`, with a seed write clearing
   `SEED_READY` the way a weight write clears `VALID`.
4. **The specification itself**, and the model that falsifies it.

It owns nothing else. The engine's state machine, the one-clock visit contract,
the output engine and the *existing* register-window semantics are frozen, and if
any of them turns out to need a change to accommodate a source, the first
presumption is that the source's design is wrong — the same rule the two previous
phases worked under, and the rule that kept their integration defects from being
"fixed" in the wrong place.

## 1. The seam, as it stands

The interface is frozen and was built before it had more than one body:

```text
        +-------------------+   ts_req, ts_t   +----------------------+
        |  bcmc_observer    |----------------->|  traversal source    |
        |  (engine + cursor)|                  |  identity  (v2.0a)   |
        |                   |<-----------------|  affine    (v2.0c)   |
        +-------------------+   ts_pi, ts_ack  |  shuffled  (v2.0c)   |
                                              +----------------------+
```

| Signal | Dir | Meaning |
| --- | --- | --- |
| `ts_req` | engine → source | "give me `pi(t)`" |
| `ts_t` | engine → source | the step index `t`, `0 .. N-1` |
| `ts_pi` | source → engine | the column index for that step |
| `ts_ack` | source → engine | `ts_pi` is valid this cycle |

**The handshake on this side of the boundary is not a contradiction of
"evaluation has no handshake",** and the argument is inherited rather than made
again here: the characteristic function is a function, so *evaluation* has no
latency; a *traversal source* is not a function of nothing, because a shuffled
source reads an index out of a bank. The handshake delays *which column is asked
about*, never *how long the answer takes*. `rtl/bcmc_observer.v` already contains
the identity body — `ts_pi = ts_t`, `ts_ack = 1` — so the interface's shape is
settled and its existing user is the control case.

A source behind this seam is bound by the observer contract and by nothing else:

- **O1** — over one pass it emits a bijection. It may not skip, repeat, filter or
  resample.
- **O3** — it is a deterministic function of its declared inputs (the seed, and
  `N`), with no wall clock, no uninitialised state and no library randomness.
- **Nothing else.** It may not touch the matrix, the bus, the register window's
  frozen semantics, or the engine's state. It knows `t` and answers with a column;
  it does not know why, and it must not learn.

If a source needs a signal the seam does not carry, that is a finding about a
*frozen* interface. It has to be argued from the source's specification and
accepted as a change to v2.0a — which is exactly the move this project has spent
two phases learning not to make casually.

### 1.1 The seam is already in the RTL, and the engine names its own change point

This belongs before anything else, because it decides whether this phase is a
source swap or a reopening of a tagged revision.

`rtl/bcmc_observer.v` carries no `ts_*` ports — the source is a *body* inside the
engine, not yet a module. But the engine was written for this phase and says so in
three places:

- `col_q` is **a register of its own, not an alias of `t_q`**, with the comment:
  "v2.0c replaces the identity with an affine or a shuffled source. That is a
  change to how `col_q` is *computed*, not to this state machine, which is why the
  column is a register of its own rather than an alias of `t_q`."
- the identity itself is two lines — `col_q <= t_next` on the edge, and
  `pi_at_start = 0` — carrying the comment "This is the one line a v2.0c source
  will legitimately change."
- the simulation assertion that the presented column equals the cursor is labelled
  "the identity source", so it is visibly a *source* assertion rather than an
  engine one.

So v2.0c **will** edit `rtl/bcmc_observer.v`, and that is not a reopening of
`v2.0a`. The tag freezes the engine's **contract** — its port list, its state
machine, its timing, its conformance suites — and the file documents the source
computation as this phase's deliverable. The distinction is worth making
checkable rather than leaving as rhetoric:

> **Every v2.0a verification path must pass unchanged after the source swap.** The
> same harnesses at the same revisions, over the same corpora:
> `sim/bcmc_observer_hw_test.cpp` against `obshw_edge.txt`, `sim/tb_observer.v`,
> the register window's three CTest cases, and the whole Icarus regression. The
> engine's own suite is the regression that proves the swap did not disturb the
> contract — and if it needs a single edit to pass, the swap is wrong rather than
> the suite.

That is the strongest available statement of "the seam held", and it is why those
two lines were written as two lines rather than inlined into the FSM.

It also leaves one decision that is about *where the boundary is drawn* rather
than whether the seam exists: does a source become a **module** (a new
`rtl/bcmc_src_*.v` instantiated inside the engine, matching §3.3's diagram, each
with its own testbench) or a **body** selected by a parameter? Section 5 carries
it; it is a specification decision, not an assumption this document may take.

---

## 2. The rate question, which v2.0c owns

### 2.1 Why it is this phase's, and not an inherited property

`docs/Output_Engine.md` §9.1 is explicit: the output engine **assumes no rate**. It
"does not assume that visits arrive at any particular rate, or that a source can
always supply one. It reacts to `visit_valid` however often that comes, and if a
buffered source stalls between banks the pins simply hold, which is correct rather
than degraded." Its corpus carries the case rather than asserting it: the
`invalid_while_driving` run spends seven of its nine cycles with no visit at all —
three with the context invalid and four after it returns — and the expected pins
simply stand still through all of them.

So the cadence a visit can arrive at is **not** constrained anywhere above this
phase, and therefore this phase owns it. A source slower than its trigger is not a
defect in v2.0c; it is a property v2.0c must state, bound and make testable. The
identity source had no rate to speak of, which is why the question appears now
rather than in v2.0a.

### 2.2 The measured constraint, as an input

A shuffled traversal cannot be computed on demand: `pi(t)` depends on the whole
history of swaps, so a bank of `N` entries must be *built* before the pass that
reads it. The measurement is already in the project — `validation/observer_hw.py`'s
`bank_fill_cycles(N, seed)`, asserted over recorded seeds and lengths by
`test_observer_hw.py` — and it says a bank of `N` entries costs, on average,
slightly more than `N` cycles to fill:

```text
   fill/N ~= 1.03 at N = 8      1.33 at N = 64      1.34 at N = 128
   approaching  2 ln 2 = 1.386  asymptotically
```

The arithmetic is in `docs/Hardware_Observer_Architecture.md` §9.3 and is not
repeated here. What matters for this phase is the shape of the conclusion:

```text
the source is legal exactly when     c_fill(N)  <=  N * R
for R = clocks per visit
```

**This enters v2.0c as an architectural input to investigate, not as a retroactive
requirement on a frozen block.** Nothing in v2.0b needs to change because a source
can be slow, and nothing in v2.0b should be re-read as if it had promised
otherwise. The measured bound is a constraint on *which sources can be offered at
which trigger rates* — a fact about a source selection, which is exactly the thing
this phase exists to design.

### 2.3 The requalification v2.0c forces, and has to decide

Here is the one place this phase touches a claim in a frozen document, and it is
recorded here rather than edited into it.

`docs/Output_Engine.md` §4.1 states:

> So there are **exactly two clocks from an accepted trigger to a changed pin**,
> and **exactly one clock from a presented visit to a changed pin**. Both are
> unconditional.

The second is about the output stage and survives anything a source does. The
first is a statement about the **engine plus its source**: it is true in v2.0a
because the identity source acknowledges unconditionally. A source that may
withhold `ts_ack` — which §9.3 of the architecture document explicitly allows, for
a bank that is not ready — makes the trigger→visit interval **conditional**.

This is not a contradiction. §4.1 bounds what an *application* may do to the
interval, and §7 of the same document says the source's cadence is not the output
engine's business. But it is a **requalification of a sentence in a frozen
document**, and this phase owns it. Two resolutions are defensible:

| Option | What it means | What it costs |
| --- | --- | --- |
| **(a) a source never withholds** | `ts_ack` is always high; a pass that cannot be served is never started, or is dropped before its first visit. The trigger→visit interval stays exactly one clock for every visit that happens | the wait moves to *whether* a pass begins, which is a different contract and needs its own statement |
| **(b) a source may withhold** | the frozen claim is requalified: the output stage adds exactly one clock, unconditionally, to whatever cadence arrives; the engine adds exactly one clock from a trigger to a visit *whenever the source acks* | a visit can be lost to a stalled source, so the loss has to be made visible (`SEED_UNDERRUN`) and its effect on O1 stated |

§9.3 of the architecture document sketched option (b) — "reject, do not underflow
... the source withholds `ts_ack`" — but this document does not choose, because the
choice changes what may be *claimed* about a frozen contract and so belongs in a
specification where it can be argued. What it does insist on is that the choice be
made explicitly, written down, and that whichever is taken, the frozen sentence it
requalifies is **cited** rather than quietly reinterpreted.

---

## 3. The three sources, and what each is *for*

| Source | `pi(t)` | Storage | Rate | Conformance |
| --- | --- | --- | --- | --- |
| identity | `t` | none | one visit per cycle | already pinned (`docs/Observers.md`) |
| affine | `(a t + b) mod N` | two registers | one visit per cycle | a new family; needs its own reference |
| Fisher–Yates | a shuffled table | `N` entries | ≤ one visit per ~2 cycles without a buffered lead | index-for-index with `sw/bcmc_observer.c` |

They are not three implementations of one thing, and the phase's work is
different for each. Saying so now avoids a specification written as though all
three were the same kind of problem:

**Identity is the control case, and it is not going away.** `docs/Observers.md`
argues that two reference traversals are the smallest number that can show the
choice matters *and* that it changes nothing proven; in hardware the identity
plays that role again, and §10.5 of the output engine's specification already
names the orthogonality check that needs it — one output engine, two sources,
identical summaries and different sequences. Removing it to "simplify" the
selector would delete the only available null hypothesis.

**Fisher–Yates is a conformance problem.** It must match something already pinned:
a 32-bit SplitMix generator, a rejection draw, downward swaps including `j == i`.
Reproducing that index for index in hardware buys the existing tables
(`sim/vectors/observer_prng.txt`, 11 seeds over 40 lengths) unchanged — a
considerable prize, and the reason this source is worth a bank at all. Its
obstacle is not arithmetic. It is time (§2.2).

**Affine is a design problem.** Nothing is pinned, so this source owns its own
reference, its own bijection argument *for arbitrary `N`* — not merely for powers
of two, which is the case the obvious construction handles — and its own mutation
battery. It is cheap in gates and streams at full rate, which is exactly why the
temptation to skip its reference has to be resisted: a new traversal is a new
family, and `docs/Observers.md` says a new family is specified before it is
implemented.

## 4. What is already fixed

Not to be re-litigated here. Each of these is inherited, and a specification that
quietly contradicts one is wrong rather than innovative:

- **O1 and O3 bind a source.** Over one pass it emits a bijection, and it is a
  deterministic function of its declared inputs. These are the observer
  contract's own requirements, and a hardware source is an observer.
- **The register window's existing semantics are frozen.** A source selector and
  seed registers are **additions**: they may not change what E1–E4 mean for the
  existing registers, and the frozen sections of `docs/Observer_Register_Map.md`
  must still pass their suite unmodified.
- **The engine's contract is frozen, and its own suites are the regression.** The
  port list, the state machine, the visit timing and the abort behaviour are
  v2.0a's. The file is edited at the two documented lines, and every v2.0a path
  must pass unchanged (§1.1). "The engine is unchanged" in §9.4 of the
  architecture document means the *contract*, not the file.
- **The output engine imposes no rate** (§2.1), so nothing downstream constrains
  the cadence a source picks.
- **The fill bound is a relation, not a number:** a source is legal exactly when
  `c_fill(N) <= N * R`. A source that violates it is legal only with a stated
  lead, and the lead is a configuration fact of the device.
- **The engine is a source *swap* target, not a source *host*.** Whatever a source
  turns out to be, `rtl/bcmc_observer.v`'s state machine, cursor and timing do not
  learn about it. If a source's design needs the FSM to change, that is a finding
  about the source rather than about the engine.

**One caveat on all six.** §9 of `docs/Hardware_Observer_Architecture.md` is
**architecture**, by that document's own status note: it fixes boundaries and a
recommendation, and it says each of its sections "will be given its own
specification before its RTL". So where §9 reads like a decision — the affine-first
position, the double-buffered lead, the seed registers — it is a recommendation
this phase may accept or reject, but only with reasons recorded. What it is not is
a licence to skip the specification step.

---

## 5. The agenda: what v2.0c's specification must decide

This is the phase's work list, and the next artefact answers it. Two of these are
the rows §11 of the architecture document explicitly left to a v2.0c
specification; the rest surfaced while opening this phase, and each is a decision
that changes what may be *claimed* rather than merely how something is built.

| # | Decision | Why it is not deferrable |
| --- | --- | --- |
| 1 | **The rate/withholding resolution** — §2.3's option (a) or (b) | it requalifies a sentence in a frozen document, so it must be argued and cited, not discovered in RTL |
| 2 | **Where the boundary is drawn** — a source as a module (`rtl/bcmc_src_*.v`, instantiated inside the engine, with its own testbench) or a body selected by a parameter (§1.1) | it decides whether "the engine is unchanged" is checkable by a port list or only by reading a diff |
| 3 | **The affine seed policy** — how `(a, b)` come from a 32-bit seed with `gcd(a, N) = 1` **for arbitrary `N`** | `N` is not necessarily a power of two, and the obvious construction (`a` odd) is only correct for one |
| 4 | **Whether Fisher–Yates must be index-for-index with `sw/bcmc_observer.c`** | the prize is the existing conformance tables; the cost is a bank and a lead. If the answer is no, the phase owes a *second* new reference instead |
| 5 | **The lead, as policy** — how it is expressed, whether the generator may write the inactive bank during a pass, and what a pass does when no bank is ready | it is the difference between a bound that is stated and one that is hoped for |
| 6 | **The register-window additions in detail** — selector encoding, `OBS_SEED`/`OBS_A`/`OBS_B`, `SEED_READY`/`SEED_UNDERRUN`, and whether a bad selector write is E1, E4, or a new class | a new error class edits a **frozen** register-map section, which is a decision in its own right |
| 7 | **The bank geometry** — the largest `N` the buffered source supports, and what happens above it | it must tell a consistent story with the register map's existing bounds |
| 8 | **Whether the affine source is pinned in `docs/Observers.md`** | it is a new traversal family, and that document says a new family is specified before it is implemented. Either it gains a third reference traversal, or the affine source owns its own reference document — but not nothing |
| 9 | **Whether a source may change mid-pass** | writing the selector mid-pass is a legal register write in every other respect, so "no" has to be stated and enforced |
| 10 | **Whether `SEED_READY` gates starting *any* pass, or only the buffered source** | it decides whether a stale seed changes the identity source's behaviour, which would be a change to a frozen one |

## 6. What v2.0c does not solve

- **v2.0d, the hardware applications.** The output engine's §9.2 describes a bank
  *around* it for more than `MAX_C` rows, and its §10.5 names the orthogonality
  check that needs this phase — but the applications are v2.0d's.
- **Concurrent sources, and arbitration between them.** One source at a time; the
  selector picks it. Several observers driving one application is a system-level
  question, not a source question.
- **Software visibility of a source's internals.** The registers are the
  interface; there is no window into a bank.
- **Anything in the construction.** The Core, the Context, the BCMC register map
  and the evaluator are untouched by this phase, exactly as they were untouched
  by the last two.
- **Index-for-index equality between the affine family and any software
  observer.** The affine family is new, so no such equality is claimed — which is
  precisely why decision 8 above cannot be waved away.

---

## 7. Verification, and what the model must be able to falsify

### 7.1 The artefacts this phase obliges

| Artefact | Role |
| --- | --- |
| `validation/traversal_sources.py` | the cycle model of the sources and the bank |
| `validation/test_traversal_sources.py` | the suite that exists to falsify the specification |
| `rtl/bcmc_src_identity.v`, `rtl/bcmc_src_affine.v`, `rtl/bcmc_src_shuffled.v` | the sources — the split and the names are the specification's to choose |
| `validation/gen_source_vectors.py` | the corpus, with the two guards |
| `sim/bcmc_src_*_test.cpp`, `sim/tb_src_*.v` | the two simulators |

None of them exists yet, and the names are provisional — including
`traversal_sources.py`, which is cheap to change now and expensive later. They are
written down because a phase that does not say what will hold it to account is a
description rather than a plan.

### 7.2 What the model must be able to falsify

Each of these is a way the specification could be wrong, and each is a case the
suite is obliged to drive:

1. **O1, for every source, over every `N` in range and every seed.** A pass is a
   bijection. The affine case is the one to attack hardest, because the coprime
   condition is easy to get wrong for `N` that are not powers of two — and the
   failure is invisible for the `N` that are.
2. **O3.** The same seed gives the same `pi` on every run, and the source consults
   nothing the caller did not hand it.
3. **The conformance claim** — index for index against
   `sim/vectors/observer_prng.txt` if decision 4 says yes, or against whatever
   reference decision 4 obliges instead. Either way there is a table, and the
   source is held to it before it is simulated.
4. **The fill bound as a relation.** For every `N` in the recorded range:
   `c_fill(N)`, the smallest `R` for which the source is legal, and the lead that
   follows. This is a *measurement*, so it is made rather than asserted — the
   precedent being `bank_fill_cycles`, whose measured figure replaced a
   document's estimate.
5. **The seed-change invalidation.** Writing a seed clears `SEED_READY`, and no
   pass begins on a bank built from a stale seed. This is the observer's analogue
   of the wrapper's "writing a weight clears `VALID`", and it is the same class of
   defect the *stale matrix* rule caught in v2.0a: a value surviving the thing it
   describes.
6. **The withholding rule** from §2.3, whichever option is taken — that a lost
   visit is *visible* rather than silent, and that it cannot break O1 without the
   status saying so.
7. **The composition invariant, for the third time.** Attaching a source must not
   change the engine. `observer_periph.replay` is reused, the engine's traces are
   compared, and — the strongest form — **every v2.0a suite passes unchanged**
   (§1.1). If a single v2.0a harness needs an edit, the seam did not hold.

### 7.3 The headline check, and where it lives

§10.5 of `docs/Output_Engine.md` names the hardware form of the check
`examples/README.md` performs in software, and it needs this phase:

```text
   one context, one output engine, one trigger schedule
            |
     identity source  -> pins sequence A, summary S
     affine source    -> pins sequence B, summary S
            |
   require  A != B   and   multiset(A) == multiset(B)   (P3)
   require both are column-wise rearrangements of M     (P1, P2)
```

It is the phase's strongest single verification, because it tests the *product*
claim — an observer contributes order and nothing else — on hardware rather than
in prose, and because it is the first check in this stream that needs two phases'
worth of frozen artefacts to be simultaneously correct.

### 7.4 The standing requirements

Unchanged from the previous three layers, and not optional: lint clean under both
simulators; `FORMAT` clean; **two independent simulators over one corpus**; the
corpus regenerated by `run_sim.sh` so a stale vector file cannot survive a change;
a **mutation battery** whose mutations are reported as counts, so the *suite* is
measured rather than the design; and the two corpus guards established last phase —
mandatory cases read back out of the file, and **discriminating power** against the
faults the model cannot plant. Cost claims are metered rather than asserted: "an
affine visit costs no bus access" is structural, because a source has no bus port,
whereas "a shuffled visit costs one bank read" is a claim about a count and should
be counted.

---

## Sources and status

This document follows `docs/Hardware_Observer.md` and
`docs/Hardware_Observer_Architecture.md` — the latter's §3.3 for the seam, §9 for
the architecture it recommends, and §11 for the two decisions it explicitly
deferred to a v2.0c specification. It also cites `docs/Output_Engine.md` §4.1 and
§9.1, where a frozen claim is requalified (§2.3) and a frozen absence relied on
(§2.1).

It leans on artefacts that already exist and are frozen or measured:
`rtl/bcmc_observer.v`, whose two documented lines are this phase's change point
(§1.1); `validation/observer_hw.py`'s `bank_fill_cycles`, the measured fill bound;
`docs/Observers.md`, for O1–O3 and the pinned software traversal family; and
`sim/vectors/observer_prng.txt`, the conformance table a hardware shuffle would buy.

**Status.** v2.0c is **opened, not specified**. Sections 1 to 4 are inherited
constraints plus one finding (§1.1) that the phase and its reviewer should agree on
before anything is built. Section 5 is the agenda the specification answers.
Nothing here should be read as a decision v2.0c has taken, except the three
statements this document expressly makes as its own:

1. **the rate question is this phase's**, because v2.0b has no rate assumption
   (§2.1);
2. **the fill bound enters as an architectural input to investigate**, not as a
   retroactive requirement on a frozen block (§2.2);
3. **`v2.0a`, `v2.0a-periph` and `v2.0b` stay exactly where they are**, and the
   test of the seam is that their suites pass unchanged (§1.1).

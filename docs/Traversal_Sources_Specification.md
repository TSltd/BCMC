# BCMC Traversal Sources — the v2.0c Specification

> **Status: specification, not frozen.** Sections 1 to 8 are the contract an
> implementation and its model are held to. They are not settled until
> `validation/traversal_sources.py` has run: three times now, executing a document
> has found defects *in the document* rather than in an implementation, and that
> is what the model is for. RTL comes after the model is green.
>
> `docs/Traversal_Sources.md` opened this phase and answered nothing; this
> document answers its §5 agenda — all ten items — and **amends one claim in that
> opening document** (§1.4). The three frozen checkpoints, `v2.0a` (221f976),
> `v2.0a-periph` (587933d) and `v2.0b` (5323eb0), are untouched, and §1.4 says
> precisely how far the amendment reaches.

---

## 0. Scope, and what this document decides

Ten decisions were the phase's agenda. Each is answered here, citable by section:

| # | Decision | Answered in |
| --- | --- | --- |
| 1 | withholding versus never-withholding | §2 — **never withhold**, with a declared rate bound |
| 2 | a source as a module, or a body selected by a parameter | §1 — **a module**, and a better seam than §3.3 sketched |
| 3 | the affine seed policy, `gcd(a, N) = 1` for arbitrary `N` | §4 |
| 4 | whether Fisher–Yates is index-for-index with the software family | §5 — **yes** |
| 5 | the lead, as policy | §7 |
| 6 | the register-window additions, and the error model | §6 |
| 7 | the bank geometry | §5.3 |
| 8 | whether the affine source is pinned in `docs/Observers.md` | §4.5 — **no**, and why |
| 9 | whether a source may change mid-pass | §6.3 — **no** |
| 10 | whether `SEED_READY` gates any pass, or only the buffered source | §7.2 — **any pass**, uniformly |

Two of these are coupled, and the coupling is why both are answered early.
**Never withholding** (§2) removes the need for an acknowledge in the seam, and
that in turn makes the honest seam **two wires rather than four** (§1). The
surviving architecture is therefore simpler than the one §3.3 of
`docs/Hardware_Observer_Architecture.md` drew — and simpler in a way that
*strengthens* a frozen contract instead of requalifying it.

---

## 1. The seam, as realized

### 1.1 Two wires, and no handshake at all

```text
        +-------------------+       ts_t        +----------------------+
        |  bcmc_observer    |------------------>|  traversal source    |
        |  (engine + cursor)|                   |  identity            |
        |                   |<------------------|  affine              |
        +-------------------+       ts_pi       |  shuffled            |
                                               +----------------------+
```

| Signal | Dir | Width | Meaning |
| --- | --- | --- | --- |
| `ts_t` | engine → source | `VAL_W` | the step index the engine is asking about, `0 .. N-1` |
| `ts_pi` | source → engine | `VAL_W` | `pi(ts_t)` |

`ts_req` is gone because **`ts_t` *is* the request**: a combinational source has
nothing to be asked for beyond the index it is asked about. `ts_ack` is gone
because §2 decides a source never withholds, so there is nothing for it to say.
What remains is a *function* — the step index in, the column out, same cycle.

That is not a loss of rigour; it is the same shape as the evaluator. "Evaluation
has no handshake" is a project rule because `M(i, j)` is a function of its
arguments, and a traversal source, once `t` is bound, is now a function of its
arguments too. The handshake §3.3 defended was a consequence of the withholding
behaviour §9.3 sketched, and removing that behaviour removes the handshake with it.

**A bank read is combinational once the bank exists.** That is what lets the
shuffled source fit the same shape as the other two: the *fill* costs cycles
(§5.2), but the *read* is an indexed read of a register file, so `pi(t)` is
available in the cycle it is asked for. The fill bound therefore constrains the
**lead**, not the visit timing — which is why §7 states it as a configuration
inequality rather than a per-visit condition.

### 1.2 Three shapes were available, and why not the other two

| Shape | Engine's port list | Runtime selection | Verdict |
| --- | --- | --- | --- |
| **body, parameter-selected** | unchanged | no | rejected: no runtime selector, so it contradicts §9.4 of the architecture document without a reason to |
| **module, muxed inside the engine** | +1 input (the selector) | yes | workable, but each source becomes untestable without elaborating the engine — and the engine still *contains* traversal |
| **module, seam exported** (chosen) | +1 input (`ts_pi`), +1 output (`ts_t`) | yes, in the window | the engine contains **no** traversal at all, and each source is a standalone module with its own testbench |

The third is chosen because it is the only one that makes this phase's own maxim
literally true rather than approximately true: "the engine is a source *swap*
target, not a source *host*" (`docs/Traversal_Sources.md` §4). Under it the engine
contains no identity source, no selector, no mux and no bank. It asks what `pi` is
at a step and is told — which is the smallest thing it could be asked to do, and
the only thing it ever needed.

### 1.3 Where the sources live, and what the engine loses

The three sources and their mux are instantiated **beside** the engine, in the
register window (`rtl/bcmc_obs_wb.v`), which is where the selector and the seed
registers have to be anyway (§6). The engine loses exactly two things:

```text
    was:   wire [VAL_W-1:0] pi_at_start = 0;      // "pi(0), the identity"
           col_q <= t_next;                       // "the identity source"
           col_q <= pi_at_start;                  // at a start

    is:    assign ts_t = t_next;                  // the step being asked about
           col_q <= ts_pi;                        // at a start AND on a trigger
```

`pi_at_start` disappears because it was the identity answering `t = 0`, and the
source answers that now: at a start `t_next` is `0`, so `ts_t` is `0` and `ts_pi`
is `pi(0)` — the same value, obtained the same way as every other step, with no
special case. And the simulation assertion that the presented column equals the
cursor was always an assertion about the *identity source*; it moves to
`rtl/bcmc_src_identity.v`, where the identity now lives.

Nothing else in the engine changes: not the state machine, not `visit_q`, not the
one-clock visit timing, not the abort, not the column-is-a-register-of-its-own
decision that made all of this cheap.

### 1.4 The amendment to this phase's opening document

`docs/Traversal_Sources.md` §1.1 says the engine's port list is frozen and that
"the file is edited at the two documented lines, and every v2.0a path must pass
unchanged". **The second half is right. The first half is wrong.** The engine's
port list gains two ports, because a runtime source selector cannot cross into a
module that has no source input — and §1.2 explains why the two shapes that avoid
the ports are worse.

What the obligation becomes, and what it was always *for*:

> **Wiring the seam to the identity reproduces v2.0a exactly.** `rtl/bcmc_obs_wb.v`
> instantiates `bcmc_src_identity` and connects `ts_t` to `ts_pi` through it; with
> that wiring, every v2.0a expectation, corpus and check is unchanged. The
> harnesses change only by *wiring the two new ports*, never by altering an
> expected value — and the v2.0a suites re-run as the regression that proves it.

The substance of §1.1's claim survives intact: the engine's **behaviour** is
unchanged, and that is checkable against corpora written before the seam had a
body. Its letter — "the port list is frozen" — was an over-statement in the
opening document, and correcting it here rather than discovering it in RTL is the
point of writing specifications first.

The same correction applies, more weakly, to the frozen **v2.0a harnesses**: two
files (`sim/bcmc_observer_hw_test.cpp`, `sim/tb_observer.v`) gain a wire. §10.3
requires that this be the *only* kind of change they receive, and the model's
composition invariant (§10.1) is what makes that checkable rather than promised.

---

## 2. Decision: a source never withholds

### 2.1 The decision

`ts_ack` is not merely unused; the behaviour it existed for is **refused**. A
source presents `pi(t)` in every cycle the engine asks, or it is not a legal
configuration. There is no stall, no wait state, no conditional visit, and no path
by which a source can delay the engine.

### 2.2 What it buys: a frozen claim that needs no requalification

`docs/Traversal_Sources.md` §2.3 identified the cost of the alternative —
`docs/Output_Engine.md` §4.1 says there are "exactly two clocks from an accepted
trigger to a changed pin ... both unconditional", and a source that may withhold
makes the first half conditional. That requalification was available and would
have been defensible, and **this specification declines to spend it.**

With §2.1, the chain is: a trigger accepted at edge `k` schedules a visit for
cycle `k+1`; `ts_t` is `t_next` combinationally; the source answers
combinationally; `col_q` registers at edge `k+1`; the output engine presents it in
`k+2`. Exactly two clocks, unconditional, and `docs/Output_Engine.md` §4.1 stands
unedited and unrequalified. A frozen document that survives a new phase without a
correction is worth more than a source that is allowed to be late.

### 2.3 The cost, and how it is paid

The cost is real: **a source that can never withhold must be ready**, and for the
buffered source that means the bank must exist before the pass that reads it. Two
mechanisms pay for it, and neither is in the datapath.

**A declared rate bound, as a precondition.** This is `rtl/bcmc_cell.v`'s idiom
applied again: the module computes a defined result for whatever it is given and
states the conditions under which the result is *meaningful* — "these are
preconditions, not behaviours ... Outside the preconditions the result is
meaningless but still defined." The buffered source's precondition is that the
trigger is not faster than the lead can service (§7.3). Exceeding it is outside
the specification, and the device does not try to repair it.

**A diagnostic that reports a violated precondition.** `SEED_UNDERRUN` in
`OBS_STATUS` is set when the generator's own cycle counter shows that the next
bank was not ready at a pass boundary. It cannot prevent the violation — that is
what "never withhold" means — but it makes a misconfiguration **visible** instead
of silent, which is the difference between a precondition and a guess. It is the
same role the transaction sequences give `err`: report, do not paper over.

### 2.4 What this rejects, and why

`docs/Hardware_Observer_Architecture.md` §9.3 item 3 sketches the alternative:
"reject, do not underflow ... the source withholds `ts_ack`, and — because a
stalled visit can swallow a trigger — the source also reports `SEED_UNDERRUN`."
That is a coherent design and this document rejects it, on three grounds:

1. **It makes the interval a function of the source.** The architecture's own
   reason for the seam's handshake was that it "delays *which column is asked
   about*, never *how long the answer takes*" (§3.3). Withholding makes it delay
   *how long the answer takes*, which is the distinction the handshake was
   justified by.
2. **It needs the engine's FSM to change.** A withheld visit means the engine does
   not advance, which means a wait state or a gating condition — and
   `docs/Traversal_Sources.md` §4 says a source that needs the FSM to change "is a
   finding about the source rather than about the engine".
3. **It costs a frozen sentence.** §2.2.

`SEED_UNDERRUN` survives the rejection, as a diagnostic rather than a stall — so
the status bit §9.3 wanted is kept, and only the stalling is refused. §9 of the
architecture document is architecture by its own status note, so rejecting its
recommendation with reasons recorded is exactly what that document says should
happen.

---

## 3. The identity source

```text
    assign ts_pi = ts_t;
```

Nothing else: no state, no seed, no bank, always ready. It is the control case
(`docs/Traversal_Sources.md` §3) and it has a second job that makes it
load-bearing rather than merely tidy — **it is the wiring the v2.0a suites are
re-run through** (§1.4). A build that selects the identity source is, at the seam,
exactly the v2.0a engine, and that is what makes "the seam held" a measurement
rather than a claim.

It is `rtl/bcmc_src_identity.v`, and it is where the "the presented column equals
the cursor" assertion now lives, because that assertion was always about the
identity rather than about the engine.

## 4. The affine source

### 4.1 The traversal

```text
    pi(t) = (a t + b) mod N,      1 <= a <= N-1,  gcd(a, N) = 1,  0 <= b <= N-1
```

Bijective for free: `t -> a t + b (mod N)` is invertible exactly when
`gcd(a, N) = 1`. That is the whole of the argument, and it is standard — which is
why the interesting part of this section is not the mathematics but the
*construction* of `(a, b)` from a seed (§4.3) and the fact that the arithmetic
needs no multiplier (§4.2).

### 4.2 It is the project's own idiom, and needs no multiplier

Computing `a t mod N` from scratch would need a real reduction — a divider, or a
Barrett step, or `t` sequential additions. Each is a poor fit: the project has no
divider anywhere, and it says so ("The Wrap is not a Division", `rtl/bcmc_cell.v`).

None of that is necessary, because the step is **incremental**:

```text
    pi(t+1) = (pi(t) + a) mod N
    pi(0)   = b
```

so the sequence is produced by keeping one register and advancing it by a constant
— one adder, one comparator and one conditional subtract, which is *exactly* the
Core's prefix accumulator and *exactly* the Evaluator's wrap. `a` is a constant
register, `b` an initial value, and no multiply appears anywhere.

The realisation follows, and the detail that matters is the width:

```text
    wire [VAL_W:0] inc = r_q + a;                       // VAL_W+1 bits
    wire [VAL_W-1:0] nxt = (inc >= N) ? inc - N : inc;  // the wrap, again
    assign ts_pi = (ts_t == 0) ? b : nxt;
```

**`inc` is `VAL_W + 1` bits, not `VAL_W`.** With `r_q <= N-1` and `a <= N-1` the
sum can reach `2N - 2`, which overflows `VAL_W` bits for any `N` above
`2^(VAL_W-1)`. Reducing in `VAL_W` bits would produce a wrong column for large `N`
and a *correct* one for all the small `N` a testbench is likely to use first,
which is the worst possible shape of bug. It is called out here so it is called
out in the RTL.

### 4.3 The seed policy, pinned exactly

`docs/Traversal_Sources.md` §5 item 3 asked how `(a, b)` come from a 32-bit seed
with `gcd(a, N) = 1` **for arbitrary `N`**. The obvious answer — `a` odd — is
correct only for `N` a power of two, and "arbitrary `N`" is the whole point of a
generator whose modulus is a weight-vector length.

The construction reuses the primitives `docs/Observers.md` already pins, so the
affine family is deterministic in a specified way rather than in a new one:

```text
    state <- seed
    repeat:  a <- uniform(N-1)          // the pinned rejection draw
             until gcd(a, N) == 1       // rejected draws are consumed, not undone
    b <- uniform(N-1)
    pi is then (b + a t) mod N
```

- **The draw order is part of the specification.** `a` is drawn — with its
  rejections consumed from the stream — and then `b` is drawn. An implementation
  that draws them the other way produces a different `pi` from the same seed and
  would pass every property test except index-for-index agreement with the
  reference.
- **Termination is certain, and the cost is bounded in expectation.** The density
  of integers coprime to `N` is `phi(N)/N`, which is `> 1/2` for even `N` and
  never much below `0.21` for the small cases with the most factors (`N = 2310`
  gives `480/2310`); the expected number of draws is therefore at most about five.
  There is no seed for which the loop fails to terminate, so there is no forbidden
  seed — the same property `docs/Observers.md` asked of the generator itself.
- **`gcd(a, N) = 1` is computed, not assumed.** A small Euclid over `VAL_W`-bit
  values, sequentially, once per seed — *not* in the per-visit path. It is
  deliberately allowed to take cycles here because it happens once, off the
  critical path, while `pi(t)` must be answerable every cycle.
- **`a = 1` is not excluded.** It yields `pi(t) = (b + t) mod N`, which is a
  rotation of the identity — a legal bijection, one draw out of many, and
  excluding it would bias the family for no benefit. `a = 0` cannot occur, since
  `gcd(0, N) = N != 1` for `N >= 2`, and for `N = 1` the loop's first draw is
  `uniform(0) = 0` with `gcd(0, 1) = 1`, which is correct: the only permutation of
  a one-element set.

### 4.4 It is not instantly ready, and that is part of the contract

The affine source keeps `a_q`, `b_q`, `r_q` and `t_last_q`, and `(a_q, b_q)` are
**computed from the seed**, off the per-visit path: the rejection draws and the
Euclid take as many cycles as they take. So `SEED_READY` is **not** permanently
high for this source either — it clears when a seed or a source is written and
sets once `(a, b)` exist (and `a` is known coprime to the current `N`).

Two consequences worth stating, because both shape a driver:

- **The seed-load latency is unbounded in principle and handled by polling.**
  The number of draws is a random variable (`phi(N)/N` acceptance, expected cost
  at most about five draws), so no cycle count is promised. Software polls
  `SEED_READY` — which is exactly what a status bit is for, and is not the `T1`
  mistake: nothing here depends on a trigger source advancing.
- **A seed change invalidates readiness for *every* source**, uniformly (§7.2),
  even the identity's, whose readiness cannot fail. That looks wasteful for the
  identity and is deliberate: a source-dependent rule would make E4
  source-dependent, and a rule that is uniform is a rule that can be stated once.

### 4.5 Why the affine family is *not* pinned in `docs/Observers.md`

Decision 8 asked whether the new family must appear there. **It must be pinned
somewhere; it is pinned here**, and `docs/Observers.md` is left alone.

That document says a new traversal "belongs in `sw/bcmc_observer.{h,c}`,
specified here first, and every application acquires it for free", and it argues
that a third *reference observer* would "add no obligation that these two do not
already discharge". Both sentences are about **software** traversals inherited by
applications. A hardware source is not inherited by software applications — it is
selected by a register bit and answers on a wire — so pinning it in
`docs/Observers.md` would create an obligation (a C implementation, a C conformance
table) that nothing discharges. The rule "a new family is specified before it is
implemented" is honoured; the *location* differs because the family does.

The affine reference is therefore §4.1–§4.3 above plus
`validation/traversal_sources.py`, and its conformance table is generated by this
phase's own generator rather than borrowed from `observer_prng.txt` — which is
exactly the distinction §5 draws for the shuffled source, where borrowing *is*
available.

---

## 5. The shuffled source

### 5.1 Index for index: yes

Decision 4 is **yes**. The source reproduces the family `docs/Observers.md` pins —
the 32-bit SplitMix generator, the rejection draw, and downward swaps including
`j == i` — exactly, index for index, for the same seed and `N`. The prize is the
existing conformance table: `sim/vectors/observer_prng.txt`, 11 seeds over 40
lengths, written by `validation/gen_observer_vectors.py` and already holding
`sw/bcmc_observer.c` to account. A hardware source that reproduces it needs **no
new reference for the permutation family at all**, which is why the bank is worth
paying for.

The generator is, deliberately, a *sequential* one: `pi(t)` cannot be computed on
demand, because the state after `t` swaps depends on the whole history. That is
the entire reason this source is buffered and the affine source is not.

### 5.2 The bank, and why the fill constrains the lead rather than the timing

```text
    building one bank of N entries
      N-1 shuffle steps, each: >= 1 SplitMix next(), plus a rejection retry
                                (< 2 draws on average), plus one bank write
      =>  measured  ~1.4 N cycles     (1.03 N at N = 8, 1.34 N at N = 128,
                                       approaching 2 ln 2 = 1.386)
```

The measurement already exists — `validation/observer_hw.py`'s
`bank_fill_cycles(N, seed)`, asserted over the recorded seeds and lengths by
`test_observer_hw.py` — and this document does not re-derive it.

What matters is where that cost lands. The bank is **read** combinationally, so a
pass at full rate costs `N` cycles for `N` visits regardless of the fill; the fill
runs *in the background*, filling the inactive bank while the active one is read.
The only question is whether it finishes in time, and that is a statement about
the **lead** (§7), not about the visit timing. §1.1 is what makes this work: a
combinational read at the seam is why a slow bank does not become a slow visit.

### 5.3 The geometry

`BANK_N_MAX` is a parameter of the source, **default 256**, and it is not a
suggestion about the register map's bounds — `MAX_C` bounds *rows*, `N` is the
row *length*, and the two are independent numbers that happen to be in the same
document.

Above `BANK_N_MAX`, the source simply never reports itself ready: `SEED_READY`
stays clear for that `(source, N)`, and START is refused (§6.2). There is no
error class for it, no truncation, and no attempt to serve a partial bank. A
configuration the device cannot serve is a configuration it will not start, which
is the same discipline the engine already applies to `N = 0`.

### 5.4 The lead, and a correction to the opening document

`docs/Traversal_Sources.md` §2.2 carries the architecture document's relation — "a
source is legal exactly when `c_fill(N) <= N * R`" — and §9.3 item 2 of that
document reads the lead as `ceil(c_fill(N) / (N * R))` passes. The first is right
and the second describes a **deficit**, not a cure, and the difference matters.

The generator produces one bank per `c_fill(N)` cycles and a pass consumes one
bank per `N * R` cycles. That is a *production-versus-consumption* comparison, and
a lead cannot change it: it only decides how long the buffer forestalls the
inequality. A lead of `L` passes buys `L` passes of reserve, not a
lower sustained rate.

```text
   sustained legality     N * R  >=  c_fill(N)          one fill per pass, on average
   start-up latency       LEAD * c_fill(N)              banks filled before the first pass
   jitter absorbed        LEAD passes of accumulated deficit
```

So the honest statement of the shuffled source's rate is the table's own:
**about one visit per two clocks**, because `c_fill(N)/N` is ~1.03 at `N = 8` and
rising toward `1.386`. `R = 1` is not reachable by a Fisher–Yates source at all —
`N` cycles of pass cannot fund `1.4 N` cycles of fill — and no lead configuration
makes it reachable. This is a *stronger* and simpler statement than the opening
document's, and it is the one the model must check: for every `N` in range, the
smallest `R` for which `N * R >= c_fill(N)`, and the resulting sustained rate.

`LEAD` is therefore a parameter with **default 2**, and its job is the other two
rows: it sets the latency from a seed write to a servable bank, and it absorbs the
*variance* of a fill whose draw count is itself random. It is not a knob that
trades correctness for speed — a lead too small cannot be compensated by a slower
trigger.

---

## 6. The register window, and the error model

### 6.1 What is added

| Register | Address | Access | Contents |
| --- | --- | --- | --- |
| `OBS_CTRL` (extended) | `0x00C` | RW | bits `[5:4]` = the source selector |
| `OBS_SEED` | `0x020` | RW | the 32-bit seed |
| `OBS_A` | `0x024` | RO | the `a` the affine source derived, for inspection |
| `OBS_B` | `0x028` | RO | the `b` it derived |
| `OBS_STATUS` (extended) | `0x010` | RW1C | bit 3 = `SEED_READY`, bit 4 = `SEED_UNDERRUN` |

The selector is encoded `0` = identity, `1` = affine, `2` = shuffled, and any
other value is refused as **unmapped (E1)** — it names a source that does not
exist, which is what E1 already means.

**`OBS_A` and `OBS_B` are read-only and are not decoration.** They make §4.3's
construction *inspectable*: a driver that writes a seed can read back the pair the
hardware derived and check the affine sequence without simulating the device. It
is the same principle as `OBS_CAPS` reporting geometry, and it costs two registers
and no datapath.

### 6.2 E4 gains one conjunct, and the frozen sentence is cited

`docs/Observer_Register_Map.md` §6 states E4 as: START refused unless
`!RUNNING & VALID & N >= 1`; STEP refused unless RUNNING. This document **extends
that condition by one conjunct**:

```text
    START is refused unless   !RUNNING  &  VALID  &  N >= 1  &  SEED_READY
```

citing the frozen sentence rather than reinterpreting it. It is an extension of
E4 rather than a fifth class, because it is the *same shape*: E4 is "refused
because the engine cannot accept this control write", and a source that cannot
serve the pass is another way that is true. Adding E5 would record a new reason
while claiming there was a new kind of thing, and there is not.

For the identity source this cannot bite — its readiness cannot fail (§7.2) — so
for a build that never selects another source, E4 behaves exactly as frozen.

### 6.3 Mid-pass writes are refused, and the selector is carried like `ONESHOT`

A source change or a seed change while a pass is **RUNNING** is refused (E4's
shape). The reason is not tidiness: changing the source mid-pass would restart or
alter the sequence partway, so the pass would stop being a bijection and O1 would
be broken by a *register write* — the one way a write could do real damage to a
proven property.

There is a trap here, and the register map already documents its twin. **A
`OBS_CTRL` write sets every field it has**, so every `STEP` carries the selector
along with `ONESHOT` — the same arithmetic that makes `STEP | ONESHOT` necessary
for a one-shot pass (`T1`'s audit). A driver that re-states the *current* selector
is accepted; one that changes it while RUNNING is refused. The rule is uniform
with `ONESHOT`'s and is stated in the same place for the same reason.

`OBS_SEED` is refused while RUNNING for the same reason: it would invalidate
readiness in the middle of the pass that readiness had already admitted.

### 6.4 A frozen corpus constrains where a new register may live

`sim/vectors/obswb_edge.txt` — frozen with `v2.0a-periph` — probes addresses
`0x01C`, `0x040`, `0x3FC` (reads) and `0x01C`, `0x400` (writes) and **expects
`err`** for every one, because in v2.0a they are unmapped. A v2.0c register placed
at any of them would be acknowledged, and the *frozen* register-window suite would
fail — a new feature breaking an old test by occupying ground the old test used as
empty.

So the additions live at `0x020`, `0x024` and `0x028`, and the rule generalises:

> An addition to this window may not occupy an address that the frozen corpora
> probe as unmapped, and those four addresses are **reserved permanently
> unmapped** so that they stay valid probes.

This is a new kind of coupling in this project — a frozen *test* artefact
constraining a future register map rather than the other way round — and it is a
good one: it means the frozen suite keeps testing what it was written to test.

### 6.5 No new error class

The additions reuse the four existing conditions and invent nothing:

| Situation | Condition | Why not a new class |
| --- | --- | --- |
| a selector naming a source that does not exist | **E1** | E1 is "inside the region and not mapped"; a source that does not exist is not a register |
| `OBS_A`/`OBS_B` written | **E2** | they are read-only, like `OBS_ID` |
| a wrong `sel` on any of the new registers | **E3** | unchanged |
| START with an unready source; a selector or seed write while RUNNING | **E4** | §6.2 and §6.3 |

---

## 7. Readiness, and the rate bound

### 7.1 What `SEED_READY` means

> `SEED_READY` is high when the **selected** source can serve the current
> `(source, N, seed)` configuration, and low otherwise.

It clears on a write to `OBS_SEED` or to the selector, and it sets again when the
selected source reports itself ready. The recovery time is the source's business
and is therefore source-dependent — one cycle for the identity, a few for the
affine (once `(a, b)` exist), `LEAD * c_fill(N)` for the shuffled source.

### 7.2 The rule is uniform, and that is decision 10

`SEED_READY` gates **START for every source**, not only the buffered one. The
alternative — a source-dependent gate — would make E4 source-dependent, and a
condition that changes with a register bit is a condition nobody can state once.

The identity's readiness cannot fail, so for it the only effect is that a seed
write costs one cycle of not-ready before START is accepted again. For a build
that never selects another source that is invisible in every frozen sequence; it
is stated because it *is* a behaviour, and a behaviour that only shows up under a
configuration nobody tested is exactly what this project writes down.

### 7.3 The rate bound, and the good news about it

The shuffled source's sustained rate is `N * R >= c_fill(N)` (§5.4) — about one
visit per two clocks. Exceeding it has a specific, bounded consequence, and this is
where §2.1 pays off a second time:

> **On underrun the source repeats the last complete bank.** It does not serve an
> incomplete one, and it does not stall.

That single choice means **O1 holds unconditionally**, for every source, in every
configuration, including a misconfigured one. Repeating a permutation is still a
bijection; serving a half-filled bank would not be. So the rate bound is not a
correctness precondition at all — it is a **freshness** precondition, and
violating it degrades the traversal's smoothness rather than its validity, exactly
as running too fast would degrade any schedule. `SEED_UNDERRUN` reports it, sticky
until cleared through the RW1C bit like `DONE` and `ABORTED`.

That is a better contract than the opening document anticipated, and it is worth
naming as the reason not to fear the bound: the worst a too-fast trigger can do is
make the traversal repeat, loudly.

---

## 8. The interfaces, verbatim

### 8.1 Each source declares only what it uses

| Module | Parameters | Inputs | Outputs |
| --- | --- | --- | --- |
| `bcmc_src_identity` | `VAL_W` | `ts_t` | `ts_pi` |
| `bcmc_src_affine` | `VAL_W` | `clk`, `rst`, `N`, `seed`, `load`, `ts_t` | `ts_pi`, `ready`, `a_out`, `b_out` |
| `bcmc_src_shuffled` | `VAL_W`, `BANK_N_MAX`, `LEAD` | `clk`, `rst`, `N`, `seed`, `load`, `ts_t` | `ts_pi`, `ready`, `underrun` |

**The identity source has no clock and no reset**, and that is the point rather
than an oversight: `pi(t) = t` is a function, and a function of a function's
argument needs no time. It is the same statement `rtl/bcmc_cell.v` makes by having
no `clk` port, and it is why the identity is the natural wiring for re-running the
v2.0a suites (§1.4).

**No source takes a uniform interface it does not need.** The alternative — every
source taking `clk`, `rst`, `N`, `seed` and `load` so the mux is easier — would
leave the identity with four unused inputs and force a lint waiver to hide them.
The project has exactly one such waiver, in `rtl/bcmc_obs_wb.v`, and it exists
because a document *chose* a reserved field; it is not a technique. Each source
declares its own interface and the window's mux ties the differences.

### 8.2 The engine, and the one place it changes

```text
    module bcmc_observer #(...) (...,
        output wire [VAL_W-1:0] ts_t,      // NEW: the step being asked about
        input  wire [VAL_W-1:0] ts_pi,     // NEW: pi(ts_t)
        ...);

    assign ts_t = t_next;                  // was: the identity, computed here
    ...
    col_q <= ts_pi;                        // was: col_q <= t_next / pi_at_start
```

### 8.3 The window, which owns the selector and the mux

`rtl/bcmc_obs_wb.v` instantiates all three sources, muxes `ts_pi` and `ready` by
the selector, ORs `underrun`, and pulses each source's `load` when `OBS_SEED` or
the selector is written. It is the right place for all of it because the selector
and the seed registers already live there (§6.1), and because the alternative —
putting them in the engine — is what §1.2 rejected.

---

## 9. What v2.0c does not solve

- **v2.0d, the hardware applications.** The output engine's §10.5 names the
  orthogonality check that needs this phase, but the applications are v2.0d's.
- **A DMA-fed or bus-fed source.** A source that fetches its bank over a bus would
  need a memory system, and this phase's sources are self-contained.
- **Several sources at once, or arbitration between observers.** One source at a
  time, selected by a register bit.
- **Software visibility into a bank.** `OBS_A`/`OBS_B` expose the affine
  construction (§6.1) and nothing exposes a shuffled bank; a bank is a traversal,
  and `docs/Observers.md`'s prohibitions are not suspended because it is in
  hardware.
- **Any change to the software family.** `sw/bcmc_observer.{h,c}`,
  `docs/Observers.md` and `observer_prng.txt` are untouched — the shuffled source
  is held *to* them, not added to them (§5.1, §4.5).
- **The FPGA work.** Tang Nano remains deferred, and nothing here changes that.

---

## 10. Verification, and what the model must be able to falsify

### 10.1 The artefacts

| Artefact | Role |
| --- | --- |
| `validation/traversal_sources.py` | the cycle model of the three sources, the bank and the seed policy |
| `validation/test_traversal_sources.py` | the suite that exists to falsify this document |
| `rtl/bcmc_src_identity.v`, `rtl/bcmc_src_affine.v`, `rtl/bcmc_src_shuffled.v` | the sources |
| `validation/gen_source_vectors.py` | the corpus, with the two guards |
| `sim/bcmc_src_*_test.cpp`, `sim/tb_src_*.v` | the two simulators, per source |

The model comes first and the RTL after it, as in every previous phase.

### 10.2 What the model must falsify

1. **O1, unconditionally, for every source, over every `N` in range and every
   seed** — including under underrun (§7.3), because that claim is what makes the
   rate bound a performance statement rather than a correctness one.
2. **O3.** The same seed gives the same `pi`, and the source consults nothing else.
3. **Index-for-index agreement** for the shuffled source against
   `sim/vectors/observer_prng.txt`, before anything is simulated.
4. **The affine construction**: the draw order of §4.3, its termination, and the
   coprime guarantee **for arbitrary `N`** — the adversarial cases being `N` with
   many small prime factors (`210`, `2310`), where `phi(N)/N` is smallest and a
   naive construction fails, with powers of two as the control.
5. **The width trap of §4.2.** A test that only uses small `N` cannot see it, so
   the suite must carry an `N` above `2^(VAL_W-1)` and check the sequence against
   the closed form.
6. **The rate bound as a measurement**: for every `N` in range, the smallest `R`
   with `N * R >= c_fill(N)` and the sustained rate that follows — the
   `bank_fill_cycles` precedent again, a measurement replacing an estimate.
7. **Readiness**: clearing on a seed or selector write, setting per source, START
   refused while clear, and the identity's one-cycle recovery — including the case
   that looks like a bug and is not, a seed write delaying an identity pass.
8. **The repeat-on-underrun rule**: a pass served under underrun is still a
   bijection, `SEED_UNDERRUN` is set, and the *same* permutation is presented
   rather than a partial one.
9. **The composition invariant, third time.** Attaching a source must not change
   the engine, `observer_periph.replay` is reused, and **the v2.0a suites pass
   with only the wiring change** (§1.4). This is the check that the seam held.

### 10.3 The headline check

§7.3 of `docs/Traversal_Sources.md` names it: one output engine, one context, two
sources, and the sequences must differ while the summaries are identical. It needs
three phases' worth of frozen artefacts to be simultaneously correct, and it is
the first check in this stream to test a *product* claim on hardware.

### 10.4 The standing requirements

Unchanged and not optional: lint clean under both simulators; `FORMAT` clean; two
independent simulators over one corpus; the corpus regenerated by `run_sim.sh`; a
mutation battery reported as counts; and the two corpus guards — mandatory cases
read back from the file, and discriminating power against the faults the model
cannot plant. Costs are metered: "the identity and affine sources cost no memory"
is structural, while "a shuffled visit costs one bank read" is a count.

---

## 11. Decisions taken, and what remains

### Taken here

1. **A source never withholds** (§2.1), so `docs/Output_Engine.md` §4.1 stands
   **unrequalified** — the requalification the opening document anticipated is
   declined, not spent.
2. **The seam is `ts_t` out and `ts_pi` in**, with no `ts_req` and no `ts_ack`
   (§1.1), and the sources live beside the engine in the register window (§1.2,
   §1.3).
3. **`rtl/bcmc_observer.v` gains two ports**, and the opening document's "the port
   list is frozen" is corrected (§1.4); the obligation becomes "wiring the seam to
   the identity reproduces v2.0a exactly".
4. **The affine traversal is produced incrementally**, needing no multiplier and no
   divider (§4.2), with `(a, b)` from the pinned generator and draw, `a` by
   rejection on `gcd(a, N) = 1` (§4.3).
5. **The affine family is pinned here, not in `docs/Observers.md`** (§4.5).
6. **The shuffled source is index-for-index with the software family** (§5.1),
   buying `observer_prng.txt` unchanged; the fill constrains the **lead**, not the
   visit timing (§5.2).
7. **The shuffled source's sustained rate is ~one visit per two clocks**, and no
   lead makes `R = 1` reachable (§5.4) — a correction to the architecture
   document's lead formula.
8. **`BANK_N_MAX` (default 256) and `LEAD` (default 2)** are parameters, and an
   unservable `N` is a readiness failure rather than an error class (§5.3).
9. **A selector in `OBS_CTRL[5:4]`, and `OBS_SEED`/`OBS_A`/`OBS_B` at
   `0x020`/`0x024`/`0x028`**, avoiding the addresses the frozen corpus probes as
   unmapped (§6.1, §6.4).
10. **E4 gains `& SEED_READY`** and no new error class is invented (§6.2, §6.5);
    the selector is carried on every `CTRL` write like `ONESHOT`, and mid-pass
    source or seed changes are refused (§6.3).
11. **`SEED_READY` gates START uniformly for every source** (§7.2).
12. **On underrun the source repeats the last complete bank**, so **O1 holds
    unconditionally** and the rate bound is a freshness precondition (§7.3).
13. **Each source declares only the interface it uses** (§8.1), so no lint waiver
    hides an unused input.

### Remaining, and who decides

| Open question | Decided by | When |
| --- | --- | --- |
| Whether the reference build instantiates all three sources or parameterises one (`SOURCE`) for a trimmed synthesis build | the model, then the RTL | with the RTL, cheaply |
| The exact `BANK_N_MAX` and `LEAD` defaults | the model's measurement | with the model |
| The model's file name (`traversal_sources.py` assumed) | this document | now, cheaply reversible |
| Whether a source should be per *observer instance* rather than per window | a v2.0d or later specification | with applications |
| Whether `SEED_UNDERRUN` should refuse anything | nobody — a diagnostic by decision (§2.3) | settled |

### Status

Sections 1 to 8 are **specification**, and are not frozen until
`validation/traversal_sources.py` has run. Three findings are already on the
record: §2 declined to spend a requalification that was available, §5.4 corrects
one the architecture document made, and §6.4 records a coupling that was invisible
until the frozen corpus was read carefully. The model exists to find more, and the
RTL comes after it.

---

## Sources and status

This document answers the ten-item agenda of `docs/Traversal_Sources.md` §5 and
follows `docs/Hardware_Observer_Architecture.md` — §3.3 for the seam it reshapes,
§9 for the architecture it partly accepts and partly rejects, §11 for the
decisions it inherits. It cites `docs/Output_Engine.md` §4.1 (which it declines to
requalify), `docs/Observers.md` (O1–O3 and the pinned software family),
`docs/Observer_Register_Map.md` §6 (E4, extended by one conjunct and cited),
`rtl/bcmc_cell.v` (the preconditions idiom, and "The Wrap is not a Division"),
`rtl/bcmc_observer.v` (whose two documented lines are the change point), and
`validation/observer_hw.py`'s `bank_fill_cycles`.

**Status.** Specification, **not frozen**. The next artefact is
`validation/traversal_sources.py`, and it exists to prove this document wrong.

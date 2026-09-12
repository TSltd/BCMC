# BCMC Output Engine — the v2.0b Specification

> **Status: specification, not yet frozen.** Sections 3 to 7 are the contract: an
> implementation is held to them. They are not settled until something executes
> them. Following the rule that has governed every layer of this project, the
> next artefact is `validation/output_engine.py` — a cycle-level model derived
> from this document **in order to falsify it** — and `rtl/bcmc_out_engine.v`
> comes only after that model is green.
>
> That rule has paid for itself three times. Executing
> `docs/Transaction_Sequences.md` found a polling defect; executing
> `docs/Hardware_Observer_Architecture.md` found the conservation over-claim
> (F11) and the unresolved `start`/`done`/`trigger` overlap (F12); executing
> `docs/Observer_Register_Map.md` found `T1`'s missing trigger source and the
> `ONESHOT`-cleared-by-`STEP` trap. Every one of those was a defect in a
> *document*, found by its model, before any RTL existed. Nothing in this file
> should be treated as settled until the same has been attempted here.
>
> The same has now begun, and the record is kept the same way. **Writing
> `validation/output_engine.py` was enough to falsify part of §10.2** — the claim
> that every visit changes the pins, and the transition counts built on it — with
> an embarrassingly simple counterexample: set every weight to zero, which is
> legal for every `N`, and a three-visit pass produces three visits and no pin
> transitions at all. Building the suite that would check the list found two more
> defects: item 1's companion claim is *not checkable* (two visits can present the
> same pattern, so the pins may legitimately arrive at the current bits), and
> item 8's bound needs the gate and reset events in it, not just the visits. All
> three corrections are in place above, and the model and its suite are green
> against them: `validation/output_engine.py` (27 doctests) and
> `validation/test_output_engine.py` (1,828 exhaustive runs and gaps, 1,724
> patterns checked against the reference matrices, the composition rule, the port
> list, and a mutation battery that catches three planted bugs by name while
> accepting one correct alternative).
>
> What that does *not* yet establish: the model has not been held against an RTL,
> because there is none. On this project's record, a specification is what a model
> has failed to falsify *and* what an implementation has been held to. The first
> is now true here; the second is not.

---

## 0. Scope, and where this sits

This document specifies the **v2.0b output engine**: the block that turns an
observed column into a physical effect.

It is the second of the two halves the observer was deliberately split into.
`docs/Hardware_Observer_Architecture.md` §8 named it, fixed its boundary, and
said explicitly that it would be given its own specification before its RTL.
This is that specification. It does not move that boundary; it decides what is
behind it.

What it decides, and nothing else:

1. what the output engine **consumes** from the observation sideband;
2. what constitutes an **emitted application event**;
3. whether there is **back-pressure**;
4. whether output order is **observation order**;
5. whether `visit_valid` **is** the application event or merely its input;
6. what happens to the presented pattern when the **context becomes invalid**,
   when the pass **aborts**, and when the observer is **reset**;
7. what **`done`** means to an application;
8. where **one-shot versus continuous** is decided;
9. how the **traversal-source seam** stays on the other side of this block;
10. what v2.0b **deliberately does not solve**.

Where an implementation and this document disagree, this document is right and
the implementation is a bug — the same rule `docs/Observers.md` states for
observers. That is what makes the word "specification" mean something here.

### 0.1 What it is anchored to

This document adds no mathematics and no traversal. It is downstream of both,
and where it repeats an existing contract it names it:

| Contract | Document | What this document takes from it |
| --- | --- | --- |
| O1–O3, P1–P4, the four prohibitions | `docs/Observers.md` | that an application *chooses* a traversal and never implements one |
| The visit stream, its timing, its abort | `docs/Hardware_Observer_Architecture.md` §3, §5 | the six wires, and the one-clock rule |
| `column_bits` is `M(·, column)`, masked by construction | §4.3, and `rtl/bcmc_column.v` | that no output masking is required |
| No `NEXT_COLUMN`, evaluation has no handshake | `docs/Register_Map.md` | that output cannot reach back and ask for a column |
| `VALID` is the bit that means a matrix exists | `docs/Register_Map.md`, E4 | that a pattern from an invalidated matrix is **stale**, and acting on it is the same error the transaction sequences call incorrect |
| `Application × Traversal` is a product | `examples/README.md` | that v2.0b is an application, and the diff that checks the product has a hardware analogue |

---

## 1. What the output engine is

> An output engine reads the set of rows active in the column just visited, and
> drives that set onto a physical output until the next visit replaces it.

Three claims live in that sentence, and each is checkable:

**It reads a *set*, not a column index.** It is never told which column was
visited. This is deliberate and is discussed in §3.2.

**It drives, and *until* the next visit it drives nothing different.** The output
is a **held state**, not a pulse. `N` visits divide the trigger period into `N`
slots, and each slot presents one column. A row not in the active set is off for
that slot; it is not "unmentioned". See §4.3.

**It is an application.** It knows its purpose and nothing about how the order
was produced, exactly as `docs/Observers.md` requires: an application supplies
purpose and does not know how traversal is implemented.

### 1.1 What it is not — the four prohibitions, in hardware

`docs/Observers.md` lists four things an observer must not do. They were written
for software observers, and every one of them has a hardware form here. This
block is on the *application* side of the product, so it is bound by them
directly:

| Prohibition | Here |
| --- | --- |
| **must not hold the matrix** | it holds the last *pattern*, not `M`. It has no weights, no offsets, no `N`, and no way to evaluate anything. |
| **must not reach past its interface** | it has no bus, no register, no address and no error. It cannot read `column_bits` for a column of its choosing. |
| **must not allocate** | it has no storage it grows: one register of `MAX_C` bits is the whole of it (§4.3). Anything wider is a bank instantiated *around* it, not inside it (§9). |
| **must not be privileged** | nothing downstream may assume a traversal. This block cannot even *see* which traversal ran, so it cannot privilege one; and it is not permitted to become the place where a second one hides (§9). |

The fourth is the load-bearing one, and it is why `column` is not among the
inputs (§3.2) and why no traversal-source selector appears anywhere in §3.

---

## 2. The input contract: the observation sideband

### 2.1 The ports, verbatim

```text
            bcmc_observer  /  the sideband            bcmc_out_engine
   column_bits   [MAX_C-1:0] ───────────────────────>  column_bits_i
   visit_valid   [1]         ───────────────────────>  visit_valid_i
   valid         [1]         ───────────────────────>  valid_i
   done          [1]         ───────────────────────>  done_i
   C             [IDX_W-1:0] ───────────────────────>  C_i
   clk, rst      [1]         ───────────────────────>  clk, rst
                                                       pins_o [MAX_C-1:0] ──> GPIO
```

| Input | Source | Used for |
| --- | --- | --- |
| `column_bits_i` | the observer's own `bcmc_column` instance | the pattern |
| `visit_valid_i` | §3.7, the registered visit gated on `VALID` | the latch enable, and the application event (§4.1) |
| `valid_i` | the wrapper's `VALID`, the same wire the observer consumes | the **presentation gate** (§4.2, §6) |
| `done_i` | §3.8, coincident with the last visit of a pass | pass-boundary policy the application may apply (§5) |
| `C_i` | the wrapper's `C` | **not** the datapath — it checks that no masking is needed (§2.3) |

`valid_i` is the one addition to the boundary §8.1 drew, which listed four
wires. The reason is §6: the staleness rule cannot be stated without knowing
whether the matrix still exists, and §2.2 records why the abort *event* cannot
substitute for it.

### 2.2 What is not among them, and why

This is the part of the document that matters most, because these are the
absences that make the boundary real rather than decorative.

**Not `column`.** Not a column index, not a wire carrying one. An output engine
that knew *which* column it was looking at could encode "do X on column 0",
which is a statement about traversal — and it would be privileged: it would work
for the identity source and break for a permuted one. Nothing downstream may
assume a traversal (`docs/Observers.md`); refusing the index is how that is
enforced rather than promised.

**Not `N`.** The engine has no cursor to wrap and the output engine has no
schedule to count. `N` appears nowhere in this block. A per-row duty cycle
(a §9 extension) would divide the *trigger* period, which is a trigger
parameter, not an `N` parameter.

**Not the weights, the offsets, or the Context.** There is no evaluator here and
nothing to evaluate. The pattern arrives already evaluated.

**Not `running`.** Considered and rejected. It is not needed by §3.2's rule, and
an input that nothing needs is an invitation for a later revision to give it a
meaning. The one question it might answer — "has a pass ever run?" — is answered
by the pattern register itself.

**Not `aborted`.** Considered and rejected, and this one is worth recording
carefully, because it is the obvious first choice. An abort *event* looks like
the natural signal for "your pattern is now stale". It is not, for two reasons:

1. **It is not sufficient.** The engine aborts only while `RUN` (§3.10).
   Invalidate the context while the observer sits `IDLE` — after a one-shot pass
   has completed, say — and no abort occurs, yet the last pattern came from a
   matrix that no longer exists.
2. **It is slower.** `aborted` is registered; `valid` is a level. Gating on the
   level takes effect in the same cycle the matrix disappears, which is the same
   instinct §3.7 applied to `visit_valid`.

So the staleness rule is expressed with a *level* (§6), and no event wire is
needed to infer it.

**Not the bus.** There is no `wb_*` port on this block, no address, no register
and no error condition. It is not software-visible. An application reachable over
the same bus as the matrix would be a second path to `NEXT_COLUMN` by another
name.

### 2.3 `column_bits` is already masked, and `C` is here to check it

§8.2 step 1 of the architecture document says to latch `column_bits` "masked to
`C` bits; the bits at or above `C` are already zero (§4.3)". That is a claim
about `rtl/bcmc_column.v`, and it is true for a specific reason: the generate
loop forces `lane_weight = 0` for every lane `i >= C`, and a cell of weight zero
answers zero for every column. Lanes above `C` are not switched off; they are
asked a question whose answer is zero.

This document therefore does **no masking**, and says so rather than doing it
harmlessly. `MAX_C` is the width of the pattern; `C` is an input only so that the
block can *check* the claim:

```text
ifndef SYNTHESIS:
    assert (column_bits_i & ~low_mask(C_i)) == 0
```

`C` is not in the datapath. A mask would make the design correct by forgetting,
and the promise being checked — that a lane above `C` is never active — is one a
future revision of the projection could break silently. `bcmc_cell.v` declines to
check its own preconditions because a combinational checker can fire
mid-transition; this is a *registered* consumer with a settled input, so it may
check, and it is the only thing `C` is for.

---

## 3. The output contract

### 3.1 The application event

> An **application event** is a change in the pattern presented on `pins_o`.

Not a pulse, not a handshake, not a level on a status wire. The observable thing
an application does is *change what is driven*, and a visit is the only thing that
can cause such a change: a visit whose pattern differs from the one already
presented produces exactly one event, one clock later.

**Not every visit produces an event**, and an earlier draft of this document said
it did. Two consecutive visits may present the same pattern, and a visit may
present an empty one: set every weight to zero — legal, since `0 <= w_i <= N` —
and no visit changes anything at all. So "one event per visit" is false; what is
true is the one-way implication of §10.2 item 7. This definition is still the one
a meter can settle, but the meter is checked against the *value* on the pins
(§10.2 item 1), not against a count.

It follows immediately that `visit_valid` is **not** the application event. It is
the event's *input*, by one clock (§4.2). Conflating the two is the first mistake
available here, and §4.2 is where it is closed.

### 3.2 Two states: driving, and not driving

The output stage is in exactly one of two states at any time.

| State | Pins carry | Entered |
| --- | --- | --- |
| **driving** | the last latched pattern | at a visit, with `valid_i` high |
| **idle** | the configured idle level, on every pin | on reset, and whenever `valid_i` is low |

"Idle" is not a third behaviour bolted on; it is the *absence* of a pattern whose
matrix exists, which is §6's whole subject. With the default polarity (§9) the
idle level is deasserted, so the idle state is legible on the pins themselves and
needs no status wire to observe it.

There is no intermediate state, no queue, and no partially-driven pattern, so
there is **nothing that can be partially consumed** — a question §6 would
otherwise have to answer. The absence is structural: one register is the whole of
this block's storage.

### 3.3 Hold within a pass, and why a register is entailed

Between visits in a valid pass the pattern is *held*, and §8.2 gives the reason:
a visit is a **state**, not an event. `column_bits` is the set of consumers served
in this slot; a consumer not in the set is off for this slot, not "unmentioned".
Holding it is what makes `N` a division of the trigger period.

The register is therefore not an implementation convenience — it is what the
semantics require. A purely combinational output stage would present each pattern
for exactly one cycle and then present nothing, which is a pulse train, not a
schedule. Holding is the definition, so the register is entailed by it.

### 3.4 The output word

`pins_o` is `MAX_C` bits, bit `i` corresponding to row `i` — the same bit order as
`column_bits_o`, which is the same order as `bcmc_column`'s, which is the same
order `docs/Observers.md` fixes for `R(j)` so that two conforming observers
produce byte-identical output. The ordering convention is inherited, not
re-chosen.

An application whose physical outputs are fewer than `MAX_C` consumes a prefix.
One needing more than `MAX_C` rows is a §9 matter, and it is a *bank around*
this block rather than a widening of it.

### 3.5 Ordering

Output order is exactly observation order. There is no queue, no reordering
buffer and no priority, so the `t`-th application event in a pass corresponds to
`pi(t)`, and the sequence of patterns presented is the sequence of visits. O2's
row order *within* a pattern is inherited from the projection, and O1's
bijection is the source's business, not this block's.

This is worth stating even though it reads as a tautology, because the natural
"improvement" to an output engine is a buffer — to smooth timing, to decouple
rates, to ride out a stall. A buffer is a reordering device, and it would put a
selection between the traversal and the application that no document specifies and
no diff could check. §7's no-back-pressure rule and this section are the same
decision seen from two sides.

---

## 4. Timing

### 4.1 One clock, and no conditional path to it

The observer's contract is that a `start` or `trigger` accepted at edge `k`
becomes a visit during cycle `k + 1`, for exactly that cycle (§5.1 of the
architecture document). This document adds one clock to that chain and no more:

```text
   edge k        a trigger is accepted
   cycle k+1     visit_valid is high, column_bits is M(., pi(t))
   edge k+1      the pattern is latched
   cycle k+2     pins_o carries the new pattern
```

So there are **exactly two clocks from an accepted trigger to a changed pin**,
and **exactly one clock from a presented visit to a changed pin**. Both are
unconditional. There is no stall, no wait-state, no `ready` wire and no path by
which an application can lengthen or shorten either interval — because there is
no back-pressure (§7), the interval cannot become a function of the application.

That is the property worth having. It is what allows a caller to reason about the
trigger period and the output in the same breath, and it is what the observer's
own timing contract was designed to make possible.

### 4.2 `visit_valid` is the input to the event, not the event

`visit_valid` is high for exactly one cycle per visit. The pattern is latched at
the edge that ends that cycle, so the pins carry the pattern from the next cycle
onwards and keep carrying it. The **event** is the arrival of the new pattern,
not the strobe that caused it.

The presentation rule, stated once here and relied on everywhere below:

```
latch:      on visit_valid_i                        -> pattern_q
present:    pins_o  = pattern_q, gated by valid_i   -> driving, or idle
```

Two signals, two meanings, and they are not the same signal:

| | Means | Registered? |
| --- | --- | --- |
| `visit_valid_i` | "the observer is presenting a column now" | yes, one cycle (§3.7) |
| the gate on `valid_i` | "the matrix this pattern came from still exists" | no — a level |

The second is §6's subject. The point of separating them is the same point §3.7
makes a layer up: the *latched* value and the *presented* value are different
things, and a consumer is entitled to see the difference.

### 4.3 The final column

`done` is coincident with the last visit of a pass (§3.8), not one cycle after
it. So the last visit of a pass is an ordinary visit in every respect: it latches
a pattern, changes the pins, and `done_i` is high in the same cycle. Nothing in
the output engine treats it specially, and §5 says where a policy that wants to
would live.

After `done`, with a valid context, the last pattern stays latched and stays
presented until the next visit — which is `pi(0)` of the next pass, or the next
pass's first visit under a source that does not repeat. Holding across a pass
boundary is not a special case here; it is §3.3 with a longer gap.

---

## 5. Completion, one-shot and continuous

The output engine is **not** where one-shot versus continuous is decided. It
cannot be: it has no `trigger`, no `oneshot` and no way to stop or start a pass.
Both modes present it with exactly the same thing — a sequence of visits, with
`done` coincident on the last visit of each pass — and it behaves identically in
both.

Where the mode does live is already fixed and is not revisited here:

| Decision | Lives in | How it is set |
| --- | --- | --- |
| one pass, then stop | the engine's `oneshot` input (§3.4, §3.8) | `OBS_CTRL.ONESHOT`, carried on every write (`docs/Observer_Register_Map.md`) |
| stop a continuous pass | the engine's `rst` (§3.9) | `OBS_CTRL.RESET` |
| when the next visit happens | the trigger source (§3.5) | `OBS_TRIG`, or a v2.0c source |

What an application *may* do with `done` is apply **policy**: an application that
wants its outputs blank at the end of a one-shot pass deasserts them itself, on
`done`. That is application semantics (`docs/Observers.md`: what an observer does
with a visit, and whether it runs once or forever, is outside the observer
specification), and it belongs in the application rather than in this block. This
block's contribution is to make it *possible*: `done` is presented, and the
pattern is held rather than auto-cleared, so an application can choose either.

The default — no policy applied — is: the last pattern stays presented. That is
§3.3 and §4.3, and it needs no configuration to be the behaviour.

---

## 6. Invalidation, abort and reset

This is the sharpest decision in the document, and the one an application is most
likely to get hurt by if it is made carelessly.

### 6.1 The hazard

A pattern is derived from a matrix. If that matrix ceases to exist, any pattern
still being presented came from something that no longer exists — and unlike a
software observer, whose stale data is merely a stale number, an output engine
drives **actuators**.

The project has already taken a position on exactly this, one layer up. Writing a
weight clears `VALID`; the transaction sequences make returning the *old* matrix
after a weight change explicitly **incorrect**. An output engine that held its
last pattern across that event would be committing that same error with a heater
attached: the mains heater application (`examples/`) would keep its rows conducting
according to a schedule for a matrix that has been replaced.

So the rule below is not new policy. It is `E4`/`VALID` applied to pins.

### 6.2 The pattern cannot outlive its matrix

Two mechanisms, and both are needed — this is the part that is easy to get half
right.

```text
pattern_q  <= visit_valid_i ? column_bits_i : pattern_q;   // latch
on falling valid_i:   pattern_q <= 0;                      // destroy
pins_o      = pattern_q & {MAX_C{valid_i}};                // present, gated
```

**The latch** is §3.3.

**The gate** is combinational, so the pins go idle in the *same cycle* the
context disappears, with no edge and no lag. This mirrors §3.7 exactly, one layer
down: there, `visit_q` is a scheduled visit and `visit_q & valid` is a presented
one; here, `pattern_q` is a latched pattern and `pattern_q & valid` is a presented
one. Same split, same reason, same section of the engine's specification.

**The destroy-on-falling-`VALID` is what the gate alone does not give you**, and
it is the reason this subsection exists. Consider the sequence a real driver
performs:

```text
   a one-shot pass completes    pattern_q = P, valid = 1, pins = P
   software writes a weight     valid -> 0                    pins = idle   (the gate)
   the Core recomputes offsets  valid -> 1
                                ...
   the next pass starts         the first visit latches the new P'
```

Between `valid` rising again and that first visit, the gate is *open* and
`pattern_q` is still `P` — a pattern from the destroyed matrix, presented as
current. Gating cannot close this hole, because the gate's premise ("VALID means
a matrix exists") is true again while the *latch's* premise ("this pattern came
from that matrix") is not. The latch has to be destroyed with its matrix, and a
falling-`VALID` clear is the cheapest honest way to say so.

Detecting an edge on a level the block already receives is not new machinery
here: `rtl/bcmc_obs_wb.v` does exactly this for the engine's abort
(`eng_aborted_q`, "for detecting the engine's abort edges"), for the same purpose
— turning a level that has gone away into a one-shot act.

### 6.3 The abort is covered, and is not a signal here

`aborted` is not an input (§2.2), and this section is why it does not need to be.
The engine aborts only because `VALID` fell (§3.10), and §6.2 already destroys the
pattern on that event — including the case §2.2 identifies as the one an abort
*wire* would miss, where the context is invalidated while the observer sits
`IDLE` and therefore never aborts at all. One lower-level fact covers both cases;
a higher-level event covers one of them and arrives a cycle late.

### 6.4 Reset

`rst` clears `pattern_q`, and the output engine takes **the same reset the
observer takes** (`ctrl_reset | wb_rst_i`), not merely the bus reset. So
`OBS_CTRL.RESET` — written to stop a continuous pass — also blanks the outputs.

That is deliberate, and it is the one place this document chooses between two
defensible defaults:

| Default | Consequence on `RESET` |
| --- | --- |
| **blank** (chosen) | the schedule stops *and* the actuators are de-energised |
| hold | the schedule stops but the actuators stay at their last position |

Holding is the worse default, because the state it leaves is "no schedule is
running and the hardware is still driving", which is precisely the condition §6.1
exists to prevent. The chosen default fails safe. An application that genuinely
wants its outputs held across a stop — a display, say — is asking for *policy*,
and policy belongs above this block (§5, §9), not in its default.

`OBS_CTRL.RESET` clearing the pattern does not contradict
`docs/Observer_Register_Map.md`, which says RESET clears "the observer and only
the observer: not `ONESHOT`, not the context, not `VALID`". The pattern is not
the context, not `ONESHOT` and not `VALID`; it is part of the driving state that
belongs to the pass being stopped. The register map's list is about what RESET
must *not* reach into, and the output engine's own register is not on that list.

**There is no partially consumed output to preserve** (§3.2). One register is the
whole of the storage, the destroy is atomic, and no block downstream — a bank, a
shift register, a wider output stage (§9) — may hold a pattern of its own that
would survive this clear. That is a constraint on those extensions, and it is
stated here so that adding one cannot quietly break §6.2.

### 6.5 The whole rule, in five lines

```text
on rst:                 pattern_q <= 0
on falling valid_i:     pattern_q <= 0
on visit_valid_i:       pattern_q <= column_bits_i
otherwise:              pattern_q unchanged
pins_o                  = pattern_q & {MAX_C{valid_i}}      (default polarity)
```

Everything in sections 3 to 6 is reachable from those five lines plus the port
list of §2.1. That is what §10's model has to falsify.

---

## 7. No back-pressure

The output engine never stalls the observer. It cannot: **there is no wire with
which to do it.** The seam is one-directional — `column_bits`, `visit_valid`,
`valid`, `done` and `C` inward, `pins_o` outward — so the absence is structural in
the same way §2.2's absences are, not a promise about conduct.

§8.3 of the architecture document gives the reason, and it is worth restating
because it is the load-bearing one: if the output engine could stall the
observer, the observer's fixed trigger→visit latency would become *conditional*,
and the observer's timing contract would depend on an application. That is exactly
the coupling this whole architecture exists to prevent, and it would undo §4.1.

Three consequences, all of them wanted:

1. **The interval in §4.1 is unconditional.** A caller can reason about the
   trigger period and the pins in the same breath. Nothing downstream can lengthen
   it.
2. **If an application cannot keep up, its trigger is too fast.** That is a
   statement about the *trigger*, and the fix belongs there — a slower source, a
   divider, a pin — not in the output stage. It is the same argument
   `docs/Observers.md` makes about balance: the fix for an uncomfortable schedule
   is to change the schedule, not to add a buffer that hides it.
3. **It constrains every extension in §9.** No extension may introduce a path by
   which the output stage's state changes the observer's timing. A "ready" output
   invented by a later revision would be a back-pressure path whatever it is
   called, and it would need this section reopened deliberately rather than
   acquired by accident.

## 8. The traversal-source seam, preserved

The output engine is downstream of the engine; the source seam (§3.3 of the
architecture document) is upstream of it. Nothing in this block touches that seam,
and the reason is structural rather than disciplinary: none of the five inputs in
§2.1 can *cause* a visit. They are all outputs of the engine, and the engine's
trigger comes from the trigger mux on its other side. There is no wire from this
block to `trigger`, to `start`, to the source's request/acknowledge pair, or to
the register window's selector.

So the output engine cannot advance the traversal, cannot choose one, and cannot
see which one ran. It has no `column`, no `N`, no seed, no `a`/`b`, and no
`trigger`. That is the hardware form of `docs/Observers.md`'s sentence — an
application *chooses* a traversal and never *implements* one — and it is enforced
by a port list rather than by a paragraph.

Three shapes are worth naming as forbidden, because each is a plausible
"improvement" that would quietly move a traversal decision into the application:

**A consumer-driven pull.** "If nothing changed, ask for another column." This
inverts the direction of control: the application would decide *when* to advance,
which is the trigger's job (§3.5), and the resulting schedule would be a function
of the application's timing rather than of the source. There is no wire for it,
and there must not be one.

**A stream buffer.** Buffering visits to smooth or reorder them is §3.5's subject;
it is a selection between the traversal and the application that no document
specifies and no diff could check. Note the distinction from §9's extensions: a
buffer that *expands one pattern* (a shift register presenting a wide bank) is
storage of a pattern, not of the stream, and it is permitted subject to §6.2's
clear reaching it.

**A dynamic pin map.** A **static** remapping from rows to pins is a wiring
constant — it does not depend on time or on the visit sequence, so it is not
traversal and is harmless. A remapping that depends on the visit count, the
previous pattern or anything else that varies *is* a traversal by another name,
and it is the thing this document is written to keep out.

## 9. What v2.0b deliberately does not solve

### 9.1 The traversal sources

Whether the stream comes from the identity source, a streaming affine source or a
buffered seeded Fisher–Yates source is **v2.0c's** question, and this block is
indifferent to the answer: it presents whatever arrives. Two consequences are
worth stating, because both are easy to leak backwards into here.

**No rate assumption.** §9.3 of the architecture document measures the
Fisher–Yates fill at roughly `1.4 N` cycles — longer than a pass — so a buffered
source must run a pass or two ahead of the visit stream. None of that appears in
this document. The output engine does not assume that visits arrive at any
particular rate, or that a source can always supply one. It reacts to
`visit_valid` however often that comes, and if a buffered source stalls between
banks the pins simply hold (§3.3), which is correct rather than degraded. Making
the fill bound into an output-engine constraint would be the architecture
deciding an implementation detail of a component that does not exist yet.

**No selection.** There is no source selector, no seed register, no bank handshake
and no "which source is active" state in this block. §9.4's register additions
belong to the observer's window and to the source. If this document ever grows a
wire for them, that is the boundary failing.

### 9.2 The extensions, and the rule each must obey

§8.2 of the architecture document listed four optional features. They are not part
of v2.0b's conformance surface, and each is admitted only under the same three
rules — §6.2's clear must reach it, it must not create a back-pressure path (§7),
and it must not encode traversal (§8):

| Extension | Rule |
| --- | --- |
| **Per-pin polarity** | applies *after* the presentation gate, so an active-low pin idles high: the gate acts on the logical row-active signal, not on the physical level |
| **Per-pin enable mask** | a static constant. A *dynamic* mask depending on the visit sequence would be traversal (§8) |
| **Per-row duty cycle** | divides the **trigger** period, which makes it a trigger parameter or application policy — not an `N`-derived quantity, and not state this block may keep without §6.2 reaching it |
| **Wider than `MAX_C` rows** | instantiate a *bank* of these blocks around one observer, all fed the same visit stream, each presenting its own `MAX_C`-row slice. They stay in lockstep because they share one engine: no arbitration, no second engine, and no extra traversal |
| **A pin-change strobe** | derivable from `pattern_q` and its predecessor, so it is deferred rather than needed. If ever added it is an output, never an input |

Two more things are explicitly out of scope, and are listed so that "not yet" is
not mistaken for "forgotten": **software visibility** (there is no bus port here,
and §2.2 says why), and **several independent observers with different traversals
driving one application** (a system-level arbitration question for v2.0d, which
cannot be answered by widening this block).

### 9.3 What is *not* deferred: the assertions this document does make

Three of v2.0b's claims are checkable and are therefore obligations rather than
intentions, and none of them waits for v2.0c:

- **The pin sequence is the visit sequence**, one clock later and gated by the
  context — checked as an equality on values (§10.2 item 1) rather than by
  counting transitions, which is false (§10.2 item 8).
- **The pin bits are the matrix's column** (§3.4) — for a concrete context, the
  pattern presented at step `t` reproduces `R(pi(t))` exactly, against
  `validation/reference.py`.
- **A pattern cannot outlive its matrix** (§6.2) — including the compose-invalidate-revalidate case that a level-only gate gets wrong.

---

## 10. Verification plan

Same discipline as every other layer, and the same order: **model first, RTL only
once the model is green, and the model written in order to falsify this
document.** The artefacts this document obliges are:

| Artefact | Role |
| --- | --- |
| `validation/output_engine.py` | the cycle-level model of §1–§6, composing the existing engine |
| `validation/gen_out_engine_vectors.py` | the corpus, with a structural coverage guard (§10.4) |
| `validation/test_output_engine.py` | the exhaustive and adversarial suite (§10.2, §10.3) |
| `rtl/bcmc_out_engine.v` | the implementation, written to satisfy the model |
| `sim/bcmc_out_engine_test.cpp` | Verilator: RTL against the model, every cycle |
| `sim/tb_out_engine.v` | Icarus: the independent second opinion |

None of them exists yet. They are named here because a specification that does not
say what will hold it to account is a description.

### 10.1 The model composes the engine, and must not reproduce it

`output_engine.py` consumes the **declared outputs** of the observer, which means
it is driven by `validation/observer_hw.py` — and by
`validation/observer_periph.py` when the pass is being driven through the register
window. It must not reimplement the cursor, the visit strobe or the abort; the
engine is already the authority on all three, exactly as `observer_periph.py`
composes `SequentialEngine` rather than restating its FSM.

That gives the layer the same **composition invariant** that
`validation/observer_periph.py` already proves for the window, one step further
down:

> **Attaching the output engine must not change the engine's behaviour.** For the
> same engine input sequence, the engine's outputs — `column`, `visit_valid`,
> `running`, `done`, `aborted` — are identical whether or not an output engine
> hangs off them.

The invariant is checkable the same way it was last time: record the engine's
input trace and output trace behind the model, feed the input trace to a bare
engine, and require the output trace to be reproduced exactly. Anything the output
stage could do to the engine — a stall, a hidden request, a second traversal —
appears as a disagreement, and it cannot hide behind agreement on the pins.

### 10.2 What the model must be able to falsify

The value of this document is in this list. Each item is a way it could be wrong,
and each is a case the suite is obliged to drive. Two of them have already been
corrected by the model, and the corrections are in place below rather than in a
footnote: the unsound half of item 1, and the transition count of item 8. Both
were claims that *sounded* like the same statement made twice and were not.

1. **One clock, not zero and not two.** The visit presented in cycle `k` is
   present in the pins in cycle `k + 1`. The check is an **equality on the
   value** (`pins(k+1)` is the `column_bits` of cycle `k`), not a claim that the
   pins *changed*; item 8 is why.
   The tempting companion claim — "the pins must not reflect a visit in the
   visit's own cycle" — is **not checkable**, and an earlier draft asserted it.
   When two consecutive visits present the same pattern (a row of weight `N` is
   active in every column), the pins legitimately arrive at the current bits
   because the *previous* visit presented that value, and no observation of the
   pins distinguishes that from a passthrough. A combinational passthrough is
   caught by the equality above whenever two consecutive patterns differ; when
   they never differ it is genuinely unobservable on the pins, which is a fact
   about the interface rather than a gap in the plan. `validation/
   test_output_engine.py` records this beside the predicate, so the omission is
   not mistaken for an oversight later.
2. **Hold.** With no visit and a valid context, the pins are unchanged, however
   many cycles pass.
3. **`done` is not special.** The last visit of a pass latches and presents like
   any other, and the pattern survives the pass boundary.
4. **The gate is combinational.** In the cycle `valid` falls, the pins are already
   idle — no edge, no lag.
5. **The destroy.** Invalidate, revalidate *without a visit*, and the pins must be
   idle for the whole interval. This is §6.2's hole, it is the case a level-only
   gate fails, and it is the single most important test in the suite.
6. **`rst` clears**, and it is the *observer's* reset that clears (so a mid-pass
   `OBS_CTRL.RESET` blanks), not merely the bus reset.
7. **A change is caused only by a visit, by the gate, or by reset.** A pin
   transition may occur on a cycle only if one of: a visit was presented in the
   preceding cycle; `valid` fell; or `rst` was asserted. Nothing else may move the
   pins — no glitch on an unvisited cycle, no drift while simply idle.
8. **Counting: the equality is false, and this document said it was true.** An
   earlier draft of this list required "the number of pin transitions equals the
   number of presented visits", and offered `N` or `N - 1` transitions per pass.
   Both are wrong, and the counterexample is trivial: set every weight to zero —
   legal, since `0 <= w_i <= N` — and every visit presents an empty pattern. A
   three-visit pass then yields **three visits and zero transitions**.
   `validation/output_engine.py`'s doctests carry that case. Nor is it a
   pathological one: with active rows, two consecutive visits can still present
   the same set, so a count can fall short by any amount. What survives is the
   **value equality** of item 1 and the **containment** of item 7, and the bound
   that containment implies: a change needs a cause, so
   `changes <= visits + gate events + resets`. Not `changes <= visits`, which an
   earlier draft wrote and which a reset in mid-pass breaks. This is the same
   shape of correction F11 forced on the engine's conservation invariant — the
   naive equality did not survive contact with a legal input — and it was found
   by writing the model that items 1–11 exist to make possible.
   Separately, and still true: a pass cut short by invalidation presents a
   **prefix** of its visits, not `N`.
9. **Nothing is presented from a dead matrix.** For every cycle: `pins_o` differs
   from idle only if the context was valid when the pattern was latched **and**
   `valid_i` is high now. This is the safety statement §6 exists for, and it is
   what the suite should assert continuously rather than at checkpoints.
10. **A lane above `C` is never active** (§2.3) — driven by the assertion, checked
    across contexts with `C < MAX_C`.
11. **The port list is exactly §2.1.** No sixth input, no internal state exported.
    Structural, and it is the check that keeps §2.2's absences from eroding.

### 10.3 Exhaustive small contexts, and invalidation at every step

Small enough to exhaust, as elsewhere in this project:

- `N = 1`, which is the degenerate case where the matrix is a single column and a
  pass is a single visit. It is where boundary arithmetic goes wrong, and it is
  cheap to cover completely.
- `C = 1` and every weight in `1 .. N`.
- For `N` in `1 .. 6`, **every** weight vector with `0 <= w_i <= N`, in both
  one-shot and continuous mode — the same shape of sweep `cell_exhaustive` uses.
- For every one of those, **invalidate at every step index** `t`, and require §6.2
  to hold from that cycle onward with the pins idle afterwards until a new pass.
  The step index is the axis that the engine's own tests found interesting, and it
  is the axis on which a stale pattern shows up.

### 10.4 The corpus, and the two simulators

`gen_out_engine_vectors.py` records inputs beside the model's declared outputs —
`valid`, `rst`, `column_bits`, `visit_valid`, `done`, `C` in; `pattern` and `pins`
out — one run per header, in the shape `gen_observer_wb_vectors.py` established.

It carries the **structural coverage guard** that generator introduced, because
that guard has already earned its keep: the corpus is *built from* a table of
mandatory cases, and the generator reads the case names back out of the file it
just wrote and refuses to emit a corpus that is missing any of them. For this
layer the mandatory set is: a visit latched; hold across several idle cycles;
`done` coincident with a final visit; a mid-pass reset; **invalidate → revalidate
with no visit** (§10.2 item 5); invalidation at the first and last step; `N = 1`;
a multi-row pattern with a nonzero `column_bits`; and `C < MAX_C` so the §2.3
assertion has something to be true about.

Then two independent paths over that one file:

- `sim/bcmc_out_engine_test.cpp` — Verilator, comparing **every cycle** against
  `validation/output_engine.py`, plus the composition invariant of §10.1;
- `sim/tb_out_engine.v` — Icarus, with a separately written reader, sharing no
  code with the model. The reader parses by tokens rather than indexing a line,
  which is the lesson `sim/tb_observer.v` cost.

Plus the standing requirements: lint clean under both simulators, `format.sh`
clean, and the corpus regenerated by `run_sim.sh` so a stale vector file cannot
survive a change.

### 10.5 The orthogonality check belongs to v2.0c, and is named here so it is not lost

`examples/README.md` checks the product claim by running one application over two
traversals and diffing: the summaries must be **identical** (P1–P4 held) and the
logs must **differ** (the traversal really was different). The hardware analogue
is the same diff, and it becomes checkable the moment v2.0c supplies a second
source:

```text
   one context, one output engine, one trigger schedule
            |
     identity source  -> pins sequence A, summary S
     affine source    -> pins sequence B, summary S
            |
   require A != B  and  multiset(A) == multiset(B)   (P3)
   require both are column-wise rearrangements of M  (P1, P2)
```

It is not v2.0b's obligation — v2.0b has one traversal and therefore nothing to
compare it with — but the output engine must not be built in a way that makes the
comparison impossible, which is why §3.5 refuses to reorder and §8 refuses to
encode traversal.

### 10.6 The mutation battery

The negative control, as everywhere else in this project: planted bugs, one at a
time, each of which the suite must catch. The number that matters is how many are
caught, not how many were written, because that number measures the *harness*.

The ones this layer is specific to:

- the latch replaced by a combinational passthrough (pins follow `column_bits`);
- the presentation gate dropped (a stale pattern is driven);
- **the destroy-on-falling-`VALID` dropped** — the subtle one, and the reason
  §10.2 item 5 exists: with the gate intact it passes every test except the
  revalidate-without-a-visit case;
- polarity applied *before* the gate rather than after;
- `rst` wired to `wb_rst_i` alone, so `OBS_CTRL.RESET` stops the pass but leaves the
  actuators energised (§6.4);
- the pattern cleared on `done`, which turns a held schedule into a pulse;
- `done` delayed by one cycle, which makes the last visit special;
- a lane above `C` presented, which the §2.3 assertion must catch.

Each must fail loudly, and the §6.2 destroy in particular must fail *only* the test
that exists for it — a mutation that everything catches measures nothing.

**Which of these the model can plant, and which it cannot.** The three that are
the stage's own behaviour — the passthrough, the dropped gate, and the dropped
destroy — are planted in `validation/test_output_engine.py` through
`Attached(stage_factory=...)`, and each must be caught by a *named* predicate.
The passthrough is caught there whenever two consecutive patterns differ; §10.2
item 1 records why it cannot be caught when they never differ. The remaining five
— polarity before the gate, `rst` wired to the bus reset alone, cleared on
`done`, `done` delayed, and a lane above `C` presented — are **wiring**
mutations, not behaviours of the block, so they belong to the RTL harnesses
(`sim/bcmc_out_engine_test.cpp` and `sim/tb_out_engine.v`), which can rewire a
port and cannot be planted in a Python stage at all.

**And the battery needs a correct variant.** It carries one: a stage that destroys
on *any* cycle where `valid` is low rather than only on the falling edge. The two
are equivalent — they differ only in cycles where nothing is presented — so the
suite must **accept** it. A test suite that rejects a correct alternative
implementation is wrong about the specification, not strict about it, and that
failure mode is invisible without a control.

---

## 11. Decisions taken, and what remains

### Taken here

1. **The application event is a change in the presented pattern**, not the
   `visit_valid` strobe that causes it (§3.1, §4.2).
2. **Two states, driving and idle**, and no third; nothing can be partially
   consumed, because one register is the whole of the storage (§3.2).
3. **The pattern is held**, within a pass and across a pass boundary, and the
   register is *entailed* by that rather than chosen (§3.3, §4.3).
4. **The presentation gate is combinational** — `pins_o = pattern_q & valid` — the
   same scheduled/presented split the engine makes between `visit_q` and
   `visit_valid`, one layer down (§6.2).
5. **The pattern is destroyed on a falling `VALID`**, because the gate alone leaves
   the invalidate→revalidate-without-a-visit hole open, and that hole presents a
   pattern from a destroyed matrix as current (§6.2).
6. **`valid` is the one addition** to the boundary §8.1 drew; `aborted` and
   `running` are considered and rejected, with the reasons recorded, because an
   abort *event* is both insufficient and later than the level (§2.2, §6.3).
7. **`column` is not an input**, so the block cannot be privileged by a traversal
   and cannot encode one (§2.2, §8).
8. **`C` is an input for the assertion only** — `MAX_C` is the datapath width and
   no masking is done, because the projection already zeroes the lanes above `C`
   and that promise is worth checking rather than assuming (§2.3).
9. **`rst` is the observer's reset**, so `OBS_CTRL.RESET` blanks the outputs; the
   fail-safe default is chosen over hold, and application-defined holding is
   policy above this block (§6.4).
10. **No back-pressure**, structurally; and every extension must preserve the
    unconditional two-clock trigger-to-pin interval (§7).
11. **Output order is observation order** — no buffer, no reordering, no priority
    (§3.5).
12. **One-shot versus continuous is not decided here**; both modes present the same
    thing, and `done` is offered so that an application can apply policy (§5).
13. **The extensions are admitted only under three rules** — §6.2's clear must
    reach them, they must not add back-pressure, and they must not encode
    traversal (§9.2).
14. **v2.0c's fill bound is not an assumption of this layer**: no rate, no source
    selection, no bank handshake (§9.1).

### Remaining, and who decides

| Open question | Decided by | When |
| --- | --- | --- |
| The name and location of the model (`validation/output_engine.py` assumed) | this document | now, cheaply reversible |
| The reference instance's default polarity and idle level | this document, with the model | before RTL |
| Whether `done` is consumed here or passed on to applications | the v2.0d specification | with the applications |
| Whether a pin-change strobe is wanted | the v2.0d specification | with the applications |
| Whether the reference output stage is one instance or a bank (§9.2) | the v2.0d specification | with the applications |
| Whether Fisher–Yates must be index-for-index with `sw/bcmc_observer.c` | the v2.0c specification | with §9.3 |
| Whether the mains heater becomes the worked example of §6.4 | the v2.0d specification | with the applications |

### Status

Sections 1 to 6 are **specification**, and are not frozen until
`validation/output_engine.py` has run: a specification in this project is what a
model has failed to falsify, and that has not been attempted yet. Sections 7 to 9
are **boundary statements** — they constrain what may be built rather than
describing a behaviour, and they are checkable structurally (a port list, a
presence of a wire). Section 10 is the plan that would freeze the rest.

Nothing here modifies v2.0a. If implementing it turned out to need a change to
`rtl/bcmc_observer.v` or `rtl/bcmc_obs_wb.v`, the first presumption is that **this
document is wrong**, not that the engine needs a feature — the same rule the
peripheral layer worked under, and the rule that kept three integration defects
from being "fixed" in the wrong place. The two frozen checkpoints are
`v2.0a` (`221f976`) and `v2.0a-periph` (`587933d`); this document does not move
either.

---

## Sources and status

This document follows `docs/Hardware_Observer.md` (the v2.0a outline) and
`docs/Hardware_Observer_Architecture.md` (its specification), and it is the
specification that §8 of the latter said would be written before v2.0b's RTL.

Where it repeats an existing contract it names it: `docs/Observers.md` (O1–O3,
P1–P4, the four prohibitions, and "an application chooses a traversal strategy and
never implements one"), `docs/Register_Map.md` (`VALID`, E4, "evaluation has no
handshake", no `NEXT_COLUMN`), `docs/Transaction_Sequences.md` (returning the old
matrix after a weight change is incorrect), `docs/Observer_Register_Map.md`
(`ONESHOT` carried on every write, `RESET` clearing the observer and only the
observer), and `examples/README.md` (`Application × Traversal`, checked by diff).

It leans on three artefacts that already exist and are frozen:
`rtl/bcmc_column.v` (the lanes above `C` are zero by construction),
`rtl/bcmc_obs_wb.v` (the visit stream, and the abort-edge detection idiom of
§6.2), and `rtl/bcmc_observer.v` (the scheduled/presented split of §3.7, which §6.2
copies one layer down).

**Status.** Specification, **not frozen**. The model exists and its suite is green
(`validation/output_engine.py`, `validation/test_output_engine.py`), and between
them they have already proved four things in this document wrong or unsupported —
§3.1's "one event per visit", §10.2 items 1, 7 and 8, and §10.2 item 1's
companion claim, which is not checkable at all. The RTL is the step that would
freeze the rest, and it is deliberately not started: on this project's record a
specification becomes a specification when a model has failed to falsify it *and*
an implementation has been held to it. The first is done; the second is not.

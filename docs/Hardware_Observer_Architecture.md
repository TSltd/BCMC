# BCMC Hardware Observer — Architecture and the v2.0a Specification

This document is **specification** for the v2.0a observer, and **architecture** for
the stream it opens.

`docs/Hardware_Observer.md` is the outline of the stream: it records the shape
the project converged on, fixes the boundaries, and lists what had to be decided.
This document makes those decisions, and where it speaks about v2.0a it speaks in
the register of `docs/Hardware_Architecture.md` — ports, states, cycles, and what
is true in each of them.

```text
docs/Observers.md                      ->  validation/observers.py   ->  sw/bcmc_observer.{h,c}
docs/Hardware_Observer.md              ->  (this document, v2.0a)
docs/Hardware_Observer_Architecture.md ->  validation/observer_hw.py  ->  rtl/bcmc_observer.v
```

Where an implementation and this document disagree, this document is right and
the implementation is a bug — the same sentence `docs/BCMC.md`,
`docs/Register_Map.md` and `docs/Observers.md` all open with.

---

## 0. Scope, and what a "v2.0a specification" commits to

A specification is a set of words an implementation is then held to. It is worth
being exact about which words those are here, because the observer is the first
component in this project whose *behaviour* is not a mathematical claim.

Three things are mathematics, and this document repeats them rather than
defining them:

- **`M`** — the characteristic function, `docs/BCMC.md`, implemented by
  `rtl/bcmc_cell.v` and consumed as `column_bits`.
- **O1, O2, O3** — the observer contract, `docs/Observers.md`.
- **P1–P4** — the consequences of O1 and O2.

Three things are this document's to define, and are new:

- the **engine**: its ports, its states, and its cycle-level protocol;
- the **seam** between the engine and the construction it consumes — the
  observation sideband and the traversal-source interface;
- the **timing contract**: when a visited column is valid, what happens at a pass
  boundary, and what happens when the context is invalidated.

Everything else — what a visit is *for*, whether the output is a pin or a DMA
descriptor, how often the trigger fires — is application semantics. `docs/Observers.md`
already excludes it, and so does this file.

**The v2.0a deliverable is deliberately the smallest thing that can be a
conforming observer in hardware:** a sequential engine (`pi(t) = t`), one
combinational Evaluator reached over a sideband, and no bus traffic per visit. A
traversal that could "only" be built in hardware would be the wrong first
traversal; the identity source is the control case again, exactly as it is in
`sw/bcmc_observer.c`, and it is what every later source is diffed against.

**Status: v2.0a is frozen.** Sections 3 to 6 are behaviourally complete — every
combination of inputs on an edge has a stated result (3.11), the timing is one
clock (5.1), and the seams are pinned (3.3, 4). What remains (section 11) is
*configuration*, not behaviour. The next artefact was `validation/observer_hw.py`,
the cycle-level model derived from this document. It now exists and is green, and
it found two defects *in the document itself* — F11 and F12 in section 10 — which
are corrected here. `rtl/bcmc_observer.v` is written only once that model is
green, and to satisfy it.

---

## 1. Architecture

### 1.1 The three questions, and where each is now answered

The project has, from the beginning, been three questions wearing one name. Each
is answered by exactly one component, and the observer is the third:

```text
where do the intervals begin?      the Core       rtl/bcmc_core.v
what is the state at a coordinate? the Evaluator  rtl/bcmc_cell.v  (bcmc_column.v)
which coordinate next?             the Observer   rtl/bcmc_observer.v   <-- new
```

The first two are mathematical and were closed in v0.2 and v0.3. The third is
not: it is a *choice*, and `docs/Observers.md` is explicit that BCMC does not
define it. What v2.0 changes is only *where the choice is made* — hardware
instead of software. It does not change what the choice is, and it cannot change
what the first two answers are.

### 1.2 Block diagram

```text
                      +-----------------------------------------------------+
                      |  bcmc_wb   (the existing Wishbone peripheral)        |
                      |                                                     |
   Wishbone B4  <---->|  decode | error model | registers | START sequencer  |
   (unchanged)        |                                                     |
                      |   +--------------+   +-----------+                  |
                      |   | bcmc_context |<->| bcmc_core |   +-----------+  |
                      |   |  storage     |   |  prefix   |   | bcmc_     |  |
                      |   +--------------+   +-----------+   | column    |  |
                      |      |     |                       | (software)|  |
                      |      |     |                       +-----------+  |
                      +------|-----|----------------------------------------+
                             |     |  observation sideband (new, outputs only)
                             |     |  N, C, VALID, weights_flat, offsets_flat
                             v     v
                      +-----------------------------------------------------+
                      |  bcmc_observer   (new block)                         |
                      |                                                     |
     start  --------->|   +-----------------+     +-------------------+      |
     trigger -------->|   |  sequential     | col |  bcmc_column      |      |
                      |   |  engine  (FSM)  |---->|  (dedicated)      |      |
                      |   |  pi(t) = t      |     |  MAX_C x cell     |      |
                      |   +-----------------+     +-------------------+      |
                      |      |        |                   |                 |
                      |   visit_valid  done          column_bits            |
                      +------|--------|-------------------|------------------+
                             |        |                   |
                             v        v                   v
                      +-----------------------------------------------------+
                      |  bcmc_out_engine   (v2.0b)  -- column_bits -> pins   |
                      +-----------------------------------------------------+
```

Two things about this picture are the whole point of the section, and both are
new relative to anything the project has built:

1. **The observer reaches the representation over a sideband, not over the bus.**
   The wire from `bcmc_context` to the observer carries `(weights_flat,
   offsets_flat)` — the same flat vectors that already run from the Context to
   the Evaluator inside the wrapper. The observer answers its own evaluation
   queries from those wires. A visit therefore costs **zero** Wishbone
   transactions, which is the one claim that makes a hardware observer worth
   building at all.
2. **The column is a wire, not a register.** The observer drives `column` into a
   `bcmc_column` instance directly. There is no `CELL_COL` write, no `COLUMN[k]`
   read, and no polling. This is possible only because of a fact already settled
   by the transaction specification: `COLUMN` is a *combinational evaluation
   query*, not a stored window of matrix bits, and `CELL_COL` is the column
   selector. There is no framebuffer and therefore no coherence to maintain —
   which is exactly the property the bus path cannot offer and the sideband gets
   for free.

### 1.3 Why the software bus path is not enough

The software observer works, is verified, and is not being replaced. But its cost
is structural and cannot be optimised away from above:

| Cost, per visit | Software observer | v2.0a observer |
| --- | --- | --- |
| Wishbone transactions | `1 + ceil(MAX_C/32)` — two at `MAX_C = 64` | **0** |
| CPU interrupts | one per tick | none |
| Latency, trigger → pins | interrupt, decode, two reads, GPIO write | trigger → 1 cycle → pins |
| Jitter | depends on the CPU's other work | one clock |
| Who owns the time axis | the CPU | the observer |

The `1 + ceil(MAX_C/32)` figure is the one v0.4d and v0.5b both pinned; it is not
a criticism of the driver, it is what a bus costs. The observer removes the bus
from the loop rather than making it faster, which is the same move the Core made
when it refused to hold its state in RAM.

---

## 2. What does not change

The first obligation of a specification is to say what it is *not* allowed to
touch. For v2.0a, the list is short and absolute.

**The mathematics.** `M`, the Balance Theorem, Row Conservation — untouched. A
hardware observer that could change a proven property would not be an observer;
it would be a second construction wearing the first one's name
(`docs/Observers.md`).

**O1, O2, O3.** The engine is a conforming observer and is bound by the same
three clauses as `sw/bcmc_observer.c`. In particular O3 — determinism — is not
weakened by the trigger being asynchronous: the trigger decides *when* a visit
happens, never *which* column. `pi` remains a function of `(N, seed)` alone.

**P1, P2, P3, P4.** Consequences of O1 and O2, and therefore inherited rather
than re-proved. They are still *checked* — section 7 makes each one a hardware
obligation — but the proof does not move.

**The Core, the Context, and the wrapper's behaviour.** `rtl/bcmc_core.v` and
`rtl/bcmc_context.v` are unchanged, port for port.

**`docs/Register_Map.md`.** No new register, no new address, no new
error condition. In particular there is still no "next column" register.

**`docs/Transaction_Sequences.md`.** S1–S8 continue to hold, and the observer
adds no sequence the peripheral must answer.

The one thing that *does* change is that `rtl/bcmc_wb.v` grows a group of
**output-only** ports (section 6). That is an interface addition, not a
behaviour change, and section 6.4 shows it cannot alter a transaction.

### 2.1 The doctrine the observer inherits

`docs/Observers.md` ends with four prohibitions. They were written for software
and they are not softened for hardware; each is restated here with what it means
for the engine, because in each case the enforcement mechanism changes:

| Prohibition | Software mechanism | Hardware mechanism |
| --- | --- | --- |
| An observer must not hold the matrix | it calls `bcmc_read_column()` | it has no matrix storage; `column_bits` is a wire from a combinational cone |
| must not reach past its interface | it uses only driver primitives | it has no bus port at all |
| must not allocate | buffers are caller-supplied | registers are fixed at synthesis; nothing is allocated |
| must not be privileged | no `next column` register | no `next column` register, and the observer's own registers live in a separate window (6.3) |

The third row is the one that changes most. A software observer refuses to
allocate because an allocator is an *application* of BCMC. A hardware observer
cannot allocate — its memory is a synthesis decision — which is the hardware
statement of the same rule: the size of the permutation bank, when v2.0c brings
one, is fixed by the person building the device, not chosen at run time by the
observer.

---

## 3. The Sequential Observer Engine (v2.0a)

### 3.1 Ports

```verilog
module bcmc_observer #(
    parameter VAL_W = 16,     // N, a column index, and a stored value
    parameter IDX_W = 16,     // C and a row index
    parameter MAX_C = 16      // rows this instance can evaluate
) (
    input  wire                   clk,
    input  wire                   rst,          // synchronous, active high

    //--- control -- the observer's own register window (section 6.3) --------
    input  wire                   start,        // one-cycle pulse: begin a pass
    input  wire                   trigger,      // one-cycle pulse: advance
    input  wire                   oneshot,      // 1: stop at done; 0: wrap

    //--- observation sideband -- from bcmc_wb, outputs only (section 4) -----
    input  wire [VAL_W-1:0]       N,
    input  wire [IDX_W-1:0]       C,
    input  wire                   valid,        // STATUS.VALID
    input  wire [MAX_C*VAL_W-1:0] weights_flat,
    input  wire [MAX_C*VAL_W-1:0] offsets_flat,

    //--- to the output engine (v2.0b) ---------------------------------------
    output wire [VAL_W-1:0]       column,       // pi(t), presented for one cycle
    output wire                   visit_valid,  // one cycle; never while !valid
    output wire [MAX_C-1:0]       column_bits,  // M(:, column), combinational
    output wire                   done,         // one cycle, with the last visit
    output wire                   running,      // a pass is in progress
    output wire                   aborted       // sticky: restart required
);
```

Two naming choices are deliberate. `trigger`, not `clock`, because the observer
does not own a clock domain and must not be confused with one: the trigger paces
visits, and the whole point is that it may be far slower than `clk`. And
`column`, not `column_sel`, because it is the same quantity `bcmc_column.v`
already calls `column` and drives on the same wire — the observer is a *source*
for that port, not a selector for a window.

### 3.2 State machine

The engine has two states, one cursor and one output register. That is the entire
sequential content of v2.0a.

```text
              rst
               │
               ▼
          +---------+ start & valid & N>=1    +------------------------+
          |  IDLE   |────────────────────────>|          RUN           |
          | no pass |  t <- 0                 |  t counts 0 .. N-1     |
          +---------+  present pi(0)          |  a pass is in progress |
              ▲                               +------------------------+
              │                                    │          │
              │  done & oneshot                    │ trigger  │ wrap
              │  !valid (any state)                ▼          │
              └───────────────────────  present pi(t+1)  ─────┘
```

| State | `running` | `visit_valid` | Meaning |
| --- | --- | --- | --- |
| `IDLE` | 0 | 0 | no pass; armed only by `start` |
| `RUN`  | 1 | 1 for one cycle per accepted trigger (and once for `start`) | a pass is in progress |

Registers, and nothing else:

| Register | Width | Holds |
| --- | --- | --- |
| `state_q` | 1 | `IDLE` / `RUN` |
| `t_q` | `VAL_W` | the cursor, `0 .. N-1` |
| `col_q` | `VAL_W` | `pi(t_q)`, the presented column |
| `visit_q`, `done_q` | 2 | the one-cycle strobes |
| `aborted_q` | 1 | a pass was cut short by `!valid` |

There is no matrix storage, no permutation storage in v2.0a, and no bus. The
engine holds a *cursor*, and `docs/Observers.md` forbids it to hold anything
else.

### 3.3 The traversal-source seam

Even though v2.0a's only source is the identity, the engine is built around the
seam v2.0c will fill, so that filling it is not a rewrite:

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

**The seam is a request/acknowledge pair, and this does not contradict
"Evaluation has no handshake".** That rule is about `M`: the characteristic
function is a function, so *evaluation* has no latency and no handshake. A
*traversal source* is not a function of nothing — a shuffled source reads a
permutation out of a block RAM, and a RAM read is a cycle. The handshake is
therefore on the wrong side of the boundary to matter: it delays *which column is
asked about*, never *how long the answer takes*. The Evaluator remains, in the
words of the register map, something with nothing to wait for.

In v2.0a the identity source is one assignment — `ts_pi = ts_t`, `ts_ack = 1` —
so the seam costs one wire and no cycle, and the state machine of 3.2 is the
whole engine. Its presence here is the architectural decision that v2.0c is a
*source swap*, not an engine change.

### 3.4 `start`

`start` is a one-cycle pulse, and its acceptance condition is a single
conjunction:

```text
start accepted  <=>  state_q == IDLE  &  valid  &  (N >= 1)
```

Otherwise it is ignored (see 3.9 and the event table in 3.11). On the clock edge
after an accepted `start`:

```text
state_q <- RUN        t_q <- 0        col_q <- pi(0) = 0
visit_q <- 1          done_q <- (N == 1)        aborted_q <- 0
```

so the first visit is presented in the **cycle after** `start`, and `running`
rises with it. `start` also clears the sticky `aborted` flag: a restart is the
acknowledgement of an abort.

**`N = 0` does not start a pass.** `N >= 1` is a precondition of the theorem, not
a legal empty instance: `docs/Transaction_Sequences.md` draws exactly that line
when it says "`C = 0` is the empty instance; `N = 0` is not an instance at all."
So `N = 0` is outside the observer's **input domain**, and the engine refuses the
`start` rather than entering `RUN` with nothing to visit. There is consequently no
"running but empty" state to escape from: the engine can only ever be `IDLE`, or
in a `RUN` with a well-defined `0 <= t_q < N`. A `start` with `N = 0` leaves the
engine in `IDLE` and changes nothing.

This is the one place where the observer deliberately differs from the wrapper's
handling of `C = 0`. `C = 0` *is* an instance — an empty matrix, legal and
visitable, whose every column is empty — so a pass over it is well defined and
runs `N` visits that emit nothing. `N = 0` is not an instance at all, so there is
no pass to define; admitting it would create a `RUN` that emits no visits
forever, which is a dead state reachable from `IDLE` and escapable only by reset.
Refusing it at the door is simpler than reasoning about that state.

### 3.5 `trigger`, and the trigger sources

`trigger` is a one-cycle pulse and is the *only* thing that paces a pass. It is
accepted only in `RUN` with `valid = 1`. On the edge after an accepted `trigger`:

```text
t_q    <- (t_q + 1 < N) ? t_q + 1 : 0
col_q  <- pi(t_q_new)       visit_q <- 1
done_q <- (t_q_new == N - 1)
```

Exactly one accepted `trigger` produces exactly one visit, at every point in the
pass and across every pass boundary — there is no cycle in which a trigger
produces two visits or none (other than the refusals of 3.9 and 3.11).

**`trigger` is an input to the engine, not a register in it.** The engine does not
know, and must not be able to tell, where the pulse came from. What *originates*
the pulse is a separate concern, and v2.0a makes it an explicit one even though it
implements only a single source of it:

```text
                        +----------------------------+
   software STEP ------>|                            |
   timer         ------>|     trigger source mux      |-----> trigger ---> engine
   zero-cross    ------>|                            |
   external pin  ------>|                            |
                        +----------------------------+
```

v2.0a instantiates the **software** source and nothing else: `OBS_CTRL.STEP` is a
W1S bit whose pulse is fed to the mux, and the mux's output is the engine's
`trigger`. So `STEP == trigger` is a property of the v2.0a *configuration*, not of
the architecture. Adding a timer, a zero-cross detector or an external pin changes
the mux, not the engine, and not the meaning of the wire — which is what keeps a
pass's column sequence independent of which source paced it (O3, section 7.4).

Which sources v2.0a advertises is one of the open questions (section 11). The mux
is specified here, ahead of that answer, so that answering it is a configuration
choice rather than a rewrite. The name `step` in the outline and `trigger` here
are the same signal seen from two sides: `step` is what a *caller* asks for,
`trigger` is what the *engine* receives.

### 3.6 `column`

`column` is `col_q`, a registered `VAL_W`-bit output. It is **guaranteed valid
for every cycle in which `visit_valid` is asserted**, and in those cycles it
satisfies `0 <= column < N` by construction, because `t_q` does and `pi` is a
bijection. Outside a visit, `column` **retains its last value**: it is not cleared
between visits and not cleared at a pass boundary (5.3). That retained value
carries no traversal-event significance — the only cycles in which `column` means
"this is the column being visited *now*" are the cycles in which `visit_valid` is
high. After a reset, `column` is `0` in `IDLE`.

It is worth stating it this way round because the shorter phrasing — "`column` is
meaningful exactly when `visit_valid` is" — appears to contradict 5.3, which
deliberately *keeps* the last column after `done` so that a consumer such as a
GPIO bank has no end-of-pass case to handle. The precise form is: **valid during
a visit; retained and insignificant otherwise.**

`column` needs no valid bit of its own, which is the same rule the Core's
`offset_out` follows: a value is meaningful when its accompanying strobe says so.

### 3.7 `visit_valid`

`visit_valid` is the enable for the whole visit, and it is defined by a
conjunction rather than by the state machine alone:

```verilog
assign visit_valid = visit_q & valid;
```

This is the single most important line in the v2.0a engine, and it is deliberately
*not* an `if` inside the FSM. Gating on `valid` at the output means the guarantee
holds **combinationally, in every cycle**, including the cycle in which `valid`
falls: if the context is invalidated on the same edge a visit was due, the visit
does not happen. It is the hardware form of E4 — the peripheral refuses to answer
a `COLUMN` read when `!VALID`, and the observer refuses to *present* a visit for
the same reason and by the same rule. It is checkable by a one-line assertion
(`assert (!(visit_valid && !valid))`) rather than by reasoning about the FSM.

**A visit is a *scheduled* visit.** `visit_q` means "a visit is due this cycle";
the output `visit_valid` means "and it is being presented". The two differ
exactly when `valid` is low, and keeping them as separate names is what makes the
next point sail rather than snag.

**Two mechanisms, kept orthogonal.** Which half of the invalidation story is
combinational and which half is synchronous is easy to conflate, and an RTL
implementation that conflates them gets one of the two wrong:

| Mechanism | Kind | Does | Where |
| --- | --- | --- | --- |
| **suppression** | combinational | `visit_valid` cannot be high while `!valid` | this section; the output gate |
| **abort** | synchronous | a scheduled visit is discarded and the pass ends, on the next rising edge | 3.10; the FSM |

The combinational half is the *safety* property: no stale bit is ever presented.
The synchronous half is the *state* property: the engine leaves `RUN` and records
why. Neither substitutes for the other. Gating alone would leave the engine in
`RUN` forever and, under `oneshot = 0`, would silently resume the aborted pass the
moment `valid` returned — a pass spanning two matrices, the failure F3 names. The
FSM alone would emit one more visit in the cycle `valid` falls, because an FSM
tests `valid` at an edge and the edge has already happened. Both are required, and
both are separately testable.

### 3.8 `done` and pass boundaries

`done` is a one-cycle pulse asserted in the same cycle as the visit of the **last
column of a pass** — the visit for which `t_q = N - 1`:

```verilog
assign done = visit_q & valid & (t_q == N - 1);
```

It marks the visit, it does not follow it. A consumer that latches `column_bits`
on `visit_valid` and separately needs to know "that was the end" sees both
asserted together, which is what makes a one-shot application a two-line program.

At the boundary, exactly one of two things happens, chosen by `oneshot`:

| `oneshot` | On the edge after the `done` visit |
| --- | --- |
| `1` | `state_q <- IDLE`, `visit_q <- 0`, `running <- 0`. A new pass needs a new `start`. |
| `0` | the engine stays in `RUN`; the **next `trigger`** sets `t_q <- 0`, `col_q <- pi(0)`, `visit_q <- 1`, `done_q <- (N == 1)`. The pass wraps. |

Note what `oneshot = 0` does *not* do: it does not present `pi(0)` of the new
pass on the edge after `done`. That would be a second visit from the trigger that
produced the last one, and it would break the one-trigger-one-visit rule of 3.5
at exactly the boundary where it is hardest to notice. A new pass begins on a
*trigger*, like every other step.

`N = 1` is the degenerate case and it is worth stating because it catches
off-by-one bugs: every trigger visits column `0`, asserts `done`, and — under
`oneshot = 0` — wraps to `t = 0` again. So `done` and `visit_valid` are asserted
together on every trigger, and a pass is a single visit. This is the case a
`(t + 1 < N)` comparison gets wrong if it is written `(t + 1 != N)` or `(t < N)`.

A `pass_counter` (an optional `IDX_W`-wide counter incremented on `done`) is part
of the observer's own status window (6.3), not of the engine's contract. It is
reported, never used to decide anything.

### 3.9 `reset`

`rst` is synchronous and active high, matching `bcmc_core.v`, `bcmc_context.v`
and `bcmc_wb.v`. While `rst` is high, on every edge:

```text
state_q <- IDLE   t_q <- 0   col_q <- 0
visit_q <- 0      done_q <- 0   aborted_q <- 0
```

`running`, `visit_valid` and `done` are all `0`, and `column_bits` is whatever
`M(:, 0)` is — defined, not undefined, since the Evaluator is a function and
`column = 0` satisfies its preconditions for every `N >= 1`. Reset does not
require a valid context and does not assert `aborted`: reset is not an abort, it
is the absence of a pass.

### 3.10 What happens when the context becomes invalid

This is the discipline the transaction specification already fixed, restated for
the engine. `docs/Register_Map.md`: writing any `WEIGHT[i]` clears `VALID`,
because the offsets in the context now describe weights that are no longer
programmed. `docs/Transaction_Sequences.md`, S7: a `COLUMN` read during that
window is **refused**, because "a peripheral that answered those reads with the
old matrix would be answering a question about a matrix that no longer exists."

The observer has no bus, so it has no `err` to return. It has the analogous
obligation, and it discharges it the same way, structurally:

1. **No visit is ever presented while `!valid`** (combinational, 3.7):

   ```verilog
   assign visit_valid = visit_q & valid;
   assign done        = visit_q & valid & (t_q == N - 1);
   ```

   Not one cycle of stale matrix bits reaches the output engine, and the guarantee
   holds in the very cycle `valid` falls, not one cycle later.
2. **A pass in flight is aborted** (synchronous, on the next rising edge), because
   a pass is a traversal of *one* matrix. If the weights change halfway through,
   the remaining visits would be visits to a different `M`, and the pass would no
   longer be a pass: O1 would hold syntactically (still `N` visits) and fail
   semantically (not the same matrix). So on the rising edge at which `RUN` and
   `!valid` are both true:

   ```text
   state_q <- IDLE    visit_q <- 0    done_q <- 0    running <- 0
   aborted_q <- 1     (unless the pass had already completed)
   ```

   The two rules are orthogonal by construction (3.7): the `if` decides the state,
   the `assign` decides what the wires show. A scheduled visit that is suppressed
   by (1) is the same event that (2) ends the pass on the following edge.

3. **The abort is reported, not silent.** `aborted` is sticky and cleared only by
   a new `start`. It is the observer's `err`: the software-side analogue is the
   refused access in S7, and the register-map analogue of a sticky,
   acknowledge-by-action flag is `IRQ` (latched, cleared by writing it). Software
   that polls `OBS_STATUS` can therefore tell "the pass ended" from "the pass
   died".

`trigger` and `start` while `!valid` are ignored, so an auto-triggered application
(timer, zero-cross) simply stops advancing through the invalidation window and
does not accumulate a backlog: **drop, not defer**. This mirrors the bus, where a
refused access leaves no trace and does not queue.

A pass may therefore have one of three endings — `done` (completed), `aborted`
(cut short), or neither (still in flight) — and `OBS_STATUS` distinguishes all
three. Nothing else resumes a pass; only `start` does, after `VALID` has returned.

### 3.11 Simultaneous events

The engine is small enough that every combination of its inputs on one edge can
be enumerated, so it is enumerated. The table below is the complete decision
rule; where an implementation and this table disagree, the table is right.

| Condition on the edge | Result |
| --- | --- |
| `start` & `IDLE` & `valid` & `N >= 1` | **accepted**: enter `RUN`, `t_q <- 0`, visit `pi(0)` |
| `start` & (`!IDLE` or `!valid` or `N == 0`) | ignored; no state change |
| `trigger` & `RUN` & `valid` | **accepted**: advance one step, schedule one visit |
| `trigger` & `IDLE` | ignored |
| `trigger` & `RUN` & `!valid` | ignored; the abort of 3.10 takes precedence |
| `!valid` & `RUN` | abort: `IDLE`, `visit_q <- 0`, `aborted_q <- 1` |
| `!valid` & `visit_q == 1` (any state) | `visit_valid = 0` **this cycle** (combinational, 3.7); the scheduled visit is not seen |
| `done` & `oneshot` | after that visit: `IDLE`, `running <- 0` |
| `done` & `!oneshot` | remain `RUN`; the next accepted `trigger` wraps to `t_q = 0` |
| `done` & `oneshot` & `trigger` | completion wins: the pass ends in `IDLE`; the `trigger` is ignored |
| `start` & `trigger` in the same cycle | **`start` wins** if `IDLE`; if `RUN`, `start` is refused and `trigger` is honoured |
| `rst` | overrides every row above; see 3.9 |

Three rows are worth a word beyond the table.

**`start` and `trigger` cannot genuinely conflict.** In `IDLE` a `trigger` is
ignored; in `RUN` a `start` is refused. So at most one of the two is ever
*accepted* on a given edge, and the row exists only to pin the behaviour for an
RTL author who gates them differently. `start` is given priority in `IDLE` as the
defensive choice, so that a test can assert it.

**`!valid` beats `trigger`.** A trigger in the same cycle the context dies is
ignored, not latched (3.10, "drop, not defer"). It is worth being precise about
what this does and does not buy, because the first draft of this section claimed
too much. A trigger accepted *one cycle earlier* has already scheduled a visit,
and if the context is invalidated before that visit is presented, the cursor has
moved but the visit is suppressed by the output gate of 3.7. So the visit count is
**not** preserved across an invalidation. What is true -- and what section 7.4 now
states -- is that every accepted request schedules exactly one visit, every
scheduled visit is either presented or suppressed, and **the suppressed count is
at most one per invalidation**, because a request is never more than one cycle
ahead of its visit. The cycle model found this; see section 10, F11.

**`N >= 1` is part of the `start` condition, not a case inside `RUN`.** There is
no row for `start` with `N == 0` entering a pass, because there is no such pass
(3.4). The refusal is total, and it leaves no state behind.

---

## 4. The BCMC ↔ Observer interface

### 4.1 How `N`, `C` and `VALID` are exposed

`N`, `C` and `VALID` are registers `bcmc_wb.v` already owns — `0x014`, `0x018`
and `STATUS[1]`. The observer is given them as **inputs** over the sideband
(section 6). Where they live is unchanged; the observer is a *reader* of the
same values software reads, and there is no second copy:

| Signal | Source in `bcmc_wb` | Why the observer needs it |
| --- | --- | --- |
| `N` | `n_q` (`0x014`) | the cursor wraps at `N`; `pi` is a bijection of `0 .. N-1` |
| `C` | `c_q` (`0x018`) | forwarded to the output engine; not used by the engine |
| `valid` | `STATUS[1]` | gates every visit (3.7) and aborts a pass (3.10) |
| `weights_flat`, `offsets_flat` | the Context's flat outputs | the Evaluator's two arguments |

Note what is deliberately **not** exposed: the Context. The observer does not see
`weight_mem` or `offset_mem`, and it does not get a client port on
`bcmc_context.v`. It sees the *flat vectors*, which are the representation as the
Evaluator already sees it. This keeps the Context's three-client structure
(software, the Core, the Evaluator) intact: the observer is a further reader of
the same flat output the Evaluator reads, not a fourth client of the storage.

### 4.2 How the observer "requests" a column: it does not

There is no request. The observer drives `column` and asserts `visit_valid`; the
Evaluator answers in the same cycle, because it is a function. The transaction
specification already said why there is no protocol to have:

> Answered in the same bus cycle as the read. The Evaluator is combinational, so
> there is nothing to wait for and no "start query, poll ready" step — that would
> be inventing latency the mathematics does not have.
> — `docs/Transaction_Sequences.md`, S4

The hardware path is the same statement without the bus. In software, "request a
column" meant `W CELL_COL; R COLUMN[k]` — a write, a read, and an `ack` — and the
observer's whole contribution is to delete the two transactions and keep the
`column`, which was always the only real argument. So the interface is one wire
in the representation and one wire out:

```text
        bcmc_observer                       (its own bcmc_column instance)
   column ──────────────────────────────> .column
                                          bcmc_column  ────────> column_bits
   N, weights_flat, offsets_flat ───────> .N .weights_flat .offsets_flat
```

`column_bits` and `column` are in the same cycle. There is no `column_ready` and
there will not be one, for the same reason `CELL` has no ready bit.

### 4.3 How `column_bits` reach the output stage

`column_bits` is the output of the observer's own `bcmc_column` instance, one
`MAX_C`-bit vector, `column_bits[i] = M(i, column)`. It goes to the output engine
(v2.0b) as a plain wire. The output engine latches it in the cycle `visit_valid`
is high; it never reads a register, never addresses anything, and never waits.

Two properties are inherited from `bcmc_column.v` and are worth stating because
the output stage relies on both:

- `column_bits[i] = 0` for every `i >= C`, unconditionally, because the lane's
  arguments are forced to zero (`active = ROW < C`) — so the output engine needs
  no special case for rows that do not exist. It is the same "inactive lane"
  rule the register map uses for `CELL` and `COLUMN`.
- `column_bits[i] = 0` for every `i >= MAX_C`, by the width of the vector.

### 4.4 Does the Evaluator remain entirely combinational? Yes — and it is its own instance

**The `bcmc_column` instance inside `bcmc_observer` is purely combinational.** It
has no clock, no reset and no state, exactly as `rtl/bcmc_column.v` is written
today. The observer adds a *source* for its `column` port and a *sink* for its
`column_bits` port; it adds nothing inside the cone.

The one genuine design question here is whether the observer gets its **own**
instance or shares the wrapper's. This document decides: **its own.**

The wrapper already states the rule it is being asked about:

> The cell is not computed a second time by a second `bcmc_cell` instance, and
> that is deliberate: two implementations of one number are two things that can
> disagree.

That rule is about two *implementations* — two bodies of logic for one number —
and it is honoured here: the observer instantiates the *same module*,
`bcmc_column`, from the same source, with the same parameters. There is one
implementation of `M` in the tree; it is instantiated twice. This is not a
loophole, and it is checked the way the project always checks replication:
`sim/bcmc_column_test.cpp` already compares every bit of a `bcmc_column` against
a separately instantiated `bcmc_cell`, so a second instance of a verified module
is not a new claim.

The alternative — sharing the wrapper's single instance by multiplexing its
`column` port between `cell_col_q` (software) and the observer — is rejected for
a reason that is architectural rather than aesthetic. A shared port makes the two
clients contend for it. A software `COLUMN` read and an observer visit could land
on the same cycle, and the resolution would have to be either a stall in the
observer (which makes the trigger→visit latency data-dependent and can drop a
trigger) or a priority rule that momentarily changes the evaluated column (which
puts a correctness argument about *sampling windows* into the one place the
project has worked hardest to keep free of them). A dedicated instance removes
the contention entirely: the two clients address two combinational cones built
from the same module, and neither can perturb the other. At `MAX_C = 64` the cost
is 64 cells, each a comparator and a conditional add — a price worth paying to
delete an arbitration argument.

Consequently the wrapper's own `column_bits` and its `CELL`/`COLUMN` reads are
untouched. Software sees exactly the transactions it saw before (section 6.4).

### 4.5 What the observer does *not* need from the Context

Worth making explicit, because each absence is a boundary the project already
drew and the observer is obliged to respect:

- **No `N` port on the Context, and none added.** The Context still has no idea
  what `N` is (v0.4b). The observer gets `N` from the wrapper, which has always
  owned it. So the observer does not force mathematics into the Context — the
  thing v0.4b spent a paragraph proving it could not express.
- **No write path.** The observer can never write a weight or an offset. It has
  no write port to the Context, so `offset[0] = 0` and "software cannot seed a
  transform" remain structural facts, not rules the observer must remember.
- **No `C` dependency in the traversal.** The engine does not use `C`; it
  forwards it. A traversal is a bijection of `0 .. N-1` and knows nothing about
  rows. That is `docs/Observers.md`: an observer answers *which column next*, and
  `C` is not part of that question.

---

## 5. The timing contract

This section is the observer's analogue of the cycle-accurate diagram in
`docs/Hardware_Architecture.md`. It fixes the one latency the observer adds,
states what is sampled and what is observed, and gives the behaviour at the two
discontinuities: the final column, and an invalidated context.

### 5.1 The one rule

> **A `start` or a `trigger` accepted at edge `k` is presented as a visit during
> cycle `k+1`, and for exactly that cycle.**

Everything else follows. The latency from request to visit is **one clock**, and
it is the same one clock for the first visit of a pass and for every visit after
it. The observer adds no other delay, because the evaluation it triggers is
combinational and adds none.

```text
        cycle   1        2        3        4        5        6
             ┌────────┬────────┬────────┬────────┬────────┬────────┐
clk          │        │        │        │        │        │        │
             └────────┴────────┴────────┴────────┴────────┴────────┘
start  ──────┐
             └──────────────────────────────────────────────────────
trigger ──────────────────────┐        ┌────────┐        ┌────────
                              └────────┘        └────────┘
running ──────────────┐
                      └──────────────────────────────────────────────
column  ──────────────<      0       >< 1 >    < 2 >    < 3 >
(pi(t))               <              ><   >    <   >    <   >
visit_valid ──────────┐   ┌───┐    ┌───┐    ┌───┐    ┌───┐
                      └───┘   └────┘   └────┘   └────┘   └─────────
done  ──────────────────────────────────────────────────┐   ┌──────
                                                        └───┘
                                              (N = 4, oneshot = 0)
```

Reading the diagram, which is the whole contract in one picture:

1. `start` in cycle 1 → the first visit in cycle 2 (`t = 0`, `column = 0`).
2. Each `trigger` produces the next visit in the **following** cycle.
3. `column` is stable from one visit to the next; it is *meaningful* only in the
   cycle `visit_valid` is high.
4. `done` is high in the **same** cycle as the visit of `t = N-1` (cycle 5
   above), not after it.
5. With `oneshot = 0`, the trigger in cycle 5 wraps: `t <- 0`, and cycle 6 is the
   first visit of the next pass.

### 5.2 Sampled or observed?

The distinction matters and the answer is different for the two directions of the
interface:

| Quantity | Registered? | By whom sampled |
| --- | --- | --- |
| `column` | yes, by the engine (`col_q`) | it *is* a register; stable across cycles |
| `visit_valid`, `done` | yes, one-cycle strobes | the consumer latches on the rising edge of `visit_valid` |
| `column_bits` | **no** | a combinational output of `bcmc_column` for the current `column`; the consumer samples it in the `visit_valid` cycle |
| `N`, `C`, `valid` | yes, in the wrapper | read combinationally by the engine every cycle |

`column_bits` is **observed combinationally and sampled by the consumer**, never
registered by the observer. Registering it would put a cycle of latency between a
column and its evaluation, which is exactly the "start query, poll ready"
invention the transaction specification forbids. The observer's contribution is
that the *column* is a register (so it is glitch-free) while the *evaluation*
stays combinational (so it has no latency).

A consumer that needs glitch-free output registers `column_bits` itself, in the
`visit_valid` cycle — and that is what v2.0b's output engine does. It is the
consumer's register, not the observer's: the observer is not permitted to decide
what a visit is *for*.

### 5.3 The final column

The last visit of a pass is `t = N-1`, and in that cycle three things are true
together:

- `visit_valid` is high — it is a visit like any other;
- `done` is high — this pass is complete;
- `column = pi(N-1)`, and `column_bits = M(:, pi(N-1))` is valid in the same
  cycle.

After that cycle, `column` **keeps its value** until the next accepted trigger.
The observer does not clear it, and it does not change it to a "null" column.
This is deliberate: the matrix still exists, the last column is still a legal
query (`0 <= N-1 < N`), and a consumer such as a GPIO bank that keeps driving its
last pattern needs no special "end of pass" case. What changes at the pass
boundary is only that `done` was high and that the *next* visit will be `pi(0)`
of a new pass.

A one-shot application therefore reads: latch on `visit_valid`; when `done` is
high in that same cycle, stop. There is no window between "the last visit" and
"the end of the pass" to reason about, at any `N`, including `N = 1`.

### 5.4 When the context becomes invalid

The timing, as distinct from the state rule of 3.10:

- `visit_valid = visit_q & valid` is combinational (3.7), so the suppression of a
  visit is **immediate, within the same cycle**, not delayed by a clock. In the
  cycle `valid` falls, no visit is seen even if `visit_q` is high.
- The abort of the pass takes effect on the **next** edge (3.10): `state_q <- IDLE`,
  `running <- 0`, `aborted <- 1`.
- A `trigger` arriving in or after that cycle is ignored, because the engine is
  in `IDLE`. No visit is queued. There is no backlog and none can form.
- The output engine sees `visit_valid` simply stop. Its last latched value
  persists (that is the output engine's own policy, v2.0b), and it changes again
  only on the first visit of the next started pass.

So the latency from "a weight is written" to "no more visits" is the latency of
`STATUS.VALID` falling — which is the wrapper's, not the observer's — plus zero.
The observer adds no window during which a stale `column_bits` can be latched,
which is the whole obligation restated at the level of cycles.

### 5.5 Contention with the bus: none

Because the observer has its **own** `bcmc_column` instance (4.4), a pass and a
software `COLUMN`/`CELL` read never share logic and never contend. The timing
consequences are worth stating explicitly because they are what lets the observer
claim a fixed latency:

- A visit's timing does not depend on bus activity. `trigger` → visit is always
  one clock, whether the bus is idle or saturated.
- Software's `COLUMN` and `CELL` reads keep exactly the timing they have today —
  answered in the same access, one-cycle `ack` — whether the observer is running
  or not.
- There is no arbitration, no priority, and no stall path between the two. The
  only shared input is the flat representation itself, which is a read-only wire.

This is the concrete payoff of section 4.4. Had the instance been shared, the
first two bullets would each become conditional, and a fixed trigger→visit
latency could not be promised.

---

## 6. Integration with the existing Wishbone peripheral

### 6.1 What is added to `bcmc_wb.v`: outputs only

The sideband is a group of **outputs**, carrying values the wrapper already
computes and currently keeps internal:

```verilog
    //--- BCMC observation sideband (v2.0a; outputs only) --------------------
    output wire [VAL_W-1:0]       obs_n_o,
    output wire [IDX_W-1:0]       obs_c_o,
    output wire                   obs_valid_o,
    output wire [MAX_C*VAL_W-1:0] obs_weights_flat_o,
    output wire [MAX_C*VAL_W-1:0] obs_offsets_flat_o,
```

These are pure observations of `n_q`, `c_q`, `STATUS.VALID`, and the two flat
vectors already routed from the Context to the wrapper's own `u_column`. No new
state, no new logic, and — critically — **no new inputs**. The observer cannot
write anything back into the wrapper in v2.0a, so the arrow is one-way and the
wrapper's behaviour cannot depend on the observer.

Because the wrappers in this project are instantiated with **named** port
connections (`.clk (clk), …`), an existing instantiation that does not mention
the new outputs simply leaves them unconnected, and Verilog permits that. The
existing harnesses — `sim/bcmc_wb_test.cpp`, `sim/tb_wb.v`,
`sim/bcmc_driver_test.cpp`, `sim/example_host.cpp` — therefore need **no
change**, and the bus functional model still sees identical wires. A future
harness that wants to observe the sideband connects the five new outputs by name.

Interface width, for the record: at the reference geometry (`MAX_C = 64`,
`VAL_W = 16`, `IDX_W = 16`) the sideband is `1024 + 1024 + 16 + 16 + 1 = 2081`
bits, all of it on-chip. It is not a board-level bus and is not proposed as one.

### 6.2 What is *not* added

The constraints the outline set as non-negotiable are met by construction, and
each is worth naming because each is a thing that would have been easy to do and
wrong to do:

| Not added | Why it stays absent |
| --- | --- |
| a `NEXT_COLUMN` register | `docs/Observers.md`: no observer may be *privileged*. A register that software writes to advance the hardware traversal would make the software path the privileged one. |
| traversal semantics in `Register_Map.md` | `docs/Register_Map.md` is the BCMC contract. Traversal is not BCMC, so it cannot appear in the BCMC map — the same reason `docs/Observers.md` sits above it. |
| a new error condition (E5) | the observer adds no bus access, so it adds nothing for the error model to judge. E1–E4 are unchanged. |
| a change to `CTRL`/`STATUS` meanings | the observer does not reuse `CTRL.START`. Turning on an observer is not starting a transform, and conflating them would put traversal on the BCMC map by the back door. |
| any new field in `CAPS` | the observer's geometry is its own (6.3); `CAPS` describes the BCMC peripheral, and it is unchanged. |

### 6.3 The observer's register window

The observer needs a small control surface — `START`, `STEP`, `ONESHOT`, and a
status it can be polled through — and that surface lives in its **own** address
window, never in the BCMC map. Following the v0.4a discipline, the window is a
specification written before its decode, in a new file:

```text
docs/Observer_Register_Map.md  ->  validation/observer_periph.py  ->  (decode, v2.0a)
```

A sketch of the window, offered here so the shape is concrete rather than
deferred (the full document is a v2.0a deliverable):

| Offset | Name | Access | Width | Meaning |
| --- | --- | --- | --- | --- |
| `0x000` | `OBS_ID` | RO | 32 | `0x4F425356`, the ASCII bytes `OBSV` |
| `0x004` | `OBS_VERSION` | RO | 32 | major, minor, patch |
| `0x008` | `OBS_CAPS` | RO | 32 | `MAX_C`, `VAL_W`, `IDX_W` of the observer |
| `0x00C` | `OBS_CTRL` | RW | 32 | `START` (W1S), `STEP` (W1S, the software trigger source), `ONESHOT`, `EN` |
| `0x010` | `OBS_STATUS` | RW | 32 | `RUNNING`, `DONE` (sticky), `ABORTED` (sticky), later `SEED_READY` |
| `0x014` | `OBS_PASS` | RO | 32 | completed-pass counter |
| `0x018`+ | reserved | — | — | mode/seed from v2.0c |

Three properties of this window matter to the architecture, not just its layout:

- **It is a separate slave, not a second decode range in `bcmc_wb.v`.** Keeping
  the two maps in two modules is what makes "the BCMC map has no traversal in it"
  a structural fact rather than a review note.
- **`START` and `ONESHOT` drive the engine's `start` and `oneshot`; `STEP` is the
  v2.0a *software trigger source*, not the engine's `trigger` port** (sections 3.4
  and 3.5). The pulse `STEP` produces is fed to the trigger-source mux, and the
  mux's output is `trigger`. The window is a thin skin over the engine's ports and
  their sources; it holds no traversal state of its own, and `STEP == trigger` is
  a v2.0a configuration fact rather than an architectural identity.
- **`ABORTED` is sticky and cleared by `START`**, mirroring `IRQ`'s
  acknowledge-by-action lifetime (section 3.10). A driver can tell a completed
  pass from an aborted one, which it could not if the abort were silent.

### 6.4 The existing transaction sequences are unchanged — worked example

Take S2 (load a BCMC instance) followed by S7 (a weight write mid-pass), now with
a pass in flight:

```text
                        BCMC peripheral                     observer
  ---------------------------------------------------------------------------
  W 0x014  N            N <- N
  W 0x018  C            C <- C
  W 0x400+4i weight[i]  weight[i] <- ...;  VALID <- 0
  W 0x00C  0x1          START; transform runs
  R 0x010  -> VALID=1   VALID <- 1,  matrix exists
  ---------------------------------------------------------------------------
  W OBS_CTRL START      (no BCMC access)      pass begins; visits pi(0)..
  ... triggers ...      (no BCMC access)      visits happen; 0 transactions each
  ---------------------------------------------------------------------------
  W 0x400+4k weight'[k] weight'[k] <- ...;  VALID <- 0
                                             visit_valid <- 0 (same cycle)
                                             pass aborted; ABORTED <- 1
  E 0x028               COLUMN read refused  (observer idle; still refused)
  W 0x00C  0x1          START; transform runs
  R 0x010  -> VALID=1   VALID <- 1
  ---------------------------------------------------------------------------
  W OBS_CTRL START      (no BCMC access)      new pass begins from pi(0)
```

Every line above the divider, and every line on the BCMC side, is exactly the
sequence `docs/Transaction_Sequences.md` already specifies — including the `err`
that S7 requires. The observer adds two things, both in its own window: a `START`
that does not touch the BCMC map, and a silence on the bus where the software
observer would have performed a write and two reads per visit.

This is the sense in which the hardware path is "cleaner than the software bus
path": the software observer runs `W CELL_COL; R COLUMN[0]; R COLUMN[1]` per
visit, and the hardware observer runs *nothing*, because `CELL_COL` was never a
register — it was always just the argument to a function, and the observer holds
that argument in a register of its own and passes it directly to the same
function.

---

## 7. Verification plan

The rule of the house, unchanged: nothing is checked against values a harness
invented. Expected outputs come from a model that was validated first, and where
the model and the document disagree, the document is right and the model is a
bug.

```text
docs/Hardware_Observer_Architecture.md
        │
        ├── validation/observer_hw.py   (the golden engine: states, cycles, pi)
        └── validation/test_observer_hw.py
                    │
                    ├── validation/observers.py   (golden traversals: sequential, permuted)
                    └── validation/reference.py   (the golden matrix: bcmc_cell/row/column)
                                │
                                v
                    sim/bcmc_observer_hw_test.cpp  (the engine vs the models, every cycle)
                    sim/tb_observer.v              (the second opinion, Icarus)
```

### 7.1 The two golden models

**The golden traversal is `validation/observers.py`.** v2.0a's `pi` is the
sequential identity, which is already pinned there and already checked by
`validation/test_observers.py` for O1, P1–P4 and the bijection property. The
hardware engine is held to *that* `pi`, index for index, exactly as
`sw/bcmc_observer.c` is. v2.0a adds no new traversal to model; it adds a new
*consumer* of the existing one.

**The golden matrix is `validation/reference.py`.** Every `column_bits` the
engine produces must equal `reference.bcmc_column(N, weights, offsets, column)`
for the same context — the same reference the Core, the cell, the row, the
column and the peripheral were all validated against.

**The golden *engine* is `validation/observer_hw.py`**, a new cycle-level model:
it takes `(N, start_cycle, trigger_cycles)` and emits the same `(column,
visit_valid, done)` the RTL must, cycle by cycle, including the one-cycle
latency, the pass boundary, and the abort on `!valid`. It is the analogue of what
`validation/bcmc_periph.py` is to `bcmc_wb.v`: a model written from *this*
document, executed against the test vectors before any RTL exists. In the v0.4a
spirit, it is expected to find defects in *this document* first — that is what it
is for.

### 7.2 Exhaustive small-`N` sequential tests

The Evaluator's exhaustive philosophy carries over unchanged, because the
interesting bugs in a traversal are off-by-one bugs at small `N`:

- **Every `N` in `1 .. N_MAX`**, driven by a recorded trigger sequence, with the
  emitted `pi(t)` checked against the sequential reference — including `N = 1`,
  which is where the pass-boundary comparison is wrong most often.
- **Every `(N, oneshot)` pair**, so both endings are exercised at every length.
- **Every legal context at small `(N, C)`**, reusing the generator that produced
  `sim/vectors/cell_exhaustive.txt`, so that `column_bits` is compared against
  `reference.py` for every column of every case — the same 6,734-case enumeration
  the cell already owns, now driven through the engine's cursor instead of by a
  testbench loop.
- **The degenerate contexts**: `C = 0` (every visit emits nothing), `N = 1`,
  `N = 2`, and a single-row context.

### 7.3 Integration tests against the existing Core and Evaluator

The engine must be exercised with a *real* context, not a hand-set one:

1. Load a BCMC instance through the real driver into the verilated
   `rtl/bcmc_wb.v` — the same C code and the same RTL the whole project uses.
2. Start the observer; drive `trigger` from the harness.
3. On every visit, compare `column_bits` against `reference.py` for the loaded
   context, and compare the bus access count across the pass.
4. Repeat for the contexts `validation/reference.py` recorded, so the cases are
   the ones the rest of the project already agrees on.

This is the hardware analogue of `sim/bcmc_observer_test.cpp`: it holds the
engine to the *same* matrices the software observer was held to, through the
*same* peripheral.

### 7.4 O1, O2, P1–P4, explicitly

Each clause becomes a hardware check, and each is checked on assembled RTL:

| Clause | Hardware check |
| --- | --- |
| **O1** | over one pass, the `N` values on `column` when `visit_valid` is high are a permutation of `0 .. N-1` — checked by the bijection routine the project already has (`bcmc_order_is_bijection`, adapted to a stream) |
| **O2** | for each visit, `column_bits` equals `R(pi(t))`, the rows `reference.py` says are active in column `pi(t)`, in ascending bit order |
| **O3** | `pi` is unchanged when the *trigger phase* is changed: the same `N` with triggers delayed, advanced or burst produces the same `column` sequence, only at different cycles |
| **P1** | the multiset of `(i, pi(t))` over a pass is exactly the support of `M`; the pass emits exactly `W = sum(weights)` set bits |
| **P2** | counting set bits per row over a pass gives `weights[i]`, for every `i` |
| **P3** | the multiset of `popcount(column_bits)` over a pass equals `r` copies of `q+1` and `N-r` copies of `q` (the Balance Theorem, re-derived from the RTL output, not assumed) |
| **P4** | deferred to v2.0c, where a second traversal exists to compare against; the machinery is the `--summary` diff of `scripts/run_examples.sh`, pointed at hardware |
| **conservation** | every accepted request (an accepted `start` or `trigger`) schedules exactly one visit, and every scheduled visit is either presented or suppressed; with no invalidation `visits == accepted requests`, and under invalidation at most one visit per falling `VALID` is suppressed (F11) |

**O3 deserves the emphasis it is given here**, because it is the one clause a
hardware observer could break and a software observer could not. The software
observer's trigger is the program counter; the hardware observer's is an external
wire that a timer or a zero-cross detector drives, and it may be irregular. The
check is therefore *not* "the output is deterministic given the trigger
schedule" — that would be trivially true and would prove nothing — but "the
`column` sequence is independent of the schedule". A testbench that jitters the
trigger and diffs only the sequence of visited columns is what makes O3 mean
something in hardware.

**The conservation invariant is the companion to O3**, and it is stated as a
*count* because a count is what catches the errors a sequence diff can miss when
the wrong answer happens to look plausible. Its precise form -- every accepted
request schedules exactly one visit, every scheduled visit is presented or
suppressed, and the suppressed count is bounded by the invalidations (F11) -- is
what makes it subsume the individual failure modes the mutation battery (7.6)
plants one at a time: a double advancement, an accidental auto-wrap
(the boundary producing two visits), a trigger honoured in `IDLE`, a trigger
honoured during an invalidation, and an off-by-one at `N - 1` each break it. It is
expressed over *accepted* triggers rather than *asserted* ones precisely because
section 3.11 makes refusal a stated, countable event rather than a silent drop.

### 7.5 Cost claims are metered, not asserted

The load-bearing claim of v2.0a is that **a pass costs zero bus accesses**. It is
a claim about traffic, so nothing but counting can settle it. The harness meters
the Wishbone BFM around the entire pass and asserts the access count does not
move — the same method that pinned `1 + ceil(MAX_C/32)` for a visit and `0` for a
traversal in v0.5b. Additionally, the harness checks that a *software* `COLUMN`
read *during* a pass still costs exactly one access and still returns the right
word, because that is what "the observer does not perturb the bus" means.

### 7.6 The mutation battery

The negative control is the same one every component has been given: plant
plausible bugs one at a time and require that each is caught. At minimum, for the
engine: the pass-boundary comparison written `!=` instead of `<` (fails at
`N = 1`); `done` emitted one cycle late; `visit_valid` not gated by `valid`; a
pass that resumes instead of aborting on `!valid`; `column` cleared at the pass
boundary; a trigger accepted in `IDLE`; the wrap presenting `pi(0)` without a
trigger; reset releasing in `RUN`; a cursor that wraps at `N-1` instead of `N`;
a `start` with `N = 0` entering `RUN`; and `trigger` winning over `start` when both
are asserted in `IDLE`. Every one of these is a bug a careful implementer could
write, and every one must be caught by a *named* test.

### 7.7 Two simulators, and lint

As for every RTL component: a Verilator harness (`sim/bcmc_observer_hw_test.cpp`,
RTL == the Python model cycle by cycle; the name carries `_hw_` because
`sim/bcmc_observer_test.cpp` is already the *software* observer's conformance
test) and an Icarus testbench (`sim/tb_observer.v`, a second opinion that shares
no code with the harness).
Both are lint-clean under `verilator --lint-only -Wall` and `iverilog -g2005
-Wall`, and both are wired into `sim/Makefile` and ctest so the stage is green
only when everything above passes. No FPGA build is required for any of it; the
deferral of the board costs the observer nothing (v0.6 in `dev/ROADMAP.md`).

---

## 8. v2.0b — the output engine

The output engine is what turns a visit into a physical effect. It is listed
here, not specified in full, because it is an **application** and BCMC does not
define applications (`docs/Observers.md`). What this document fixes is the
boundary and the discipline.

### 8.1 Boundary

```text
        bcmc_observer                       bcmc_out_engine
   column_bits ─────────────────────────>  .column_bits
   visit_valid ─────────────────────────>  .visit_valid
   done        ─────────────────────────>  .done
   C           ─────────────────────────>  .C
                                           .pins ─────────────> GPIO / PWM
```

It has no `column`, no `trigger`, no `N`, no context, and no bus. It cannot
advance the traversal, because it has no wire with which to do so. This is the
hardware form of `docs/Observers.md`'s "an application chooses a traversal
strategy and never implements one": the output engine is downstream of the choice
and structurally cannot make it.

### 8.2 Behaviour

1. On `visit_valid`, latch `column_bits` (masked to `C` bits; the bits at or
   above `C` are already zero, section 4.3).
2. Drive the latched value onto the output pins.
3. Between visits, **hold the last value**. There is no "idle" output state,
   because a visit is a *state*, not an *event*: `column_bits` is the set of
   consumers active in this slot, and a consumer not in the set is simply off.
   Holding it is what makes `N` a division of the trigger period.
4. On `done`, nothing special happens. The last visit's pattern stays latched
   until the next visit, which is `pi(0)` of the next pass (section 5.3). A
   one-shot application that wants to blank the outputs on `done` does so itself;
   that is policy, not contract.

Optional, and deliberately left to v2.0b's own document: per-pin polarity, a
per-row duty cycle, an enable mask, and a packed-word output mode for driving a
bank wider than `MAX_C`.

### 8.3 No back-pressure

The output engine never stalls the observer. If it did, the fixed trigger→visit
latency of section 5 would become conditional, and the observer's timing contract
would depend on an application — exactly the coupling this architecture exists to
prevent. If an application needs back-pressure, that is a statement that its
trigger is too fast, and the fix belongs in the trigger, not in the engine.

---

## 9. v2.0c — traversal sources

### 9.1 The seam, filled

Section 3.3 fixed the interface. v2.0c supplies bodies. Three sources are
planned, and they are not variations on one theme — they trade storage, rate and
conformance against each other, and the trade is the interesting part.

| Source | `pi(t)` | Storage | Rate | Conformance |
| --- | --- | --- | --- | --- |
| identity | `t` | none | 1 visit/cycle | already pinned (`docs/Observers.md`) |
| affine | `(a t + b) mod N` | two registers | 1 visit/cycle | new family, needs its own spec |
| Fisher–Yates | a shuffled table | `N` entries | ≤ 1 visit / ~2 cycles | index-for-index with `sw/bcmc_observer.c` |

### 9.2 Affine, streaming

```text
pi(t) = (a * t + b) mod N,      gcd(a, N) = 1
```

Bijection for free: `t -> a t + b (mod N)` is invertible exactly when
`gcd(a, N) = 1`. For `N` a power of two that is every odd `a`. It needs two
registers, one multiply-add and the project's existing `mod N` idiom (one
comparator and one conditional add, the same one v0.3 used for the Evaluator), and
it streams at **one visit per cycle**. The seed selects `(a, b)`.

The cost is that it is not the family `sw/bcmc_observer.c` pins. That is a real
cost and section 11 keeps it as a decision: an affine source is cheap and fast
but a *new* traversal, and a new traversal owns its own specification, reference
and mutation battery.

### 9.3 Fisher–Yates, buffered — and the fill bound

The software shuffle of `docs/Observers.md` is the family the project already
pins: a 32-bit SplitMix generator, a rejection draw, downward swaps including
`j == i`. Reproducing it index for index in hardware buys the existing
conformance tables (`sim/vectors/observer_prng.txt`, 11 seeds over 40 lengths)
unchanged — a considerable prize. The obstacle is not arithmetic. It is **time**.

A shuffled traversal cannot be computed on demand one step at a time; `pi(t)`
depends on the whole previous history of swaps. It must be *built* into a bank of
`N` entries before the pass that reads it. So the question is whether a bank can
be built within the time a pass gives you, and the answer is: **not at full
rate.**

```text
building one bank of N entries
  N-1 shuffle steps
  each step: uniform(i) = >= 1 SplitMix next(), plus a rejection retry
             acceptance probability (i+1)/2^k  >  1/2,  so  < 2 next() on average
             plus one bank write

  =>  measured ~1.4 N:  1.03 N at N = 8,  1.33 N at N = 64,  1.34 N at N = 128,
      approaching ln 2 * 2 = 1.39 asymptotically
      (validation/observer_hw.py, bank_fill_cycles; the test prints the table)

one pass at full rate
  =>  N  visits,  N  clocks
```

A single bank therefore cannot be filled within the pass that consumes it when
the trigger fires faster than roughly one visit per two clocks. This is the
sharpest constraint in the whole stream, and it is stated here rather than
discovered in RTL because it *is* the architectural decision:

1. **Confirm it in the reference model first.** `validation/observer_hw.py` gains
   `bank_fill_cycles(N, seed)` and `test_observer_hw.py` asserts the bound over
   the recorded seeds and lengths. If the model says a bank needs `c_fill(N)`
   cycles and a pass is `N` visits at one visit per `R` clocks, the source is
   legal exactly when `c_fill(N) <= N * R`.
2. **Double-buffer with a stated lead.** Two banks; the source streams bank `k`
   while the generator fills bank `k+1`. The lead must be at least
   `ceil(c_fill(N) / (N * R))` passes — one pass if the trigger is slow enough,
   two or more if it is not. The lead is a configuration fact of the device, and
   the contract is `c_fill` versus the trigger rate, nothing more.
3. **Reject, do not underflow.** If the next bank is not ready when a pass begins,
   the source withholds `ts_ack` (section 3.3), and — because a stalled visit can
   swallow a trigger — the source also reports `SEED_UNDERRUN` in `OBS_STATUS`.
   This is the same "drop, not defer" discipline as section 3.10, applied to the
   one source that can be late.

The recommendation that falls out is concrete, and is taken as this document's
position: **make the affine source v2.0c's streaming reference traversal, and
offer a Fisher–Yates source as the buffered one whose fill bound is explicit.**
The affine source answers "which column next" with a bijection at full rate and
no storage, which is the property a hardware observer most wants; the shuffle is
offered where index-for-index conformance with the software family is worth a
bank, a lead, and a bounded trigger rate. Offering both, with the bound stated,
keeps `docs/Observers.md`'s promise that traversals may be replaced or extended
without touching the primitive — v2.0c is a source swap, not an engine change
(3.3), so this choice is genuinely reversible.

### 9.4 The register window by v2.0c

`OBS_CTRL` gains a source selector and `OBS_SEED`/`OBS_A`/`OBS_B` follow
(section 6.3). Writing a seed or a source clears `SEED_READY`, which is the
observer's analogue of the wrapper's "writing a weight clears `VALID`": a bank
built from an old seed does not describe the new one. The engine is unchanged;
only the source behind the seam changes.

---

## 10. Stress-testing the design against the existing contracts

The design above was tested against the contracts the project already committed
to, not merely written beside them. Each finding below is a decision the
stress-test forced; several are the kind that only appear when a new component is
held against an old promise.

**F1 — `N` must stay in the wrapper, not move into the Context.** The observer
needs `N` to wrap the cursor. The natural-looking move is to give `bcmc_context.v`
an `N` port, since the observer is "part of the same system". That would destroy
the one claim v0.4b spent the most effort making checkable — "this module contains
no BCMC mathematics … it has no `N` port and no `C` port". So `N` is taken from
the wrapper, which has always owned it, and the Context is untouched. This is
recorded because the wrong version is *easier*.

**F2 — the Evaluator is a second instance, and the doctrine needed restating.**
`bcmc_wb.v` argues against "a second `bcmc_cell` instance". Read carelessly, that
forbids the observer's own evaluator. Read correctly, it forbids a second
*implementation*, which this is not (section 4.4). The stress-test's value is the
sharpening: the rule is about divergent *logic*, not about *instances*, and the
proof obligation is discharged by the existing compositional-equivalence test
rather than waved away.

**F3 — a pass must abort on invalidation, or O1 becomes vacuous.** O1 says a pass
visits every column once. If a pass could span a context change — resume after
`!valid` — it would still satisfy O1 *syntactically* while visiting two different
matrices. `docs/Transaction_Sequences.md` S7 refuses a `COLUMN` read in exactly
that window for exactly that reason. The engine inherits the refusal and extends
it to "the pass dies", which is a statement O1 alone does not make; it comes from
the transaction specification, which the observer is obliged by (section 3.10).

**F4 — `visit_valid` must be gated on `valid` combinationally, not by an FSM
branch.** An FSM that checks `valid` at the top of a cycle still emits one more
visit in the cycle `valid` falls. The register map's E4 is a *same-cycle* refusal,
so the observer's must be too (section 3.7). This is the difference between
"eventually stops" and "cannot present a stale bit", and only the latter is the
claim worth making.

**F5 — `done` coincides with the last visit; it does not follow it.** The Core's
`done` is a one-cycle pulse and the register map explains that a pulse is useless
to a poller, which is why `VALID` and `IRQ` exist. The observer is consumed by
hardware, not pollers, so it keeps the pulse semantics — but it aligns the pulse
*with* the visit rather than after it, because a consumer latching on
`visit_valid` must be able to read `done` in the same cycle.

**F6 — a wrapped pass begins on a trigger, not automatically.** "Continuous" mode
could have wrapped `pi(0)` on the edge after `done`. Doing so would make the
boundary trigger produce two visits, breaking the one-trigger-one-visit rule
precisely where it is hardest to see. The wrap waits for a trigger (section 3.8).

**F7 — the traversal seam introduces a handshake that "no handshake" does not
forbid.** `Register_Map.md`: "There is no 'start query, poll ready' sequence
anywhere in this map, and there never will be." A request/acknowledge on the
traversal source looks like exactly that. It is not, and the distinction is worth
its own sentence: the clause is about *evaluation* (`M` is a function, so a query
has no latency), whereas a shuffled traversal source reads a table and a table
read has a cycle. The handshake delays *which* column is asked about, never *how
long the answer takes* (section 3.3).

**F8 — a Fisher–Yates source cannot keep up with a one-visit-per-cycle stream.**
This is the finding that shapes v2.0c. The shuffle's per-step rejection draw
makes a bank cost roughly `1.4N` cycles to fill (measured, section 9.3), while the pass that reads it
is `N` cycles at full rate. So the buffered source has a bounded trigger rate and
a required bank lead, both of which are now explicit rather than discovered in
RTL (section 9.3). The affine source exists *because* of this finding.

**F9 — O3 in hardware is invariance to the trigger phase, not determinism given
a schedule.** The software observer's trigger is the program counter, so
determinism is nearly free. A hardware trigger is an external, possibly irregular
wire, so "the same input gives the same output" becomes vacuous unless it is
stated as "the visited sequence is independent of the trigger schedule" (section
7.4).

**F10 — the zero-bus claim is only meaningful if a software read still works
during a pass.** Removing the bus from the observer's path is easy to state and
easy to overclaim: it would be possible to build a version in which a pass
monopolises the evaluator and software reads slow down. The dedicated-instance
decision (4.4) is what lets the claim be stated for *both* directions at once,
and section 7.5 meters both.

**F11 — the conservation invariant is false under invalidation, and the model
found it.** Section 3.11 originally claimed that refusing a trigger in the cycle
the context dies keeps the visit count equal to the accepted-trigger count. The
cycle model (`validation/observer_hw.py`, the first artefact after the freeze)
falsified that on its first invalidation run: a trigger accepted in cycle `k`
schedules its visit for `k + 1`, and if `VALID` falls during `k + 1` the cursor
has moved but the visit is suppressed by the gate of 3.7. The count is short by
one, not equal. The corrected statement -- every accepted request schedules
exactly one visit, every scheduled visit is presented or suppressed, and at most
one visit per falling `VALID` is suppressed -- is now in 3.11 and 7.4. There is a
second, quieter consequence: "a pass is `N` visits" (O1) is a statement about a
pass that is allowed to finish, and an aborted pass is a *prefix* of one, which is
the precise sense in which an abort is not a pass.

**F12 — the section 3.11 table did not resolve `trigger` against a one-shot
`done`.** The rows `done & oneshot` (leave `RUN`) and `trigger & RUN & valid`
(advance) can both be true on the same edge, and the table as first written did
not say which wins -- so the table was not, quite, the "complete decision rule" it
claimed to be. The model had to choose in order to be written at all, chose
**completion priority** (the pass has ended, so there is no cursor to advance, and
the trigger is ignored), and that choice is now a row of 3.11 rather than an
inherited accident. In continuous mode there is no conflict: a trigger at
`t = N - 1` is the wrap trigger, and it is meant to begin the next pass.

---

## 11. Decisions taken, and what remains

### Taken here

1. The observer is a **separate block** (`rtl/bcmc_observer.v`) reached over an
   **observation sideband**, with its own `bcmc_column` instance — not by sharing
   the wrapper's evaluator, and not by the bus (4.4, 6.1).
2. The engine is a **two-state FSM with one cursor**, one-cycle trigger→visit
   latency, `visit_valid` combinationally gated on `VALID`, `done` coincident
   with the last visit, and **abort-on-invalidation** (3, 5).
3. The **traversal-source seam** is a request/acknowledge pair, present in v2.0a
   with an identity source, so v2.0c is a source swap (3.3).
4. The wrapper gains **output-only** sideband ports and no inputs, no register and
   no error condition; the existing harnesses need no change (6.1, 6.2).
5. The observer's control surface is a **separate register window**
   (`docs/Observer_Register_Map.md`), never a change to the BCMC map (6.3).
6. **`N` is not added to the Context** (4.5, F1).
7. The streaming reference traversal for v2.0c is **affine**; Fisher–Yates is a
   **buffered** source with an explicit fill bound (9.3).
8. The **trigger-source mux** is an explicit architectural element even though
   v2.0a instantiates only the software source: `STEP` is that source's pulse, not
   the engine's `trigger` port (3.5, 6.3).
9. `N = 0` does **not** start a pass: `start` requires `N >= 1`, so the engine has
   no "running but empty" state (3.4, 3.11).

### Remaining, and who decides

| Open question | Decided by | When |
| --- | --- | --- |
| The geometry of the reference instance (`VAL_W`, `IDX_W`, `MAX_C`, max `N`) | the v2.0a register-map document | before RTL |
| Whether the observer starts as its own Wishbone slave or as a bare `start`/`trigger` wire for a bench harness | the v2.0a register-map document | before RTL |
| Which trigger sources v2.0a advertises (software kick only, or timer/zero-cross/pin) | the v2.0a register-map document | before RTL |
| Affine seed policy: how `(a, b)` are chosen from a 32-bit seed, and the `gcd(a, N) = 1` guarantee | a v2.0c specification | with 9.2 |
| Whether Fisher–Yates must be index-for-index with `sw/bcmc_observer.c` | a v2.0c specification | with 9.3 |
| Whether the stream is tagged independently as v2.0a/… (this document's assumption) | the roadmap owner | now, cheaply reversible |

The last row is already acted on: `dev/ROADMAP.md` and `README.md` record Tang
Nano as deferred and place v2.0a–d as an independent stream, which is the
assumption this document makes.

### v2.0a is frozen

With the event table of 3.11 added, the `N = 0` dead state removed (3.4), the
synchronous/combinational split made orthogonal (3.7, 3.10), and `STEP`
re-framed as a trigger *source* rather than the trigger itself (3.5), nothing in
sections 3–6 is open at the level of behaviour. Every remaining item in the table
above chooses a geometry, an address range or a trigger set; none of them changes
the state machine, the one-clock timing, or the contracts at the seams.

That artefact now exists. `validation/observer_hw.py` is the cycle-level model
derived directly from this document, and `validation/test_observer_hw.py` drives
it through all of 3.11, O1, O2, P1–P3 and the section 9.3 bound. Executing the
specification, as v0.4a executed `docs/Transaction_Sequences.md`, found two
defects — **in this document, not in an implementation**: the conservation
over-claim (F11) and the unresolved `start`/`done`/`trigger` overlap (F12). Both
are corrected above. That is the outcome the document-first discipline exists to
produce: the model is written from the specification in order to falsify it, and
`rtl/bcmc_observer.v` is written only once the model is green, to satisfy it.

---

## Sources and status

This document makes concrete the decisions left open by
`docs/Hardware_Observer.md`, which distils `notes/v2_Architecture.md` and
`notes/hardware_observer_discussion.md` (a private, untracked notes directory).

It is anchored to the contracts the project already committed to, and where it
repeats them it names them: `docs/Observers.md` (O1–O3, P1–P4, and the four
prohibitions), `docs/Register_Map.md` (no `NEXT_COLUMN`, E1–E4, "evaluation has no
handshake", `VALID` as the bit that means a matrix exists),
`docs/Transaction_Sequences.md` (S2, S4, S5, S7), and the RTL interfaces of
`rtl/bcmc_context.v`, `rtl/bcmc_column.v` and `rtl/bcmc_wb.v` that make the
sideband possible.

**Status.** Sections 3 to 6 are v2.0a **specification**: an implementation is held
to them. Sections 8 and 9 are **architecture**: they fix boundaries and a
recommendation, and each will be given its own specification before its RTL.
Section 11 lists what is still open, and nothing in it should be treated as
committed until it is.

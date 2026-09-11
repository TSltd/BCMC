# BCMC Hardware Observer

> Status: design outline for the next development stage. This document records
> the architecture the project has converged on, fixes the boundaries that must
> not be crossed, and lists what must be decided before it becomes a
> specification. It is deliberately not a specification yet: the discipline of
> this repository is document first, Python reference second, RTL third, and a
> spec commits to words that an implementation must then be held to.

This document is the next-stage companion to `docs/Observers.md`. Observers.md
is the contract — *what* a traversal may do, and what it may not. This document
is the architecture proposal for *where* that traversal may live: in hardware,
beside the construction it consumes, so that software is asked only for what it
alone can supply.

---

## 1. The gap this closes

Today every observation of a BCMC matrix passes through the CPU.

```text
trigger (timer / zero-cross / irq)
    │
    ▼
CPU: choose next column          pi(t)
    │
    ▼
CPU: read column                bcmc_read_column() over Wishbone
    │
    ▼
CPU: update outputs             write GPIO
```

The Balance Theorem has already done the hard part: the matrix is balanced, and
the observer cannot touch that. But the observer still runs as software, one
bus transaction per visit, and the CPU therefore owns the **time axis**. It is
interrupted on every tick, chooses a column, reads it, and acts on it. That is
the whole loop, and for a heater or a GPIO scheduler it is all the CPU ever
does.

A hardware observer does not change the mathematics. It moves the third of the
project's three questions — *which coordinate next* — from software into
hardware, exactly as the Core already owns the first (*where do the intervals
begin?*) and the Evaluator the second (*what is the state at this
coordinate?*).

```text
trigger (timer / zero-cross / irq)
    │
    ▼
Observer Engine: advance          pi(t+1)
    │
    ▼
Evaluator: evaluate               column → column_bits
    │
    ▼
Output Engine: present            GPIO / PWM / ...

          (no CPU)
```

The CPU's remaining job is the only one that is genuinely its own: **choose the
weights**. Software sets a target distribution of activity; the hardware turns
it into a balanced, autonomously walked schedule.

```c
bcmc_load(dev, weights, n, c);       /* exactly as today */
hw_observer_config(dev, mode, seed, trigger);
hw_observer_start(dev);
/* nothing. The observer owns the time axis from here. */
```

This is a configurable-then-forgettable engine, like a timer: *configure,
enable, forget*. It begins to resemble the comparison the design notes kept
returning to — not a scheduler library, but a DMA controller or an RP2040 PIO:
a peripheral that exists to remove the CPU from repetitive, deterministic work.

---

## 2. What does not change

Nothing about BCMC changes. The Core is unchanged. The Context is unchanged.
The Evaluator is unchanged. The Wishbone wrapper is unchanged. The observer
contract of `docs/Observers.md` is unchanged, and the new hardware is obliged
by it in the same way the software observer is:

- **O1 — Completeness:** over one pass, `pi` is a bijection of
  `0 .. N-1`. Every column is visited exactly once.
- **O2 — Fidelity:** visiting `j` yields exactly `R(j)`, the rows the
  characteristic function says are active in column `j`, in ascending order.
- **O3 — Determinism:** `pi` depends only on its declared inputs. A seeded
  observer is a deterministic function of a seed, in hardware as in software.

The consequences P1–P4 of `docs/Observers.md` then hold for the hardware
observer automatically, because they follow from O1 and O2 alone and are
properties of the matrix, not of its observer. A hardware observer that changed
a proven property of BCMC would not be an observer; it would be a second
construction wearing the first one's name.

The reference traversals and the example applications also survive unchanged.
`matrix_dump`, `gpio_scheduler` and `heater_controller` do not know how their
traversal is produced. The `Application × Traversal` product of v0.5c is a
property of the interface, not of the implementation of either axis; a
hardware-supplied traversal is just another implementation of the same `pi`.

---

## 3. Principles — the boundaries this stage must not cross

The project's decomposition is its main asset, and the hardware observer is the
first new block since v0.1 that risks blurring it. The boundaries below are
therefore stated as rules, not as taste:

1. **The observer remains a separate IP block.** It is a sibling of BCMC that
   happens to consume it, exactly as `sw/bcmc_observer.{h,c}` consumes the
   driver today. Nothing from it is merged into `rtl/bcmc_{core,cell,row,
   column,context,wb}.v`.
2. **An observer is a traversal strategy, not an application.** The hardware
   observer may answer *which column next* and *when*; it must not acquire
   application meaning — heater, LED scan, queue service — any more than the
   software observer does. Meaning belongs to the block that consumes
   `column_bits` (an Output Engine, or whoever reads them).
3. **An observer must not hold the matrix.** It holds a traversal order, `pi`.
   `M` is evaluated on demand from the stored `(weights[], offsets[])`, as it
   is today. The observer must not cache `column_bits`: that would be a
   framebuffer, and a framebuffer would have to answer for its coherence.
4. **The observer engine must not generate permutations.** Traversal order is
   produced by a separate Traversal Generator; the engine merely walks it.
   Fisher–Yates is one generator, an affine map is another; replacing one with
   the other must change nothing downstream. This is the hardware analogue of
   "building an order and starting a pass cost zero bus accesses" in
   `sw/bcmc_observer.h`.
5. **The observer must not own a random source.** Randomness, when a traversal
   needs any, is drawn from an existing RNG elsewhere in the SoC. The observer
   declares a seed and a source; it owns neither.
6. **No privileged traversal.** The register map deliberately has no "next
   column" register. The hardware observer must not smuggle traversal state
   back onto the bus as if it were part of the BCMC programming model: the CPU
   configures the observer and reads its status, and does not steer it tick by
   tick.
---

## 4. Architecture

```text
            CPU
             │   weights, observer config
             ▼
   ┌───────────────────┐        ┌───────────────────────┐
   │   BCMC Core       │        │  Traversal Generator  │
   │   weights → off.  │        │  builds pi            │
   └─────────┬─────────┘        └───────────┬───────────┘
             ▼                              │  seeded, from RNG
   ┌───────────────────┐                    ▼
   │   BCMC Context    │        ┌───────────────────────┐
   │ (weights, offsets)│        │   Observer Engine     │
   │  flat outputs     │        │   walks pi on trigger │
   └─────────┬─────────┘        └───────────┬───────────┘
             ▼                              │  column index
   ┌───────────────────┐                    ▼
   │   Evaluator       │◄───────────────────┘
   │ bcmc_column       │        column → column_bits
   └─────────┬─────────┘
             ▼
   ┌───────────────────┐
   │   Output Engine   │   what the bits mean
   │ GPIO / PWM / DMA  │
   └───────────────────┘
```

The diagram preserves the project's separation as independent mechanical
questions, each owned by a different block:

- **Core** — *where do the intervals begin?* (unchanged)
- **Evaluator** — *what is the state at this coordinate?* (unchanged)
- **Observer Engine + Traversal Generator** — *which coordinate next?* (new, hardware)
- **Output Engine** — *what does it mean?* (unchanged in role; becomes hardware-facing)

The Observer Engine and the Traversal Generator are shown as separate boxes
deliberately. They are **not** one module: the engine consumes a traversal
source, and the generator fills one. The engine should not know whether the
next column came from a counter, a permutation table, an affine map or a DMA
descriptor. The generator should not know that its output walks a BCMC matrix.

### 4.1 The query path: how the observer reads the matrix

The existing Evaluator already has exactly the interface the observer needs.
`rtl/bcmc_context.v` exposes both windows combinationally as flat outputs
(`weights_flat`, `offsets_flat`), and `rtl/bcmc_column.v` turns
`(N, C, column, weights_flat, offsets_flat)` into `column_bits` —
combinational, no handshake, because `M(i,j)` depends on nothing but its own
arguments. That is the same storage the Wishbone wrapper's own `bcmc_column`
reads; the observer's column evaluation is simply another reader of the same
context.

Consequences:

- **No bus traffic.** A visit costs the observer nothing on the Wishbone bus.
  The software observer's cost claim — *a visit is exactly one
  `bcmc_read_column()`* — is inverted for the hardware observer: a visit is
  zero bus accesses, because there is no bus in the path.
- **No arbitration change.** The Context's two writers (software writes
  weights, the Core writes offsets) are the only contending clients. An
  additional read-only, combinational consumer changes nothing about ownership
  in time.
- **No stale data.** The observer holds `pi`, not `M`. Changing the weights
  changes `column_bits` for the same `j` on the next visit, and the observer
  cannot serve a stale answer because it never stored an answer.

The exact packaging — whether the observer is integrated in a new top module
beside the Context and a column instance, or the Context's flat ports are
brought out to a sideband — is an open design point (see "[Open
questions](#open-questions)"). The interface below is independent of it.

### 4.2 The Observer Engine

The engine owns two things and nothing else: **when** a visit happens, and
**which column** that visit addresses.

- A **trigger source** selects *when*. Candidates: a software command, a timer,
  a zero-cross detector, an external pin, a hardware event. The trigger changes
  only *when* traversal advances; it never changes the traversal itself.
- A **step counter** runs `0 .. N-1` over a pass. At the end of a pass the
  engine either stops (`DONE`), repeats the same pass, or — when it is fed by a
  generator that can produce a fresh order — begins the next pass, exactly as a
  software loop may rewind and walk `pi` again.
- A **column request** presents `column = pi(t)` to the Evaluator. `column_bits`
  settle combinationally from the stored context and are sampled by the Output
  Engine.

The engine holds no permutation and generates none. Its entire state is the
step counter, the trigger selection and the run/stop control. The first engine
is a counter and a state machine; describing it takes longer than building it.

---

## 5. The Traversal Generator

The generator builds traversal orders. It is named for what it supplies — an
order — rather than for any particular way of producing one, because
"permutation generator" names an implementation when the block is meant to
name an interface. The engine consumes an order; it must not care which of the
following produced it.

### 5.1 Sequential — `pi(t) = t`

One counter. Zero storage. This is the identity traversal, the control case
from `docs/Observers.md`. It is the entire v2.0a engine by itself, and it is
what makes the simplest hardware observer almost trivial to build.

### 5.2 Fisher–Yates — the pinned shuffle, stored

The permutation family already pinned in `docs/Observers.md`: a downward
Fisher–Yates shuffle over a seeded 32-bit SplitMix source with a bounded
rejection draw. The software observer stores the result because the shuffle is
fundamentally not a streaming algorithm; so does hardware. Storing it has a
concrete cost (section 7); generating it into a fresh bank while the engine
walks the current one is why the generator and the engine are separate blocks
(section 6).

A hardware Fisher–Yates has one decision to make before it can be held to the
existing conformance tables: whether it reproduces the software permutation
**index for index** from the same seed, or defines its own family. Reproducing
it buys the entire existing test machinery (`gen_observer_vectors.py`,
`observer_prng.txt`, `observer_order.txt`) unchanged; it also commits the
hardware to the exact SplitMix constant-multiplier sequence and the rejection
draw. That is an open question, not an assumption — see section 11.

### 5.3 Affine maps — `pi(t) = (a·t + b) mod N`

Provided `gcd(a, N) = 1`, the map is a bijection of `0 .. N-1`, it is
computable in one multiply-add and a bounded compare — the project's favourite
kind of 'mod' — and it needs **no storage and no RNG**. An affine traversal is
the cheapest non-trivial permutation hardware can produce, and it is streaming
by construction.

Two caveats, honestly stated. The family is far smaller than the set of all
permutations, so it cannot stand in for a shuffle where uniformity matters;
and because the canonical matrix concentrates its heavy columns at `j < r`, an
affine map trades that concentration for an arithmetic progression of heavy
positions. Whether that is smooth enough for a given application is an
empirical question — and it is one the existing harness can answer, because P3
checks exactly the multiset of per-step occupancies a traversal produces.

### 5.4 External table — software supplies the order

The generator is a RAM bank that software (or anything else) may fill with any
conforming `pi`. This is the traversal the software observers already know how
to build, loaded once and walked by hardware afterwards. It costs nothing in
the visit path and moves the permutation cost to wherever the caller already
pays it.

### 5.5 Future sources

LFSR-based orders, DMA-fed orders, priority orders, Gray codes — any future
source satisfies the same two requirements: it produces a bijection of
`0 .. N-1` (O1), and it is a deterministic function of its declared inputs
(O3). Nothing downstream changes when one replaces another. An LFSR, for
example, is a natural streaming candidate, but not every LFSR configuration
yields a full-period cycle over the column range, so it must be pinned and
verified exactly like everything else here.
---

## 6. Double-buffered permutation banks

A permutation that takes thousands of cycles to build must not stop the
observer for thousands of cycles. Nothing in the construction requires it:
Fisher–Yates builds a *fresh* order while the engine walks a *finished* one,
and the two are disjoint buffers.

```text
                 ┌ ─ ─ ─ ─ ─ ─ ─ ─ ─ ┐
                 │   generator side   │
            fills│                    │consumes
                 ▼                    ▼
   ┌──────────────────────┐  ┌──────────────────────┐
   │  permutation bank B  │  │  permutation bank A  │
   └──────────────────────┘  └──────────────────────┘
                 △                    △
                 │   atomic swap at   │
                 └──── end of pass ───┘
```

Buffer ownership is exchanged **atomically at the end of a pass**: the engine
starts walking the just-finished bank, and the generator immediately begins
building the next order in the bank that was just exhausted. This is exactly
the double-buffer discipline of a framebuffer, applied to an order instead of
an image, and it is the reason the engine and the generator must be separate
blocks — a single module would have to be two modules pretending to be one.

The requirement is architectural, not implementation-specific: the buffers may
be block RAM, SRAM or registers, and the "ping-pong" wording used in early
discussions was deliberately dropped because "registers" implies flip-flops
and this storage is almost certainly not that. What matters is the atomic
exchange and the absence of any window in which the engine is starved.

A pass lasts `N` triggers. The generator must therefore fill a bank in no more
than the time of one pass, or the engine will have nothing to exchange to. For
a Fisher–Yates generator, one shuffle of `N` entries is `N-1` draws with
rejection; the timing budget is an open question that the reference model will
have to bound (section 8).

---

## 7. Storage costs

One permutation of `N` column indices needs `N · ceil(log₂ N)` bits.

```text
N =   256       256 ×  8 =   2,048 bits ≈   0.25 KiB per bank
N =  1024     1,024 × 10 =  10,240 bits ≈   1.25 KiB per bank
N =  4096     4,096 × 12 =  49,152 bits ≈   6.0  KiB per bank
N = 65536    65,536 × 16 = 1,048,576 bits ≈ 128.0  KiB per bank
```

Double-buffered, double each figure. At the project's current default widths
(`VAL_W = 16`, so `N ≤ 65535`), even the upper bound is comfortably expressible
in FPGA block RAM; at the sizes a heater, GPIO scheduler or LED scan actually
uses (`N` in the tens to low thousands), a bank is a rounding error.

Sequential traversal (`pi(t) = t`) and affine traversal need **zero** storage.
The full-shuffle cost is the price of the stronger family, and it is the only
cost — it is paid once per order, not once per visit.
---

## 8. Verification plan — the same discipline, applied to a new block

Nothing about the verification culture changes because the observer is
hardware. The pipeline stays

```text
spec  →  Python reference  →  test vectors  →  RTL  →  two simulators
```

and the hardware observer earns its claims in exactly the way the Core,
Evaluator, Context, wrapper, driver and software observers earned theirs:

1. **The specification first.** Whatever this document leaves open (section 11)
   must be pinned — in the language of `docs/Observers.md`, with the same
   precision — before any RTL exists. An unpinned "traversal" is a comment.
2. **A Python reference.** A reference model of each traversal generator
   (sequential, the pinned Fisher–Yates and/or affine) and of the engine's
   pass semantics, in the style of `validation/observers.py`. The engine model
   is nearly free: its entire behaviour is "emit `pi(t)` on trigger `t` and
   count to `N`". The generators are where the work is.
3. **Vectors from the reference, never invented.** `gen_observer_vectors.py`
   already writes `pi` for the software observers; the hardware generators add
   theirs to the same style of file, index for index. If the hardware
   Fisher–Yates reproduces the pinned shuffle, it is held **index for index to
   the existing tables** — the same obligation `sw/bcmc_observer.c` already
   discharges. An affine source writes its `(a, b, N) → pi` tables the same
   way.
4. **Two simulators, one truth.** A Verilator harness and an Icarus
   testbench, both comparing against the Python reference — not against each
   other and not against numbers a test author typed. Exhaustive enumeration
   for small `N` (the sweep discipline) plus randomised cases at the width
   limits.
5. **The structural checks are mandatory, not customary.** On every
   assembled matrix, the harness checks O1 (pi is a bijection — cheap to
   prove on a permutation table, important to prove on a streaming source),
   O2 (column_bits are exactly `R(j)`, which the existing `bcmc_column`
   verification already owns at the cell level), O3 (same seed, same order,
   across both simulators), and the derived P1–P4 (row conservation and the
   balance multiset survive the traversal without exception).
6. **The cost claim is metered, not asserted.** The software observers are
   held to *"building an order and starting a pass cost zero bus accesses; a
   visit costs one `bcmc_read_column()`."* The hardware observer's
   corresponding claim is *"a visit costs zero bus accesses"* — nothing on the
   Wishbone bus happens at all — and the harness counts bus activity around
   every visit to keep it true. The observer's own logic is also bounded: one
   column request per trigger, never two, never zero when armed.
7. **Mutation testing.** The suite must be able to fail. The established
   negative control — planting plausible observer bugs one at a time and
   demanding each is caught — applies to the observer RTL as it applied to
   `sw/bcmc_observer.c`. A faulty step counter, a shuffle that allows `j == i`
   to be excluded, a bank swap that happens a cycle early, an affine `a` that
   is not coprime to `N`: each must be found.
8. **The negative control of the whole design.** The examples' orthogonality
   diff (`scripts/run_examples.sh`) remains the check that **no single
   testbench can make**: the same application driven by a hardware traversal
   must produce byte-identical summaries to the same application driven by
   software. When the observer is real RTL, running the heater controller over
   the hardware engine with the contents of its permutation bank held against
   the Python-written table is the strongest form the whole project has of
   saying "the observer contributed order, and nothing else."

### 8.1 Reference-model obligations

The reference model must be able to answer the timing question section 6 left
open: how long a Fisher–Yates shuffle takes, worst case, including rejection
draws, given a pinned RNG — because the generator must fill a bank within one
pass or the double buffer starves. This is exactly the kind of bound the
project likes to prove in the model before committing the RTL to a scheduler.
---

## 9. Roadmap

The hardware observer is **independent of the v1.0 release stream**. The
deferred Tang Nano demonstration (v0.6), formal verification (v0.7),
documentation polish (v0.8) and ecosystem work (v0.9) do not depend on it, and
it does not depend on them. BCMC the IP stays finished at v1.0; the observer is
the first block of the **BCMC ecosystem** that grows around it, and the 
decomposition that keeps the two separate is precisely what makes this true.

The stages below follow the state machine: each finishes only when the stage
above it is still green, and nothing downstream is trusted until everything
upstream of it has passed.

### v2.0a — the Observer Engine (sequential first)

- The Observer Engine: trigger select, step counter `0 .. N-1`, run/stop,
  `DONE`, IRQ.
- The query path: column request into the Evaluator, `column_bits` out, zero
  bus accesses (metered).
- Sequential traversal — the identity `pi(t) = t`. The engine with no
  generator attached must already be a conforming observer (O1–O3), because
  the counter is the control case.
- A first Wishbone register set: mode, trigger, start/stop, status (`BUSY`,
  `DONE`). Whether these registers live on the existing wrapper, on a new
  observer slave, or on a sideband of a new top module is open question 7.
- The Python reference, vector files, both simulators, the cost meter, and the
  mutation battery — the whole of section 8, for `pi(t) = t`.

### v2.0b — the Output Engine

- A GPIO backend: one register written per visit from `column_bits` (or a
  masked subset), with an optional per-tick interrupt.
- The three example applications run against the hardware observer, with the
  orthogonality diff of section 8 kept byte-identical to the software runs.
  This is the demonstration the deferred Tang Nano work was going to provide —
  and it remains available to be shown on real silicon whenever the FPGA
  stream resumes, because the observer neither needs nor is needed by it.

### v2.0c — the Traversal Generator

- The double-buffered permutation banks and the atomic exchange.
- Fisher–Yates against the RNG interface (an existing SoC RNG; the observer
  owns no random source), held index-for-index to the existing Python tables
  if decision question 1 so chooses.
- Affine traversal (`pi(t) = (a·t + b) mod N`, `gcd(a, N) = 1`) and the
  external-table traversal as alternative sources, each pinned, each verified.
- The timing bound of section 6 — the generator must fill a bank within one
  pass — proved in the reference model before the scheduler exists.

### v2.0d — applications and further backends

- PWM and DMA-oriented outputs; CAN-style multi-queue service behind a single
  peripheral; LED scanning; anything application-shaped.
- Each new backend is an Output Engine, not an observer change. The traversals
  do not change; O1–O3 and P1–P4 do not change; the engine does not change.

### Not yet scheduled

AXI-Lite wrapping, multi-instance BCMC, and a software **Policy Engine** that
turns high-level targets (temperatures, queue depths, priorities) into weight
vectors are all downstream of the observer and are explicitly outside this
stage — see section 10.
---

## 10. Deliberately out of scope

Being explicit about what this stage is *not* is as important as being explicit
about what it is. None of the following belongs to the hardware observer, and
the moment any of them is proposed as an extension of it, the extension has
left the observer's concern:

- **Control and policy engines** — PID, deadline, priority, queue-depth
  weighting. These produce *weights*. They consume nothing the observer
  produces, and the observer must never produce a weight. (See
  `notes/policy_engine.md`, which records the design discussion.)
- **Application backends with domain meaning** — heater semantics (that a bit
  on `column_bits` switches a triac), CAN arbitration, LED scan patterns.
  These belong to Output Engines and application logic, not to the observer.
- **An RNG.** The observer declares a seed and draws from an existing SoC
  random source. It does not contain one, because a random source is exactly
  the kind of block that wants its own lifecycle and its own consumers.
- **Traversal generation inside the engine** — the engine walks; the generator
  generates; neither knows the other's algorithm (principle 4).
- **AXI-Lite wrapping**, **multi-instance BCMC**, and **software policy
  engines** — downstream items, recorded in the "Not yet scheduled" part of
  section 9.
- **The Tang Nano demonstration.** Deferred by decision; the observer neither
  requires it nor is required by it (section 9).

---

## 11. Open questions

The decisions below must be made before this document is promoted from an
outline to a specification. They are listed rather than decided because
deciding them is the point of the review that this document is asking for.

1. **Permutation fidelity.** Must a hardware Fisher–Yates reproduce the
   software permutation *index for index* from the same seed (reusing the
   pinned SplitMix constant multipliers and the rejection draw), or may it
   define its own shuffle family? Index-for-index buys the existing conformance
   tables unchanged; a new family needs its own spec, vectors and mutation
   battery.
2. **The first permuted traversal.** Affine maps cost nothing and stream; a
   shuffled table costs RAM and a build phase but is the family software
   already pins. Which is v2.0c's reference source — and is `a` required to be
   a power of two for the multiply-add, or is any odd `a` acceptable when `N`
   is a power of two (where `gcd(a, N) = 1` ⟺ `a` odd)?
3. **Trigger sources for v2.0a.** Software kick only, or also timer,
   zero-cross and external pin? All four are advertised in section 4.2; the
   register set and the reference model differ in size accordingly.
4. **Where the observer ends.** Does the v2.0a engine terminate at
   `column_bits` (a consumer external to the block), or does it include an
   output register so that "the block" is demonstrable on its own?
5. **Column evaluation placement.** One shared Evaluator instance inside the
   observer's top module, driven by the Context's flat outputs; or the mapped
   CELL/COLUMN window through the wrapper (bus traffic per visit, reusing the
   register map unchanged)? The first removes the bus; the second changes
   nothing about the existing wrapper. A hybrid — sideband for the observer,
   register window for software — is also possible.
6. **Register ownership.** New registers on `bcmc_wb.v` (a wrapper change the
   project has so far avoided), a separate Wishbone slave for the observer, or
   a sideband configuration port with its own minimal handshake? This is the
   one decision that can disturb the existing contract surfaces.
7. **Geometry of the reference instance.** `VAL_W`, `IDX_W`, `MAX_C`, and the
   maximum `N` the permutation banks must hold. The current defaults bound `N`
   at 65,535, which is far beyond what section 7's storage table suggests is
   sensible for the first device.
8. **Relationship to v1.0.** Should the observer stream be tagged independently
   (v2.0a/…) as section 9 proposes, or folded into the v0.x numbering of the
   existing roadmap? The proposal in this document is independence: the release
   stream and the ecosystem stream do not block each other.

---

## Sources and status

This outline distils design discussions that predated it, including the
architecture sketch in `notes/v2_Architecture.md` and the extended
give-and-take in `notes/hardware_observer_discussion.md` (a private, untracked
notes directory). It reconciles those with the contracts the project already
committed to: `docs/Observers.md` (the observer contract), `docs/Register_Map.md`
(no "next column" register), and the RTL interfaces of `rtl/bcmc_context.v` and
`rtl/bcmc_column.v` that make the sideband query path possible.

Its status is an outline, and section 11 is the explicit list of what must be
decided to make it a specification. Nothing in it should be treated as
committed until it is.
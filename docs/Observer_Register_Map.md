# BCMC Observer Register Map

This document is **specification**, not documentation of an implementation.

`docs/Register_Map.md` is the contract that `validation/bcmc_periph.py` and
`rtl/bcmc_wb.v` must satisfy. This file plays the same role for the observer's
own control window, one level up:

```text
docs/Hardware_Observer_Architecture.md  ->  validation/observer_hw.py      ->  rtl/bcmc_observer.v
docs/Observer_Register_Map.md           ->  validation/observer_periph.py  ->  (decode, v2.0a)
```

Where an implementation and this document disagree, this document is right and
the implementation is a bug.

---

## 1. What this window is

The observer engine of `rtl/bcmc_observer.v` has four inputs that are not the
matrix — `start`, `trigger`, `oneshot`, `rst` — and six outputs that describe
what it is doing — `column`, `visit_valid`, `done`, `running`, `aborted`, and
the `column_bits` it produces through its own projection. Something has to
connect those wires to a bus, and this document is that something.

The window is small on purpose. It is a **skin**, and its whole job is to let a
driver do four things and read five:

- **start** a pass, **advance** one step, choose whether a pass is **one-shot**,
  and **reset** the engine;
- read whether a pass is **running**, whether one **completed**, whether one was
  **aborted**, how many have completed, and what the block is.

## 2. What this window is not

The section that matters most, because it is the one a register map is most
likely to get wrong.

> **This window exposes and controls the observer semantics that
> `docs/Hardware_Observer_Architecture.md` already froze. It introduces no
> traversal semantics of its own.**

Three consequences, each of which rules out a design that would have been easy
to write and wrong to ship:

1. **No register redefines the state machine.** Where this document says what a
   control bit does, it *names the section 3.11 row* the bit reaches, and does
   not restate it. A register map that spells out its own transition table
   becomes a second specification of one FSM, and the two will drift; that is
   the same objection `rtl/bcmc_wb.v` makes to a second implementation of the
   characteristic function, applied one level up.
2. **No register gates what the engine does not gate.** An `EN` bit was
   considered — "arm the trigger sources" — and **deliberately not added**,
   because the engine has no such gate: `trigger` is accepted or ignored by the
   rule of section 3.11 and by nothing else. Adding `EN` would have made the
   register window and the engine disagree about when a visit happens. A gate
   belongs to a *trigger source* (v2.0c, when a floating pin exists to gate),
   not to the engine.
3. **No `NEXT_COLUMN`, and no readable "current column".** `docs/Observers.md`
   forbids a privileged observer. `NEXT_COLUMN` would privilege software over
   hardware; a readable `column` register would invite software to build a
   second, polled traversal path underneath the hardware one. Both are refused,
   and the second is worth stating because it looks harmless: the hardware
   harnesses observe `column` directly, which is what they are for, and nothing
   in the driver needs it.

The BCMC register map is not extended. This is a **separate slave in a separate
address region**, so "the BCMC map contains no traversal" stays a structural fact
rather than a review note (section 6.2 of the architecture document).

---

## 3. Bus

Identical to `docs/Register_Map.md`, because there is no reason for it not to be
and a second bus dialect would be one more thing to get wrong.

| Property      | Value                                                        |
| ------------- | ------------------------------------------------------------ |
| Bus           | Classic **Wishbone B4**, slave, registered feedback not used  |
| Data width    | 32 bits                                                      |
| Granularity   | 32 bits — word accesses only                                 |
| Addressing    | byte addresses, all registers 32-bit aligned                 |
| Bursts        | none                                                         |
| Pipelining    | none                                                         |
| Acknowledge   | single cycle: every accepted cycle terminates in `ack`       |
| Errors        | single cycle `err`                                           |
| Region size   | 4 KiB, **separate from the BCMC peripheral's 4 KiB**          |
| Clock domains | one. There is no CDC in the observer either.                 |
| Endianness    | none — no sub-word access exists                             |

Every bus cycle terminates in exactly one of `ack` or `err`, in the cycle after
`stb & cyc`, and never in both and never in neither.

## 4. The trigger sources

Section 3.5 of the architecture document specifies a **trigger source mux**
between the outside world and the engine's `trigger` port, and says which sources
v2.0a advertises is a decision. **This document makes it: v2.0a advertises one
source, software.**

```text
   OBS_CTRL.STEP  ------>|                        |
   (v2.0c: timer) ------>|   trigger source mux    |-----> trigger ---> engine
   (v2.0c: pin)   ------>|                        |
```

`OBS_TRIG` reports the advertised set as a bitmask, so a driver **discovers**
which sources exist rather than assuming — the same discipline `CAPS` applies to
geometry.

| Bit | Source   | v2.0a       |
| --- | -------- | ----------- |
| 0   | software | yes         |
| 1   | timer    | no — v2.0c  |
| 2   | pin      | no — v2.0c  |

`OBS_CTRL.STEP` is therefore **explicitly exposed in v2.0a**, as the software
source's pulse. It is *not* the engine's `trigger` port and does not become one:
the mux still exists, with one input populated, and v2.0c fills the others
without a change here or in the engine.

## 5. Address map

`MAX_C`, `VAL_W` and `IDX_W` are the observer's synthesis parameters, reported at
run time in `OBS_CAPS`. They are the *observer's* copies of the geometry; the
BCMC peripheral reports its own, and an integrator that elaborates the two blocks
differently will see the difference rather than be told a comfortable lie.

| Offset          | Name          | Access | Width | Meaning                                          |
| --------------- | ------------- | ------ | ----- | ------------------------------------------------ |
| `0x000`         | `OBS_ID`      | RO     | 32    | `0x4F425356`, the ASCII bytes `OBSV`              |
| `0x004`         | `OBS_VERSION` | RO     | 32    | major, minor, patch                              |
| `0x008`         | `OBS_CAPS`    | RO     | 32    | `IDX_W`, `VAL_W`, `MAX_C`                        |
| `0x00C`         | `OBS_CTRL`    | RW     | 32    | `START`, `STEP`, `RESET` (all W1S), `ONESHOT`     |
| `0x010`         | `OBS_STATUS`  | RW     | 32    | `RUNNING` (RO), `DONE`, `ABORTED` (RW1C)         |
| `0x014`         | `OBS_PASS`    | RO     | 32    | completed-pass counter                           |
| `0x018`         | `OBS_TRIG`    | RO     | 32    | advertised trigger sources                       |
| `0x01C`–`0x3FF` | —             | —      | —     | unmapped (E1)                                    |

Reserved bits of any register read `0`, and writes to them are ignored — field
width doing its job, deliberately not an E-condition.

---

## 6. The error model

> Silent success hides bugs.

An access that is not meaningful returns **`err`**, and a write that returns
`err` has **no side effect whatsoever**. There are four reasons, all structural:

| #   | Condition                                                        |
| --- | ---------------------------------------------------------------- |
| E1  | the address is not mapped                                        |
| E2  | the access type is wrong: a write to a read-only register        |
| E3  | the access is not a full 32-bit word (`sel != 4'b1111`)          |
| E4  | the access asks the engine for something the engine would ignore |

E4 is the interesting one, and it is deliberately *derived* rather than invented.
`rtl/bcmc_observer.v` **ignores** a `start` it cannot accept and a `trigger` in
the wrong state; that silence is right for a wire and wrong for a bus. So the
window turns it into an error, by the engine's own acceptance rule:

```text
START is refused unless   !RUNNING  &  VALID  &  N >= 1     (section 3.4)
STEP  is refused unless    RUNNING                          (section 3.5)
```

This mirrors the BCMC map's E4, which refuses a `COLUMN` read while `!VALID`: the
hardware would not answer meaningfully, so the bus says so rather than
acknowledging nothing.

**One boundary cycle, stated rather than hidden.** In the single cycle carrying
a one-shot pass's closing visit, `RUNNING` is still 1, so a `STEP` is
acknowledged — and the engine then ignores it, because completion has priority
over a trigger (F12 in the architecture document). The step is not silently lost:
`OBS_STATUS.DONE` latches in that same cycle, so the driver observes the pass end.
E4 mirrors the engine's acceptance rule; it cannot shadow the one case where two
of the engine's own rules meet.

**What is not an error.** Violating a mathematical precondition is not an error
here either, for the same reason it is not one in the BCMC map — with one
exception. `N = 0` is not a legal instance and the engine refuses to start a pass
with it (section 3.4), so `START` with `N = 0` is E4 rather than undefined.
Writing `ONESHOT` is never an error: it is a mode bit, and a mode bit that could
only be set in one state would be a second state machine wearing a register's
name.

---

## 7. Register detail

### `OBS_ID` — `0x000`, RO

`0x4F425356`, the ASCII bytes `OBSV`. A driver that reads anything else is not
talking to this block.

### `OBS_VERSION` — `0x004`, RO

Same field layout as the BCMC map: `MAJOR[31:16]`, `MINOR[15:8]`, `PATCH[7:0]`.
This document defines `0x00000100` — version 0.1.0.

### `OBS_CAPS` — `0x008`, RO

| Bits    | Field   | Meaning                           |
| ------- | ------- | --------------------------------- |
| `31:24` | `IDX_W` | bits in `C` and in a row index    |
| `23:16` | `VAL_W` | bits in `N` and in a column index |
| `15:0`  | `MAX_C` | rows this observer can evaluate   |

The same encoding as the BCMC peripheral's `CAPS`, so one parser reads both.

### `OBS_CTRL` — `0x00C`, RW

| Bits   | Field     | Access | Meaning                                                      |
| ------ | --------- | ------ | ------------------------------------------------------------ |
| `31:4` | —         | —      | reserved                                                     |
| `3`    | `ONESHOT` | RW     | mirrors the engine's `oneshot` input (section 3.8)            |
| `2`    | `RESET`   | W1S    | pulses the engine's `rst` (section 3.9); reads `0`            |
| `1`    | `STEP`    | W1S    | the software trigger source's pulse (section 3.5); reads `0`  |
| `0`    | `START`   | W1S    | the engine's `start` (section 3.4); reads `0`                 |

Each field reaches exactly one frozen engine input. The three W1S bits
self-clear and have no readable value; writing `0` does nothing. A read returns
`ONESHOT`, with the other three reading `0`.

**Why `RESET` is here and `EN` is not.** The engine has a `rst` input and no
enable, so the window exposes the first and would have had to invent the second.
`RESET` earns its place because without it a **continuous** pass has no stop:
`START` is refused while `RUNNING`, and the only other way to end a pass is to
invalidate the context, which destroys the matrix (section 3.10). `RESET` is a
one-cycle pulse to `rst` and nothing else — it does **not** touch the context,
does not clear `VALID`, and does not clear `ONESHOT`. A driver that wants a pass
it can regain control of should set `ONESHOT`; a driver that wants to stop a
continuous one uses `RESET`.

### `OBS_STATUS` — `0x010`, RW

| Bits   | Field     | Access | Reset | Meaning                                    |
| ------ | --------- | ------ | ----- | ------------------------------------------ |
| `31:3` | —         | —      | `0`   | reserved                                   |
| `2`    | `ABORTED` | RW1C   | `0`   | a pass was cut short by an invalid context  |
| `1`    | `DONE`    | RW1C   | `0`   | a pass completed; write `1` to clear        |
| `0`    | `RUNNING` | RO     | `0`   | the engine's `running` output               |

`ABORTED` is the engine's `aborted` latch (section 3.10): set when the context
goes invalid mid-pass, and cleared by writing `1`, by `RESET`, or by a `START`
the engine accepts — because a restart *is* the acknowledgement of an abort.
Three ways to clear one bit, and all three acknowledge the same event; the BCMC
map's `IRQ` has the same acknowledge-by-action lifetime.

`DONE` is set by the engine's `done` pulse (section 3.8) and cleared by writing
`1` or by `RESET`. It is **latched rather than a pulse**, for the reason the BCMC
map gives about its own `done`: a pulse is invisible to software that polls.

`RUNNING` is the engine's `running` output, unlatched. A driver uses it to tell
"a pass is in flight" from "a pass ended", and `ABORTED` to tell *how* it ended —
the three endings of section 3.10, each visible in exactly one bit combination.

### `OBS_PASS` — `0x014`, RO

A 32-bit count of completed passes, incremented on every `done` and cleared by
`RESET`. It wraps. Reported, never used to decide anything.

### `OBS_TRIG` — `0x018`, RO

The advertised trigger sources, as described in section 4. `0x1` in v2.0a.

---

## 8. Programming sequences

### T1 — Start a pass

The observer is not armed by the BCMC load; it is armed by `START`, and only once
the matrix exists.

```text
R 0x000  -> 0x4F425356        OBS_ID
R 0x008  -> geometry          OBS_CAPS
R 0x018  -> 0x00000001        OBS_TRIG: software only

W 0x00C  0x00000009           ONESHOT=1, START=1
R 0x010  -> RUNNING=1         the first visit was pi(0)
*        ...                  poll, or wait for DONE
R 0x010  -> DONE=1 RUNNING=0
W 0x010  0x00000002           clear DONE
R 0x014  -> 1                 one pass has completed
```

`START` is refused with **`err`** while the BCMC context is invalid, so a driver
that starts the observer before the transform completes gets a refusal rather
than a pass that never visits. That ordering requirement is the one real
coupling between the two windows, and it is explicit in both documents.

### T2 — Step a continuous pass

```text
W 0x00C  0x00000001           ONESHOT=0, START=1
R 0x010  -> RUNNING=1
W 0x00C  0x00000002           STEP
W 0x00C  0x00000002           STEP
*        ...
R 0x014  -> k                 passes completed so far
```

One `STEP` is one visit, at the rate software drives it. This is the same
traversal the hardware would run from any other source; the register map is not
party to the difference.

### T3 — Stop a continuous pass

```text
W 0x00C  0x00000004           RESET
R 0x010  -> RUNNING=0 DONE=0 ABORTED=0
R 0x014  -> 0                 the counter was cleared too
R 0x008  -> geometry          the context is untouched; VALID is still 1
```

`RESET` clears the observer and only the observer. The matrix survives it, so a
driver may restart immediately — with `START`, not with a re-run of the
transform.

### T4 — The context dies mid-pass

This is the sequence the whole invalidation discipline exists for.

```text
R 0x010  -> RUNNING=1 ABORTED=0        a pass is in flight
W (BCMC) 0x400+4k weight'[k]           BCMC VALID drops to 0
                                       the engine aborts in the same cycle
R 0x010  -> RUNNING=0 ABORTED=1        next cycle: no visit was presented
W 0x00C  0x00000001                    START now refused: E4, !VALID
E 0x00C  -> err                        (no side effect)
E 0x00C  -> err                        STEP too: E4, !RUNNING
   ... (BCMC: re-run the transform, poll VALID=1) ...
W 0x00C  0x00000009                    ONESHOT=1, START=1 -> accepted
R 0x010  -> RUNNING=1 ABORTED=0        the restart acknowledged the abort
```

Nothing here needs the observer to be told the context died. It does not have a
`VALID` register to poll; it takes `VALID` from the sideband (section 4.1 of the
architecture document) and aborts by itself. The window only *reports* the
consequence.

---

## 9. Sequences that must fail

As much a part of the specification as T1-T4, because "the access is refused" is a
promise the driver relies on. Each must terminate in `err` and leave no trace.

| #   | Sequence                          | Why                                   |
| --- | --------------------------------- | ------------------------------------- |
| F1  | `R 0x048`                         | unmapped (E1)                         |
| F2  | `W 0x000` — write `OBS_ID`        | read-only (E2)                        |
| F3  | `W 0x014` — write `OBS_PASS`      | read-only (E2)                        |
| F4  | any access with `sel != 4'b1111`  | not a word (E3)                       |
| F5  | `W 0x00C` `START` while `RUNNING` | E4 — the engine is already in a pass  |
| F6  | `W 0x00C` `START` while `!VALID`  | E4 — no matrix to traverse            |
| F7  | `W 0x00C` `START` while `N = 0`   | E4 — not an instance (section 3.4)    |
| F8  | `W 0x00C` `STEP` while not `RUNNING` | E4 — nothing to advance            |

What is *not* in this table matters too. `W 0x00C` `ONESHOT` is legal in every
state, including while running: it is a mode bit, and mode bits are written, not
commanded. `W 0x010` clearing `DONE` when it is already `0` is legal and does
nothing. Writing a reserved bit is ignored, not an error.

## 10. What this buys

Every sequence and every failure row above is a test case, and — following
v0.4a's discipline — it exists **before the decode does**:

```text
docs/Observer_Register_Map.md  ->  validation/observer_periph.py  ->  rtl/bcmc_obs_wb.v
```

`validation/observer_periph.py` is the reference model of *this window*: a
program that holds `OBS_*` state and implements T1-T4 and F1-F8, and that
contains no BCMC mathematics and no traversal of its own — the engine's rules
live in `observer_hw.py`, and the window's model defers to them exactly as the
window's RTL defers to `bcmc_observer.v`. Executing this document against that
model, before any decode exists, is what turns the prose above into a
specification rather than a description; the last two times it was done
(`docs/Transaction_Sequences.md`, and
`docs/Hardware_Observer_Architecture.md` itself) it found defects in the
document, which is the point.

The claim this document is entitled to make, and no more, is:

> **The window introduces no new traversal semantics.** It is verified by
> replaying recorded sequences — bus tests, not mathematics — and the engine's
> own verification (cycle-by-cycle against `observer_hw.py`) is unaffected by
> whether this window exists at all.

That is why the engine could be frozen and verified before this document was
written, and why nothing in sections 3 to 6 of the architecture document changed
in order to write it.

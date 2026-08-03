# BCMC Transaction Sequences

`docs/Hardware_Architecture.md` gives the Core a cycle-accurate timing diagram.
This file is the same thing one level up: the **software analogue of that
diagram**, written before `rtl/bcmc_wb.v` exists.

It contains no RTL and no code. It contains only the canonical bus transactions
a driver performs, in order, and what each one must return.

```text
docs/Hardware_Architecture.md   ->  what the Core's wires do, cycle by cycle
docs/Register_Map.md            ->  what each address means
docs/Transaction_Sequences.md   ->  what software actually does, in order
```

`docs/Register_Map.md` remains the authority. Where this file appears to say
something new about an address or a state, that is a bug in this file. What this
file adds is **sequence**: which access follows which, and which orderings are
mistakes.

---

## Notation

```text
W  addr  value      write; must terminate in ack
R  addr  -> value   read;  must terminate in ack and return value
E  addr             access must terminate in err, with no side effect
*  ...              repeat
```

Addresses are the byte offsets of `docs/Register_Map.md`. All accesses are full
32-bit words with `sel = 4'b1111`; no other access exists.

The reference build is assumed throughout: `MAX_C = 64`, `VAL_W = 16`,
`IDX_W = 16`, hence two `COLUMN` words.

---

## S1 — Probe

Establish that the peripheral is present and discover its geometry. A driver
does this once, and it does it **before** it assumes anything.

```text
R 0x000  -> 0x42434D43        ID, the ASCII bytes "BCMC"
R 0x004  -> 0x00000400        VERSION 0.4.0
R 0x008  -> 0x10100040        CAPS: IDX_W=16, VAL_W=16, MAX_C=64
```

`MAX_C` comes from `CAPS`, never from a `#define`. Every later sequence that
mentions `MAX_C` means the value read here.

---

## S2 — Load a BCMC instance, polling

The central sequence. Everything else presupposes it.

```text
W 0x014  N                    row length
W 0x018  C                    number of rows, C <= MAX_C
W 0x400  weight[0]            WEIGHT[0]
W 0x404  weight[1]
*        ...                  WEIGHT[i] at 0x400 + 4i, for i < C
W 0x00C  0x00000001           CTRL.START
R 0x010  -> BUSY=1            optional; the transform is running
*        ...                  poll
R 0x010  -> BUSY=0 VALID=1    the context now holds the offsets
```

Steps 1–3 may be interleaved in any order: `N`, `C` and the weights are sampled
by the transform when `START` is written, not when they are written.

The poll terminates on **`VALID = 1`**, not on `BUSY = 0`. After reset `BUSY` is
already `0` and no matrix exists, so a driver that waits for `BUSY == 0` alone
will happily proceed to read a matrix that was never built. `VALID` is the bit
that says a matrix exists.

There is no `DONE` bit to poll, and there will not be one. `done` in the Prefix
Stream Interface is a one-cycle pulse, and a pulse is invisible to software that
polls. Completion is latched twice instead, with different lifetimes: `VALID`
until the context is disturbed, `IRQ` until acknowledged.

---

## S3 — Load a BCMC instance, interrupt-driven

The same transform. Only the waiting differs.

```text
W 0x00C  0x00000002           CTRL.IRQ_EN = 1, once, at init
...
W 0x014  N
W 0x018  C
W 0x400+4i  weight[i]         * for i < C
W 0x00C  0x00000003           START and IRQ_EN together
                              -- interrupt --
R 0x010  -> IRQ=1 VALID=1     in the handler
W 0x010  0x00000004           STATUS.IRQ = 1 to clear it
R 0x010  -> IRQ=0 VALID=1
```

`IRQ_EN` must be rewritten alongside `START`, because `CTRL` is one register and
a write sets both fields. Writing `0x00000001` would start the transform _and_
disable the interrupt.

`IRQ` latches whether or not `IRQ_EN` is set: the enable gates the output pin,
never the latch. So a polling driver and an interrupt-driven driver see exactly
the same `STATUS`, and a driver may switch between them at any time.

**Acknowledge before the next `START`.** `IRQ` is a level, not a count. Two
transforms with no acknowledgement between them leave one `IRQ = 1`, and nothing
in the register map can tell you it was two.

---

## S4 — Read one cell

```text
W 0x01C  i                    CELL_ROW
W 0x020  j                    CELL_COL
R 0x024  -> bit 0 = M(i, j)
```

Answered in the same bus cycle as the read. The Evaluator is combinational, so
there is nothing to wait for and no "start query, poll ready" step — that would
be inventing latency the mathematics does not have.

Both index writes are permitted while `BUSY`, and neither clears `VALID`: they
are part of the _query_, not part of the context. Only the read of `0x024` needs
`VALID = 1`.

`i >= C` is not an error. Rows at or above `C` are inactive lanes, evaluated with
weight `0`, and a cell of weight `0` answers `0` for every column — so the read
returns `0`. It does so regardless of what `WEIGHT[i]` happens to contain.

---

## S5 — Read one column

```text
W 0x020  j                    CELL_COL
R 0x028  -> rows 0..31        COLUMN[0]
R 0x02C  -> rows 32..63       COLUMN[1]
```

Bit `b` of word `k` is row `32k + b`, so row 0 is bit 0 of `COLUMN[0]`. Bits for
rows at or above `C` read `0`.

`CELL_ROW` takes no part in a column read. It is not written here, and its value
is irrelevant.

There is no `QUERY_COL` register. The column index is `CELL_COL` at `0x020`, the
same register `CELL` uses, because it is the same `j` in the same function. The
name says `CELL_*` rather than `QUERY_*` because there is no row window in this
map, and a name implying a whole row could be requested would be a lie.

This is the sequence an allocator runs in a loop: a column is a scheduling slot,
and the Balance Theorem is a statement about its popcount.

---

## S6 — Read the offsets back

```text
R 0x800      -> offset[0]     always 0
R 0x804      -> offset[1]
*            ...              OFFSET[i] at 0x800 + 4i
R 0x800+4i   -> 0             for C <= i < MAX_C
```

The whole window is defined after a completed transform: `OFFSET[0 .. C-1]` from
the Core, and `OFFSET[C .. MAX_C-1]` cleared, so a smaller `C` can never expose
a stale offset left by a larger one.

Diagnostic only. A driver does not need the offsets to use the matrix, and it
cannot write them: the window is read-only so that `offset[0] = 0` stays a
structural fact rather than an accident of initialisation.

---

## S7 — Re-run with a modified weight vector

The common case in a control loop, and the one with a trap in it.

```text
W 0x400+4i  weight'[i]        VALID drops to 0 here
E 0x028                       COLUMN[0]: refused, !VALID
W 0x00C  0x00000001           START
*        ...                  poll
R 0x010  -> VALID=1
R 0x028  -> rows 0..31        now permitted
```

`N` and `C` persist, so only the changed weights and `START` are needed.

The `err` in the middle is the point of the sequence. Writing any weight clears
`VALID`, because the offsets in the context now describe a weight vector that is
no longer programmed. Every read of `CELL`, `COLUMN[k]` and `OFFSET[i]` is
refused until the transform has been re-run. A peripheral that answered those
reads with the old matrix would be answering a question about a matrix that no
longer exists.

---

## S8 — The empty instance

`C = 0` is legal, and a driver should not special-case it.

This sequence continues from S2, so `N` is already programmed:

```text
W 0x018  0                    C = 0
W 0x00C  0x00000001           START
*        ...                  poll; there are no weights to stream
R 0x010  -> BUSY=0 VALID=1
R 0x024  -> 0                 every cell
R 0x028  -> 0                 every column word
```

No offsets are written, the transform completes without a single weight being
consumed, and every evaluation answers `0` — as a consequence of the inactive
lane rule, not as a special case anywhere.

`C = 0` is the empty instance; `N = 0` is not an instance at all. `N >= 1` is a
precondition of the theorem, so starting a transform with `C = 0` before `N` has
ever been written is undefined — see the note below the failure table. The bus
still says `ack`, and that is the whole of the peripheral's obligation.

---

## Sequences that must fail

These are as much a part of the specification as the ones above, because "the
access is refused" is a promise the driver relies on. Each must terminate in
`err` and leave no trace: no register changed, no flag moved, no transform
started.

| #   | Sequence                               | Why             |
| --- | -------------------------------------- | --------------- |
| F1  | `R 0x048`                              | unmapped (E1)   |
| F2  | `R 0x400 + 4*MAX_C`                    | `i >= MAX_C`    |
| F3  | `W 0x000` — write `ID`                 | read-only (E2)  |
| F4  | `W 0x800` — write `OFFSET[0]`          | read-only (E2)  |
| F5  | `R 0x00C` — read `CTRL.START`          | see below       |
| F6  | any access with `sel != 4'b1111`       | not a word (E3) |
| F7  | `W 0x014` (`N`) while `BUSY`           | E4              |
| F8  | `W 0x00C` `START` while `BUSY`         | E4              |
| F9  | `R 0x024` (`CELL`) while `BUSY`        | E4              |
| F10 | `R 0x024` (`CELL`) while `!VALID`      | E4              |
| F11 | `R 0x800` (`OFFSET[0]`) while `!VALID` | E4              |

F5 needs care and is a question for the wrapper, not for this document to
invent: `CTRL` is `RW` in the map, and `IRQ_EN` is genuinely readable, so a read
of `0x00C` returns `IRQ_EN` with `START` reading `0`. It is **not** an error.
The row is listed here only because "`START` always reads `0`" is easy to
mistake for "`START` is write-only".

What is _not_ in this table matters too. Violating a mathematical precondition —
`N = 0`, `WEIGHT[i] > N`, `CELL_COL >= N` — is **not** an error. Those are
preconditions of the theorem, the peripheral does not check them, and checking
them would be new mathematics inside a bus adapter. The result is undefined and
the bus still says `ack`.

---

## What this buys

Every sequence above is a test case, and it exists before the RTL does. Both
`validation/bcmc_periph.py` and `rtl/bcmc_wb.v` must reproduce all of them, so
the same list can be replayed against the Python model and against the wrapper
through a bus functional model, and the two must agree access for access.

That replay is not hypothetical. Every sequence and every failure row above has
been executed against `validation/bcmc_periph.py` — 817 assertions, including
that each `err` leaves the peripheral's state bit-for-bit unchanged — before a
line of `rtl/bcmc_wb.v` was written. It found one defect, and it was in this
file rather than in the model: S8 originally read `C = 0` from reset, which
leaves `N = 0` and so violates a precondition of the theorem. A specification
that has never been executed is just prose.

The bugs v0.4c can produce are not mathematical — they are sequencing, decode,
`sel`, `err` and interrupt-lifetime bugs — and that is precisely the class this
file is written to catch.

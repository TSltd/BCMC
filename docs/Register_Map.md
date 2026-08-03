# BCMC Peripheral Register Map

This document is **specification**, not documentation of an implementation.

`docs/BCMC.md` is the contract that both `validation/reference.py` and the RTL
must satisfy. This file plays exactly the same role one level up: it is the
contract that both `validation/bcmc_periph.py` and `rtl/bcmc_wb.v` must satisfy.
Where the two disagree, this document is right and the other one is a bug.

```text
docs/BCMC.md          ->  reference.py            ->  rtl/bcmc_{core,cell}.v
docs/Register_Map.md  ->  validation/bcmc_periph.py  ->  rtl/bcmc_wb.v
```

---

## What the peripheral is

The BCMC Core streams and stores nothing. The BCMC Evaluator is _given_
`weights[]` and `offsets[]` and holds nothing (see "Stateful Core, Stateless
Evaluator" in `docs/Hardware_Architecture.md`).

So the moment there is a bus, something has to hold the pair. That something is
this peripheral, and it is the first component in the project that **owns the
canonical prefix representation**:

```text
        WEIGHT window          software writes the weight vector
              │
              ▼
          BCMC Core             the prefix transform
              │
              ▼
        OFFSET window           the Core writes; software may only read
              │
              ▼
        BCMC Evaluator          the characteristic function
              │
              ▼
     CELL and COLUMN reads      answered combinationally, on demand
```

The RAM that v0.2 correctly refused to put inside the Core appears here, where
it belongs: not as a memory primitive but as the persistent BCMC **context**
that exists between software and hardware.

## What the peripheral is not

It is **not an observer**. Nothing in this register map decides which column to
look at next, when to look, or what a column means. Traversal is v0.5 and
belongs above this interface, exactly as `docs/Hardware_Architecture.md`
requires.

It contains **no matrix storage**. `CELL` and `COLUMN` are evaluated on demand
from `(weights[], offsets[])`. There is no framebuffer to go stale.

---

## Bus

Classic **Wishbone B4**, slave, registered feedback not used.

| Property      | Value                                                  |
| ------------- | ------------------------------------------------------ |
| Data width    | 32 bits                                                |
| Granularity   | 32 bits — word accesses only                           |
| Addressing    | byte addresses, all registers 32-bit aligned           |
| Bursts        | none                                                   |
| Pipelining    | none                                                   |
| Acknowledge   | single cycle: every accepted cycle terminates in `ack` |
| Errors        | single cycle `err`                                     |
| Region size   | 4 KiB (`0x000`–`0xFFF`)                                |
| Clock domains | one. There is no CDC in this peripheral.               |
| Endianness    | none — no sub-word access exists                       |

Every bus cycle terminates in exactly one of `ack` or `err`, in the cycle after
`stb & cyc`, and never in both, and never in neither.

---

## The error model

> Silent success hides bugs.

An access that is not meaningful returns **`err`**. It never returns `ack` with
a plausible-looking zero, and a write that returns `err` has **no side effect**
whatsoever: no register changes, no flag moves, no transform starts.

There are exactly four reasons for `err`, and they are all **structural** — they
concern what is addressable and when, never the mathematics:

| #   | Condition                                                                                     |
| --- | --------------------------------------------------------------------------------------------- |
| E1  | the address is not mapped (including `WEIGHT[i]`/`OFFSET[i]` with `i >= MAX_C`)               |
| E2  | the access type is wrong: a write to a read-only register, or a read of a write-only register |
| E3  | the access is not a full 32-bit word (`sel != 4'b1111`)                                       |
| E4  | the access is not meaningful in the current state (see below)                                 |

### E4 — state

| State       | Denied                                                                                       |
| ----------- | -------------------------------------------------------------------------------------------- |
| `BUSY = 1`  | writes to `N`, `C`, `WEIGHT[i]`, and `CTRL.START`; reads of `OFFSET[i]`, `CELL`, `COLUMN[k]` |
| `VALID = 0` | reads of `OFFSET[i]`, `CELL`, `COLUMN[k]`                                                    |

While `BUSY`, the context is mid-update, so it may not be perturbed and may not
be interrogated. While `!VALID`, no BCMC matrix exists, so asking for one of its
bits is a programming error and is refused. `ID`, `VERSION`, `CAPS`, `STATUS`,
`CTRL.IRQ_EN`, the `IRQ` acknowledgement and the two query-index registers are
always accessible.

### What is _not_ an error

The **mathematical preconditions are still preconditions**, exactly as in
`docs/Hardware_Architecture.md` rule 6 and `docs/Proof.md`. The peripheral does
not check them, because checking them would be new mathematics in a bus adapter:

```text
N >= 1
0 <= WEIGHT[i] <= N     for i < C
0 <= CELL_COL < N
```

Violating these is outside the specification, and the peripheral is permitted to
return anything at all. Note that `C <= MAX_C` is _not_ in that list: `MAX_C` is
a synthesis parameter, so it is geometry, and `C > MAX_C` is refused under E1.

`CELL_ROW >= C` is likewise **not** an error. Rows at or above `C` do not exist
in the matrix, so they are evaluated as **inactive lanes**: weight `0`, offset
`0`, and a cell of weight `0` answers `0` for every column. Such a row "is asked
a question whose answer is zero", which is exactly the doctrine already
implemented by `rtl/bcmc_column.v`, where `active = (ROW < C)` forces both
arguments to zero. `CELL` therefore reads `0`, and `COLUMN[k]` bits at or above
`C` read `0`.

Note the consequence: the inactive-lane rule depends on `C` alone, **not** on
what happens to be sitting in `WEIGHT[i]`. Software that leaves a stale non-zero
weight above `C` cannot make a non-existent row appear.

---

## Address map

`MAX_C`, `VAL_W` and `IDX_W` are synthesis parameters, reported at run time in
`CAPS`. The windows are sized for `MAX_C <= 256`; the reference build uses
`MAX_C = 64`.

| Offset          | Name        | Access | Width   | Meaning                                                 |
| --------------- | ----------- | ------ | ------- | ------------------------------------------------------- |
| `0x000`         | `ID`        | RO     | 32      | `0x42434D43`, the ASCII bytes `BCMC`                    |
| `0x004`         | `VERSION`   | RO     | 32      | major, minor, patch                                     |
| `0x008`         | `CAPS`      | RO     | 32      | `MAX_C`, `VAL_W`, `IDX_W`                               |
| `0x00C`         | `CTRL`      | RW     | 32      | `START`, `IRQ_EN`                                       |
| `0x010`         | `STATUS`    | RW     | 32      | `BUSY`, `VALID`, `IRQ`                                  |
| `0x014`         | `N`         | RW     | `VAL_W` | row length                                              |
| `0x018`         | `C`         | RW     | `IDX_W` | number of rows, `C <= MAX_C`                            |
| `0x01C`         | `CELL_ROW`  | RW     | `IDX_W` | row index `i` for the next `CELL` read                  |
| `0x020`         | `CELL_COL`  | RW     | `VAL_W` | column index `j` for `CELL` and `COLUMN`                |
| `0x024`         | `CELL`      | RO     | 1       | bit 0 = `M(CELL_ROW, CELL_COL)`                         |
| `0x028`–`0x047` | `COLUMN[k]` | RO     | 32      | column `CELL_COL`; bit `b` of word `k` is row `32k + b` |
| `0x048`–`0x3FF` | —           | —      | —       | unmapped (E1)                                           |
| `0x400`+`4i`    | `WEIGHT[i]` | RW     | `VAL_W` | the weight vector, `i < MAX_C`                          |
| `0x800`+`4i`    | `OFFSET[i]` | RO     | `VAL_W` | the canonical offsets, `i < MAX_C`                      |
| `0xC00`–`0xFFF` | —           | —      | —       | unmapped (E1)                                           |

`COLUMN[k]` exists for `k < ceil(MAX_C / 32)`; higher `k` within the reserved
span is unmapped (E1). With `MAX_C = 64` there are two words, `0x028` and
`0x02C`.

Reserved bits of any register read `0`, and writes to them are ignored. That is
conventional register behaviour and is deliberately _not_ an E-condition: it is
the field width doing its job, not an address decode failing.

---

## Register detail

### `ID` — `0x000`, RO

```text
31                                                                 0
+-------------------------------------------------------------------+
|                            0x42434D43                             |
+-------------------------------------------------------------------+
```

Constant. A driver that reads anything else is not talking to this peripheral.

### `VERSION` — `0x004`, RO

| Bits    | Field   | Meaning                                        |
| ------- | ------- | ---------------------------------------------- |
| `31:16` | `MAJOR` | incremented when this map changes incompatibly |
| `15:8`  | `MINOR` | incremented when registers are added           |
| `7:0`   | `PATCH` | implementation fixes; no map change            |

This document defines `MAJOR = 0`, `MINOR = 4`, `PATCH = 0`, i.e. `0x00000400`.

### `CAPS` — `0x008`, RO

| Bits    | Field   | Meaning                             |
| ------- | ------- | ----------------------------------- |
| `31:24` | `IDX_W` | bits in `C` and in a row index      |
| `23:16` | `VAL_W` | bits in `N`, a weight and an offset |
| `15:0`  | `MAX_C` | rows this instance can hold         |

A driver **discovers** geometry; it never assumes it. The reference build has
`IDX_W = 16`, `VAL_W = 16`, `MAX_C = 64`, hence

```text
CAPS = (16 << 24) | (16 << 16) | 64 = 0x10100040
```

### `CTRL` — `0x00C`, RW

| Bits   | Field    | Access | Meaning                                          |
| ------ | -------- | ------ | ------------------------------------------------ |
| `31:2` | —        | —      | reserved                                         |
| `1`    | `IRQ_EN` | RW     | when `1`, `IRQ` drives the interrupt output      |
| `0`    | `START`  | W1S    | writing `1` begins a transform; always reads `0` |

`START` is a command, not a state: it self-clears and has no readable value.
Writing `0` to it does nothing. Writing `1` while `BUSY` is E4.

### `STATUS` — `0x010`, RW

| Bits   | Field   | Access | Reset | Meaning                                                  |
| ------ | ------- | ------ | ----- | -------------------------------------------------------- |
| `31:3` | —       | —      | `0`   | reserved                                                 |
| `2`    | `IRQ`   | RW1C   | `0`   | a transform completed; write `1` to clear                |
| `1`    | `VALID` | RO     | `0`   | the context holds the offsets for the programmed weights |
| `0`    | `BUSY`  | RO     | `0`   | a transform is in progress                               |

`STATUS` is the only register with mixed access, and the reason is that `IRQ`
must be both observable and acknowledgeable. Writing `1` to a reserved bit or to
`VALID`/`BUSY` is ignored, not an error.

There is deliberately **no `DONE` bit**. `done` in the Prefix Stream Interface is
a one-cycle pulse, and a pulse is useless to software that polls. Completion is
latched in two places instead, with different lifetimes: `VALID` says a matrix
exists and stays true until the context is disturbed; `IRQ` says a completion
happened and stays true until acknowledged.

### `N` — `0x014`, RW; `C` — `0x018`, RW

Sampled by the transform when `START` is written, not when they are written.
Writing either clears `VALID`. `C = 0` is legal (Prefix Stream Interface rule 5):
the transform completes immediately, no offsets are written, `VALID` becomes `1`,
and every `CELL`/`COLUMN` read answers `0`.

### `CELL_ROW` — `0x01C`, RW; `CELL_COL` — `0x020`, RW

Query indices. They are part of the **query**, not part of the context, so
writing them does _not_ clear `VALID` and is permitted while `BUSY`.

They are named `CELL_*` rather than `QUERY_*` on purpose. There is no row
register and no row window in this map, so a name suggesting that a whole row can
be requested would be a lie. `CELL_ROW` selects the row index _for the following
`CELL` read_ and nothing else.

### `CELL` — `0x024`, RO

| Bits   | Field | Meaning                 |
| ------ | ----- | ----------------------- |
| `31:1` | —     | reserved, reads `0`     |
| `0`    | `BIT` | `M(CELL_ROW, CELL_COL)` |

### `COLUMN[k]` — `0x028`+`4k`, RO

Column `CELL_COL` of the matrix, packed by row, row 0 in bit 0 of word 0:

```text
COLUMN[k] bit b  =  M(32k + b, CELL_COL)
```

Bits for rows at or above `C`, and bits at or above `MAX_C`, read `0`.

`CELL_ROW` is not involved in a `COLUMN` read.

### `WEIGHT[i]` — `0x400`+`4i`, RW

The weight vector. Writing any `WEIGHT[i]` clears `VALID`, because the offsets
now describe weights that are no longer programmed. Weights are readable so that
a driver can verify what it wrote.

`WEIGHT[i]` for `i >= C` is writable and readable, but takes no part in a
transform and no part in an evaluation: rows at or above `C` are inactive lanes
and are evaluated with weight `0` whatever the window contains. Writing weights
above `C` is therefore harmless, and it is also pointless.

### `OFFSET[i]` — `0x800`+`4i`, RO

The canonical prefix representation's second half, written by the Core during a
transform.

A transform writes `OFFSET[0 .. C-1]` and clears `OFFSET[C .. MAX_C-1]` to `0`,
so the whole window is defined after every completed transform and never exposes
a stale offset belonging to some earlier, larger `C`.

**This window is read-only, and that is doctrine rather than convenience.** If
software could write an offset it could seed a transform with an arbitrary
value, and

```text
offset[0] = 0
```

would stop being structural. The result would be a matrix that is not BCMC. This
is precisely the misuse for which `P_C mod N` is withheld from the Core (see
"`P_C mod N` is not an output"). Read-only here closes the same door at the bus.

---

## Programming sequence

The software interface is deliberately short, and it contains no protocol:

```text
1.  read  ID        == 0x42434D43
2.  read  CAPS      -> MAX_C, VAL_W, IDX_W
3.  write N
4.  write C                     (C <= MAX_C)
5.  write WEIGHT[0 .. C-1]
6.  write CTRL.START = 1
7.  wait until STATUS.VALID == 1  (or take the interrupt)
8.  read  CELL / COLUMN[k] / OFFSET[i] freely
```

Step 7 waits on `VALID`, not on `BUSY`. `BUSY == 0` is already true after reset,
when no matrix exists, so it is not a completion condition — it is the absence of
one. `VALID` is the bit that says a matrix exists, and it is also the bit the
access rules above use to decide whether step 8 is answered or refused.

Steps 3–5 may be in any order. A driver **never** sees `weight_valid`,
`offset_valid`, `busy` handshaking or a cycle count: the wrapper owns the Prefix
Stream Interface, exactly as the Core owns the prefix recursion.

```text
BCMC Core
    │   Prefix Stream Interface
    ▼
Wishbone wrapper
    │   this register map
    ▼
CPU
```

Re-running with a modified weight vector means writing the changed `WEIGHT[i]`
and `START` again. `N` and `C` persist.

`docs/Transaction_Sequences.md` expands this sequence, and seven others, into the
individual bus accesses a driver performs, together with the orderings that must
be refused. This file says what each address means; that one says which access
follows which. Where they disagree, this file wins.

### Evaluation has no handshake

`CELL` and `COLUMN[k]` are answered in the same bus cycle as the read, because
the Evaluator is combinational: `M(i,j)` depends on nothing but its own
arguments, so there is nothing to wait for. There is no "start query, poll
ready" sequence anywhere in this map, and there never will be — that would be
inventing latency that the mathematics does not have.

---

## State

Three bits of externally visible state, and every transition:

| From     | Event                             | To                                                                           |
| -------- | --------------------------------- | ---------------------------------------------------------------------------- |
| reset    | —                                 | `BUSY=0 VALID=0 IRQ=0`, `WEIGHT[]=0`, `OFFSET[]=0`, `N`, `C`, `CELL_*` = `0` |
| `BUSY=0` | write `CTRL.START=1`              | `BUSY=1`, `VALID=0`                                                          |
| `BUSY=1` | transform completes               | `BUSY=0`, `VALID=1`, `IRQ=1`                                                 |
| any      | write `N`, `C` or any `WEIGHT[i]` | `VALID=0`                                                                    |
| `IRQ=1`  | write `STATUS.IRQ=1`              | `IRQ=0`                                                                      |

The interrupt output is `IRQ & IRQ_EN`. It is level-sensitive, so clearing `IRQ`
deasserts it; `IRQ_EN` gates the pin only and never gates the latching of `IRQ`,
so polling drivers and interrupt-driven drivers observe the same `STATUS`.

Starting a transform clears `VALID` before setting `BUSY`: during a transform the
old offsets are being overwritten, so there is no moment at which `VALID` is true
and the context is inconsistent.

---

## Things deliberately not in this map

**No matrix RAM, and no row window.** A column is bounded by `MAX_C`, a
synthesis parameter, so it fits in a fixed set of registers. A row is `N` bits
and `N` is a _runtime_ value up to `2^VAL_W - 1`, so a row window would be either
unbounded or a lie about how much of the row it returned. Cell plus column is the
honest addressable set, and the cell is fully general in any case: any row can be
read one bit at a time. This is the same argument as "A Note on Width" in
`docs/Hardware_Architecture.md`.

**No writable offsets.** See `OFFSET[i]` above.

**No `P_C mod N`.** It belongs to no row and evaluates no matrix element, so it
is not in the canonical prefix representation and therefore not in the register
map either.

**No traversal, no timers, no GPIO, no next-column register.** Those are
observers. They are v0.5, they live above this interface, and they use nothing
but the reads defined here.

**No `bcmc_evaluator.v` behind this map.** The projection multiplexer — the logic
that decides whether a read wants a cell or a column — is address decode. It is
part of the bus adapter, because that is the only place a _bus_ is mentioned. The
Evaluator remains an architectural concept, as `dev/ROADMAP.md` requires.

**No burst read of a whole matrix.** That is a DMA observer, and it is listed
under Future in the README for a reason: it needs an address generator, and an
address generator is a traversal order.

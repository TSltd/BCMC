# BCMC Hardware Architecture

## Overview

The mathematical definition of Balanced Cyclic Matrix Construction (BCMC) separates naturally into two independent operations:

1. **Construction** of the canonical BCMC representation.
2. **Evaluation** of that representation.

This separation mirrors the mathematical formulation developed in the BCMC proof. The construction stage computes the canonical prefix offsets from a weight vector, while the evaluation stage computes elements of the binary matrix on demand using the characteristic function.

The resulting architecture consists of two independent IP blocks:

```text
                 BCMC Core
          (Canonical Construction)
                     │
     weights[]       │       offsets[]
────────────────────►│──────────────────┐
                     │                  │
                     ▼                  ▼
           Canonical BCMC Representation
                     │
                     ▼
              BCMC Evaluator
        (Matrix Characteristic Function)
                     │
        ┌────────────┼────────────┐
        ▼            ▼            ▼
      Cell         Row         Column
      Query       Query        Query
```

The BCMC Core performs the mathematical transform.

The BCMC Evaluator interprets the resulting representation.

Neither block imposes any traversal order on the matrix.

---

# Design Philosophy

The BCMC algorithm defines only a binary matrix.

It does **not** define

- scheduling,
- timing,
- streaming order,
- column traversal,
- randomisation,
- application semantics.

Those responsibilities belong to downstream consumers.

Consequently, the BCMC hardware architecture preserves the same separation:

```text
Weight Vector
      │
      ▼
 BCMC Construction
      │
      ▼
Canonical Representation
      │
      ▼
Matrix Evaluation
      │
      ▼
Application-specific Observation
```

This architecture ensures that the hardware remains a faithful implementation of the mathematical definition.

---

# The Core is a Transform, not an Accelerator

This distinction sounds like semantics. It is not: it determines the entire design.

The BCMC Core's sole contract is

```text
weights[]
    │
    ▼
offsets[]
```

Consequently the Core has

- no GPIO,
- no timers,
- no observers,
- no matrix output,
- no traversal,
- no bus interface,

and no state beyond the prefix accumulator and the row index.

Everything else belongs to the Evaluator or to a downstream observer.

## `P_C mod N` is not an output

The Core does **not** expose the final accumulator value `P_C mod N`.

That quantity

- belongs to no row,
- evaluates no matrix element,
- forms no part of the prefix representation,
- appears nowhere in the proof.

It exists only as an artefact of having processed every row.

In particular, BCMC transforms are **not** chainable: every BCMC transform begins at

```text
offset[0] = 0.
```

Seeding a transform with a previous final accumulator value would construct a different matrix, and would therefore not be BCMC. Exposing the value would invite exactly that misuse, so it is not exposed.

---

# BCMC Core

## Purpose

The BCMC Core computes the canonical representation from the input weight vector.

The Core does **not**

- construct the binary matrix,
- generate rows,
- generate columns,
- determine traversal order.

Its sole responsibility is computing the prefix offsets.

---

## Inputs

```text
N          Row length

C          Number of rows

weight[0]
...
weight[C−1]
```

Subject to

```text
0 ≤ weight[i] ≤ N
```

---

## Outputs

```text
offset[0]
...
offset[C−1]
```

where

```text
offset[0] = 0

offset[i+1] =
(offset[i] + weight[i]) mod N
```

The pair

```text
(weight[], offset[])
```

forms the canonical prefix representation used by the hardware.

---

## Internal Operation

The Core performs a single prefix accumulation.

```text
offset = 0

for each row

    output offset

    offset =
        (offset + weight) mod N
```

The computation is complete after one pass through the weight vector.

---

## Emit before Update

The ordering of the two operations is part of the specification.

Let `offset_q` denote the internal accumulator, holding `oᵢ`. When a weight arrives, the Core

1. **first** presents `offset_q` as the output offset,
2. **then** updates

```text
offset_q ← (offset_q + weight) mod N.
```

Emitting before updating is what makes

```text
offset[0] = 0
```

a structural property of the datapath rather than a special case in the control logic.

---

## The Reduction is not a Division

The specification guarantees

```text
0 ≤ wᵢ ≤ N,
```

hence

```text
offset_q + weight < 2N,
```

so the reduction modulo `N` is a **single comparison and a single subtraction**:

```text
sum = offset_q + weight

offset_q ← (sum ≥ N) ? sum − N : sum
```

No divider is ever required.

This is the hypothesis `wᵢ ≤ N` of Lemma 2 (see `docs/Proof.md`) appearing directly as hardware.

---

# BCMC Prefix Stream Interface

The Core presents a single named interface, the **BCMC Prefix Stream Interface**.

The interface is named because the BCMC Evaluator consumes exactly the same

```text
(weight, offset)
```

stream. Later documentation refers to the interface by name rather than re-enumerating its signals.

| Group         | Signal                       | Direction | Meaning                                                    |
| ------------- | ---------------------------- | --------- | ---------------------------------------------------------- |
| Framing       | `start`                      | in        | begin a transform; sample `N` and `C`                      |
|               | `busy`                       | out       | a transform is in progress                                 |
|               | `done`                       | out       | one-cycle pulse; the entire offset stream has been emitted |
| Weight stream | `weight_in`, `weight_valid`  | in        | one accepted weight per asserted cycle                     |
| Offset stream | `offset_out`, `offset_valid` | out       | one emitted offset per asserted cycle                      |
| Parameters    | `N`, `C`                     | in        | sampled at `start`, held for the duration of the transform |

All signals are synchronous to `clk`. All outputs are registered.

---

## Timing

`N = 8`, `C = 3`, weights `6, 3, 5`, offsets `0, 6, 1`.

```text
cycle        0     1     2     3     4     5     6
          ___   ___   ___   ___   ___   ___   ___
clk      _|   |_|   |_|   |_|   |_|   |_|   |_|   |_

start     ▁▁███▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁

busy      ▁▁▁▁▁▁▁███████████████████████▁▁▁▁▁▁▁▁▁▁

weight_valid ▁▁▁▁▁███████████████████▁▁▁▁▁▁▁▁▁▁▁▁▁

weight_in   ---   ---    6     3     5    ---   ---

offset_valid ▁▁▁▁▁▁▁▁▁▁▁▁███████████████████▁▁▁▁▁▁

offset_out  ---   ---   ---    0     6     1    ---

done      ▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁███▁▁▁▁▁
```

The authoritative cycle table:

| cycle | `start` | `busy` | `weight_valid` | `weight_in` | `offset_valid` | `offset_out` | `done` |
| ----- | ------- | ------ | -------------- | ----------- | -------------- | ------------ | ------ |
| 0     | 1       | 0      | 0              | –           | 0              | –            | 0      |
| 1     | 0       | 1      | 1              | 6           | 0              | –            | 0      |
| 2     | 0       | 1      | 1              | 3           | 1              | **0**        | 0      |
| 3     | 0       | 1      | 1              | 5           | 1              | **6**        | 0      |
| 4     | 0       | 1      | 0              | –           | 1              | **1**        | 0      |
| 5     | 0       | 0      | 0              | –           | 0              | –            | 1      |
| 6     | 0       | 0      | 0              | –           | 0              | –            | 0      |

---

## Protocol Rules

The diagram fixes the following, which therefore never require re-deciding.

1. `offset_valid` is asserted exactly one cycle after each accepted `weight_valid`. There is no combinational path from `weight_in` to `offset_out`.
2. `done` is a one-cycle pulse asserted **after** the final offset has been delivered. `done` therefore means unambiguously "the entire offset stream has been emitted". `busy` falls in the same cycle.
3. `weight_valid` may be asserted in any pattern, with arbitrary idle gaps. The **sequence of accepted weights alone** determines the output sequence; idle cycles affect timing only, never values.
4. `weight_valid` is illegal while `busy` is low, and no more than `C` weights may be presented during a transform.
5. `C = 0` is legal: `start` raises `busy` for one cycle, no offsets are emitted, and `done` pulses.
6. `N ≥ 1` and `0 ≤ weight_in ≤ N` are preconditions, not behaviours. Violating them is outside the specification (see the necessity of `wᵢ ≤ N` in `docs/Proof.md`).

These rules describe the interface between the Core and **whatever presents weights to it** — they are not a software interface. Software never sees `weight_valid`, `offset_valid`, `busy` or `done` in this form. A bus wrapper owns the handshake and exposes it as a register write that starts a transform and a status bit that reports completion. That translation is specified in `docs/Register_Map.md`, which is the contract for the programmer's model in the same way this section is the contract for the Core's pins.

---

# Canonical Prefix Representation

The BCMC Core computes the canonical prefix representation

```text
weights[]

offsets[]
```

which is a **lossless representation of the canonical BCMC matrix** and is the internal representation used by the hardware architecture.

The canonical mathematical object remains the matrix `M`. The pair `(weights[], offsets[])` is the hardware's compressed encoding of that object — an implementation representation, not a redefinition of the mathematics.

No binary matrix is stored.

Every valid BCMC matrix can be reconstructed uniquely from this representation.

Conversely, every canonical prefix representation corresponds to exactly one BCMC matrix.

The representation is therefore lossless.

Being lossless is what makes the representation worth **storing**, and the Core deliberately does not store it: it streams weights in and offsets out, holding only the accumulator. Storage is the business of whichever component the representation persists in. The first such component is the memory-mapped peripheral, where the two halves of the pair appear as the `WEIGHT` and `OFFSET` windows of `docs/Register_Map.md` — software writes one half, the Core produces the other, and the Evaluator reads both. The asymmetry of those windows, one writable and one not, is that sentence expressed as an address map.

---

# Stateful Core, Stateless Evaluator

The two halves of BCMC meet at the canonical prefix representation.

```text
                            Stateful

        Weights  ──────────────────────────────►  Offsets

                            BCMC Core

                                 │
                                 ▼

                  Canonical Prefix Representation

                                 │
                                 ▼

        Offsets  ──────────────────────────────►  Matrix Bits

                          BCMC Evaluator

                            Stateless
```

They are complementary halves, and their differences are not arbitrary — every
one of them is inherited from the mathematics:

```text
                BCMC Core                  BCMC Evaluator
────────────────────────────────────────────────────────────────
Mathematics     Prefix recursion           Characteristic function
State           Prefix accumulator         None
Clock           Required                   None
Operation       Prefix transform           Point evaluation
Reduction       Conditional subtract       Conditional add
Latency         O(C)                       O(1)
```

The Core is sequential because a prefix sum is **recursive**: `offset[i+1]`
cannot be known without `offset[i]`.

The Evaluator is combinational because the characteristic function is
**pointwise**: `M(i,j)` depends on nothing but `weight[i]`, `offset[i]`, `j`
and `N`.

Neither property was chosen. Both were read off the definition.

---

# BCMC Evaluator

## Purpose

The BCMC Evaluator implements the characteristic function defining the BCMC matrix.

Given

```text
weights[]

offsets[]
```

the Evaluator computes arbitrary elements of the matrix on demand.

Unlike the Core, the Evaluator performs no prefix computations.

## The Evaluator is a Concept, not a Module

"BCMC Evaluator" names an **architectural role**. It does not imply a Verilog module, and there is deliberately no `bcmc_evaluator.v`.

The Core happens to map onto exactly one module, `bcmc_core.v`, because a prefix recursion is one indivisible sequential process. The Evaluator maps onto

```text
bcmc_cell.v      the characteristic function itself
bcmc_row.v       N cells, one weight/offset pair
bcmc_column.v    C cells, one column index
```

and a wrapper multiplexing between them would add no mathematics, own no state, and serve no consumer that currently exists. It will be introduced if and when something genuinely requires it.

When a consumer does require a choice of projection, the choice belongs to that consumer, not to a new Evaluator module. A memory-mapped interface is the first such consumer: `docs/Register_Map.md` gives it a `CELL` register for the cell projection and a `COLUMN` window for the column projection, and the address a program reads is what selects between them. The multiplexer is address decoding in a bus adapter — it is not mathematics, which is precisely why it does not live here.

Note also what the Evaluator does **not** contain: storage. It is given `weights[]` and `offsets[]`; it does not hold them. Whoever owns the canonical prefix representation presents it. This is what keeps the Evaluator stateless in fact and not merely in description.

---

# Characteristic Function

For row `i` and column `j`

```text
M(i,j) = 1

iff

((j − offset[i]) mod N) < weight[i]
```

Otherwise

```text
M(i,j) = 0
```

This equation completely defines the binary matrix.

---

## The Wrap is not a Division

The Core's reduction was cheap because `0 ≤ wᵢ ≤ N`. The Evaluator's is cheap for the mirror-image reason.

The offsets produced by the Core satisfy `0 ≤ offset[i] < N`, and a column index satisfies `0 ≤ j < N`, hence

```text
−N < j − offset[i] < N,
```

so the reduction modulo `N` is a **single comparison and a single addition**:

```text
diff = j − offset[i]

delta = (j < offset[i]) ? diff + N : diff
```

after which

```text
M(i,j) = (delta < weight[i]).
```

No divider is ever required here either.

The symmetry is exact, and it is not a coincidence — both facts are consequences of the same hypothesis of Lemma 2:

```text
Core        offset + weight     →   conditional subtract
Evaluator   column − offset     →   conditional add
```

The Core wraps _forwards_ and so may overshoot `N`; the Evaluator wraps _backwards_ and so may undershoot `0`. One subtracts `N`, the other adds it.

---

# Architectural Decomposition

> The BCMC Evaluator introduces no new mathematics.
>
> Every operation performed by the Evaluator is an evaluation of the BCMC characteristic function.
>
> Cell, row and column queries differ only in which argument or arguments are held constant.

This is the load-bearing statement of the entire Evaluator design. It has a direct consequence for the hardware: **only the cell is a design.** Everything else is replication.

---

# Projections of the Characteristic Function

```text
Matrix
   │
   ▼
 Rows
   │
   ▼
Columns
   │
   ▼
 Cells
```

These are not four operations. They are four **projections** of the same function `M(i,j)`, distinguished only by which arguments are bound:

| Projection | Bound    | Free     | Result       |
| ---------- | -------- | -------- | ------------ |
| Cell       | `i`, `j` | –        | 1 bit        |
| Row        | `i`      | `j`      | `N` bits     |
| Column     | `j`      | `i`      | `C` bits     |
| Matrix     | –        | `i`, `j` | `C × N` bits |

The Evaluator has no opinion about which projection an application should use. Hardware implementations should choose whichever projection best matches the surrounding system: a slot-driven controller naturally wants **columns**, one bit per row per time slot; a per-channel analysis might genuinely want **rows**; an addressable interface wants **cells**. All three are the same mathematics, evaluated with the arguments grouped differently.

## Bit Ordering

Bit ordering is specified, never left to convention.

For a row query, the `N`-bit result is indexed by column:

```text
bit[0]   = column 0
bit[1]   = column 1
...
bit[N−1] = column N−1
```

For a column query, the `C`-bit result is indexed by row:

```text
bit[0]   = row 0
bit[1]   = row 1
...
bit[C−1] = row C−1
```

## A Note on Width

Because a row is `N` bits wide and `N` is a runtime value, any fully parallel row module must be built to a compile-time maximum. This is a property of the projection, not of BCMC: a combinational row for `N = 65535` would be 65,535 cells, which no one should build.

Parallel row and column modules therefore exist chiefly to make the replication claim **verifiable**. Large implementations should instantiate the cell directly, as many times as their traversal strategy actually requires.

---

# Architectural Separation

The BCMC Core constructs the mathematical object.

The BCMC Evaluator evaluates the mathematical object.

Neither component defines how the resulting matrix is consumed.

Consequently, sequential traversal, pseudorandom traversal, Gray-code traversal, DMA access, software access or hardware streaming are all external to the BCMC architecture.

---

# Downstream Observer Layer

Applications interact only with the Evaluator.

Examples include

- software drivers,
- DMA engines,
- timer peripherals,
- GPIO controllers,
- hardware schedulers,
- FPGA logic,
- custom accelerators.

These consumers determine

- which rows are queried,
- which columns are queried,
- the order of evaluation,
- timing,
- repetition.

This layer is intentionally excluded from the BCMC specification.

---

# Future Observer Implementations

Possible observer implementations include

- sequential column iterator,
- seeded pseudorandom permutation,
- Gray-code traversal,
- arbitrary address generator,
- DMA streaming engine,
- interrupt-driven software interface,
- memory-mapped matrix access.

Each observer interprets the same canonical BCMC representation.

Since observation order is external to BCMC, all preserve the mathematical properties proven for the construction.

---

# Advantages of the Two-Stage Architecture

Separating construction from evaluation provides several benefits.

## Mathematical Fidelity

The hardware mirrors the mathematical definition exactly.

---

## Minimal Canonical State

Only

```text
weights[]

offsets[]
```

must be stored.

The explicit binary matrix is never required.

---

## Multiple Independent Observers

Several observers may simultaneously evaluate the same canonical representation without interfering with one another.

---

## Hardware Reuse

Alternative phase-generation algorithms may reuse the BCMC Evaluator provided they produce compatible

```text
(weight, offset)
```

representations.

Similarly, alternative observer implementations may operate on the BCMC Core without modification.

---

# Summary

The BCMC hardware architecture separates construction from evaluation.

The BCMC Core computes the canonical prefix-offset representation from a weight vector.

The BCMC Evaluator computes the characteristic function defining the binary matrix.

All traversal, scheduling and application-specific behaviour are delegated to downstream observer layers.

This architecture faithfully implements the mathematical definition of BCMC while remaining independent of any particular interpretation or application.

# Guiding Principle

The BCMC Core is not a "matrix generator" — it is a canonical transform, while the Evaluator is a realization of the characteristic function. That makes the architecture almost identical to the proof:

1. Compute the prefix partition (Core).
2. Evaluate the characteristic function (Evaluator).
3. Interpret the resulting matrix (Observer).

This preserves the correspondence between the mathematics and the hardware.

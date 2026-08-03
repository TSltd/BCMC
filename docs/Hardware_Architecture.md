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

# Evaluation Operations

The Evaluator supports three fundamental operations.

## Cell Evaluation

Input

```text
row

column
```

Output

```text
single bit
```

representing

```text
M(row,column)
```

---

## Row Evaluation

Input

```text
row
```

Output

```text
N-bit vector
```

containing the complete row.

---

## Column Evaluation

Input

```text
column
```

Output

```text
C-bit vector
```

containing the complete column.

This operation evaluates the characteristic function independently for every row.

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

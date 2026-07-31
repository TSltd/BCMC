# Motivation and Applications

## Motivation

Balanced Cyclic Matrix Construction (BCMC) did not originate as a theoretical exercise. It arose from a practical engineering problem:

> Given a collection of independent channels with varying duty requirements, how can switching events be distributed as uniformly as possible while exactly preserving the requested activity of every channel?

Examples include:

- multi-zone burst-fire heater control,
- deterministic load scheduling,
- PWM event distribution,
- time-division resource allocation,
- hardware event generation.

The objective is not merely fairness over time, but **uniform instantaneous loading** while preserving exact long-term totals.

### Original Application

The original application was multi-zone burst-fire heater control.

Each heater zone requested a different number of mains half-cycles during a control window. BCMC was used to construct a binary matrix whose rows represented the requested energy allocations while maintaining balanced aggregate loading across the scheduling window.

In the original implementation, BCMC generated the binary allocation matrix. The heater controller then traversed the matrix columns according to a seeded Fisher–Yates permutation. Every column was visited exactly once per scheduling window, preserving all BCMC invariants while avoiding visually and electrically repetitive switching patterns and remaining completely deterministic and reproducible.

This separation between **matrix construction** and **matrix traversal** is fundamental to BCMC. The construction defines only the binary matrix; the order in which its columns are interpreted is the responsibility of the downstream consumer. Sequential, pseudorandom, Gray-code, or application-specific traversal orders are all valid provided they are bijections of the column indices.

Early implementations exploited this separation by storing only the canonical matrix while representing the traversal as pointer arithmetic rather than physically permuting columns.

The seeded pseudorandom traversal was adopted in the original heater-control application because it reduced visually and electrically repetitive switching patterns while preserving exact per-zone energy delivery and the balanced aggregate loading guaranteed by BCMC.

Although the algorithm originated from this application, the construction itself is independent of time, power systems or control. It defines only a binary matrix. Other applications may interpret the rows and columns in entirely different ways.

---

## The Problem

Consider `C` independent channels.

Each channel requests a number of activations during a scheduling window of length `N`.

For example,

```text
Channel A: 6
Channel B: 3
Channel C: 5
```

must be distributed across

```text
N = 8
```

time slots.

Many valid schedules satisfy the row totals.

Very few also minimise the variation in the number of simultaneously active channels.

BCMC constructs one such schedule deterministically.

---

## What BCMC Guarantees

BCMC satisfies three independent properties.

### Exact Row Conservation

Every channel receives exactly its requested allocation.

```text
row_sum(i) = wᵢ
```

No approximation is introduced.

---

### Contiguous Internal Representation

Each row is represented internally by a single contiguous interval.

No optimisation or search is required.

This enables efficient implementations using

- pointer arithmetic,
- cyclic buffers,
- interval comparisons,
- FPGA logic,
- simple address generation.

---

### Globally Balanced Columns

The total number of active channels in each column differs by at most one.

More strongly, if

```text
W = qN + r
```

then the column occupancies are exactly

```text
q+1     for columns 0...r−1

q       for columns r...N−1.
```

This balance is independent of how the weights are distributed among the rows.

---

## Why This Matters

Many scheduling algorithms optimise individual channels.

BCMC instead optimises the aggregate behaviour.

This distinction is important.

For example, two schedules may deliver identical duty cycles while producing dramatically different instantaneous loads.

Reducing variation in instantaneous load can provide benefits including

- lower peak current,
- reduced thermal cycling,
- smoother power demand,
- improved utilisation of shared resources,
- reduced simultaneous switching noise,
- simpler downstream control.

The exact benefit depends on the application.

---

## Computational Properties

The BCMC construction is deterministic.

It requires no

- optimisation,
- search,
- iteration,
- convergence,
- heuristics.

The matrix is completely determined by

- `N`,
- the weight vector.

This makes BCMC attractive for hardware implementation.

---

## Hardware Implementation

The mathematical construction is independent of implementation.

Equivalent realisations include

- explicit binary matrices,
- pointer-based traversal,
- cyclic interval generation,
- FPGA peripherals,
- ASIC logic.

The construction naturally maps onto streaming hardware because each row corresponds to a contiguous interval.

A fuller discussion of the equivalent representations is given in the [Representations](BCMC.md#representations) section of the main document.

---

## Relationship to Existing Methods

Traditional approaches often distribute activity using

- round-robin scheduling,
- weighted round-robin,
- PWM,
- Bresenham-style error accumulation,
- stochastic scheduling,
- optimisation algorithms.

These methods solve related but different problems.

BCMC is distinguished by combining

- exact row conservation,
- deterministic construction,
- provably balanced column occupancies,
- a closed-form mathematical description.

The intent is not to replace existing schedulers universally, but to provide an alternative construction when these properties are desirable.

---

## Beyond Scheduling

BCMC should be viewed as a mathematical construction rather than a scheduling algorithm.

Scheduling is one interpretation.

Others include

- deterministic resource allocation,
- communication slot assignment,
- memory-bank activation,
- event sequencing,
- hardware peripheral design,
- balanced test-pattern generation.

Any problem requiring exact per-row totals together with globally balanced column occupancies may be formulated using BCMC.

---

## Summary

BCMC provides a deterministic construction of binary matrices with three key properties:

- exact preservation of row weights,
- exact balance of column occupancies,
- implementation using only simple cyclic interval arithmetic.

The accompanying Balance Theorem shows that the column occupancies are determined entirely by the total weight

```text
W = Σwᵢ,
```

independent of how that weight is distributed among the rows.

This combination of simplicity, exactness and implementation efficiency makes BCMC a useful primitive for both software and hardware systems.

BCMC intentionally separates construction from interpretation.

The construction is uniquely defined and admits rigorous mathematical analysis.

Interpretation—including traversal order, timing, and application semantics—is delegated entirely to downstream consumers.

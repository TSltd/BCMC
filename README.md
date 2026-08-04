# Balanced Cyclic Matrix Construction (BCMC)

> A reference implementation of the Balanced Cyclic Matrix Construction (BCMC) primitive, comprising a formal mathematical specification, executable reference model, verified RTL implementation, memory-mapped SoC interface, portable software driver, and comprehensive verification framework.

**BCMC** (Balanced Cyclic Matrix Construction) is a deterministic construction of binary matrices from integer weight vectors, and a reusable hardware primitive for balanced scheduling of weighted activities. It exactly preserves prescribed row weights while producing a globally balanced column occupancy with a provable imbalance of at most one, and exposing the resulting schedule independently of traversal or execution policy.

BCMC deliberately separates construction from interpretation: it constructs a canonical balanced representation, while traversal and application semantics belong entirely to downstream observers.

The project consists of two complementary parts:

- a mathematically rigorous specification and proof of the BCMC construction,
- an open-source hardware implementation targeting FPGA and System-on-Chip integration.

The long-term goal is to establish BCMC as a reusable hardware primitive for deterministic balanced scheduling, balanced activation and sparse incidence construction.

---

# Project Status

**Current release:** **v0.5c**

✔ Mathematical specification complete

✔ Balance Theorem formally proved

✔ Computational verification complete

✔ Hardware architecture defined

✔ BCMC Core RTL complete: the prefix transform `weights[] → offsets[]`

✔ BCMC Evaluator RTL complete: the characteristic function `M(i, j)`, plus its
row and column projections

✔ All of it lint-clean and verified against the Python reference model by two
independent simulators — including **exhaustive** verification of the
characteristic function for small `N`

✔ Register map specified, and modelled in Python before any bus RTL exists —
the same discipline that gave the mathematics a reference model

✔ BCMC context RTL complete: the persistent `(weights[], offsets[])` that
software and the Core share — storage and arbitration, and deliberately no
mathematics at all

✔ Wishbone B4 wrapper RTL complete: the register map made real, refusing with
`err` every access that is not exactly right — verified by replaying recorded
bus conversations from the Python peripheral model, in two simulators

✔ C driver complete: eight primitives, each one bus access, and `bcmc_load()`
built from nothing but those — compiled unmodified against the Wishbone RTL, and
held to its cost claims by counting bus accesses

✔ Observer contract specified and given a Python reference — a traversal is
proved to contribute order and nothing else, so the Balance Theorem survives
being read out of sequence

✔ Observer API in C complete: a sequential cursor and a seeded permuted one,
built on the driver and nothing else — held to the Python permutation index for
index, then run over the real Wishbone RTL, where a visit is shown to cost
exactly one `bcmc_read_column()` and a traversal to cost nothing at all

✔ Example integrations complete: three applications — a matrix printer, a GPIO
scheduler and a mains heater controller — each runnable over either reference
traversal, from three source files rather than six, all against the real
verilated peripheral and never a software model of it

⏳ Tang Nano 20K demonstration next (v1.0)

---

# Repository Structure

```text
README.md      Project overview and roadmap

docs/           Mathematical specification, proof, architecture, register map

rtl/            Generic, hardware-agnostic BCMC RTL modules

sw/             Portable C99 driver: no malloc, no OS, no headers but stdint

sim/            Simulation harnesses
  CMakeLists.txt    Verilator flow (primary)
  Makefile          Icarus Verilog flow (second opinion)
  vectors/          Test vectors generated from the Python reference model
  waves/            VCD output

validation/     Python reference implementation, peripheral model, test suites

scripts/        lint, format and the full end-to-end pipeline

dev/            Roadmap and development notes

fpga/           Board-specific FPGA projects

examples/       Demonstration applications
  common/             Argument parsing, traversal selection, the host seam
  matrix_dump/        The application that only looks
  gpio_scheduler/     One tick, one column, one port write
  heater_controller/  Exact duty cycles with a bounded peak load
```

---

# Building and Testing

Everything flows in one direction, and nothing downstream is trusted until
everything upstream of it is green:

```text
Proof  ->  Python reference  ->  test vectors  ->  RTL  ->  simulators  ->  FPGA
```

The entire pipeline, in that order:

```bash
./scripts/run_sim.sh              # add --big for the 10,000-case soak
```

Or one stage at a time:

```bash
# Reference model
python3 validation/reference.py                 # the executable specification
cd validation && python3 test_reference.py      # is the specification right?
cd validation && python3 gen_vectors.py         # emit sim/vectors/*.txt

# RTL
cd validation && python3 bcmc_periph.py         # the register map, executable
cd validation && python3 test_periph.py         # does it obey Register_Map.md?

# Software
cd validation && python3 observers.py           # the reference observers
cd validation && python3 test_observers.py      # do they obey Observers.md?
cd validation && python3 gen_observer_vectors.py  # write pi down for the C

cd validation && python3 gen_wb_vectors.py      # record the bus conversations

./scripts/lint.sh                               # verilator -Wall, iverilog -Wall

mkdir -p sim/build && cd sim/build              # Verilator: RTL == Python
cmake .. && make && ctest --output-on-failure

cd sim && make                                  # Icarus: the same, independently
cd sim && make gtkwave                          # look at a waveform

./scripts/run_examples.sh                       # does the traversal matter?
```

The `ctest` step includes `bcmc_driver_test`, which compiles `sw/bcmc.c` itself
and drives `rtl/bcmc_wb.v` with it, and `bcmc_observer_test`, which stacks
`sw/bcmc_observer.c` on top of that driver and checks its permutations against
the ones `validation/observers.py` wrote down. `run_sim.sh` additionally rebuilds
both with every C and C++ compiler it can find, since a Verilog simulator cannot
give a second opinion on C.

Its last step is the only one that checks a claim no single testbench can.
`scripts/run_examples.sh` runs each example twice with nothing changed but the
traversal, and compares the two programs' output: the summaries must be
byte-identical, and the running logs must differ. The first says the observer
changed nothing that was proved; the second says the observer really did change
something. See `examples/README.md`.

Requirements: Python 3, Verilator ≥ 5, CMake ≥ 3.20, a C++17 compiler, and
optionally a second C compiler, Icarus Verilog and gtkwave.

---

# Documentation

| Document                              | Description                                                     |
| ------------------------------------- | --------------------------------------------------------------- |
| `docs/BCMC.md`                        | Formal mathematical definition of BCMC                          |
| `docs/Proof.md`                       | Complete proof of the Balance Theorem                           |
| `docs/Verification.md`                | Exhaustive and randomized verification                          |
| `docs/Hardware_Architecture.md`       | Hardware architecture and IP specification                      |
| `docs/Register_Map.md`                | Programmer's model: the software-facing contract                |
| `docs/Transaction_Sequences.md`       | Canonical bus transactions, in order                            |
| `docs/Observers.md`                   | The observer contract: what traversal may do                    |
| `examples/README.md`                  | `Application × Traversal`, and why it is a product              |
| `docs/Motivation_and_Applications.md` | Motivation, design philosophy and applications                  |
| `docs/Why_BCMC.md`                    | Why would I use BCMC?                                           |
| `docs/Design_Rationale.md`            | Why is it designed this way? Key design decisions and reasoning |

---

# Mathematical Results

Let

```text
W = qN + r
```

where

```text
0 ≤ r < N.
```

BCMC guarantees:

- exact preservation of every prescribed row weight,
- column occupancies of exactly `q` or `q+1`,
- exactly `r` columns of occupancy `q+1`,
- occupancy independent of the distribution of the input weights.

The proof is constructive and is based on a bijection between projected prefix intervals and residue classes.

---

# Hardware Architecture

The hardware implementation follows the mathematical decomposition.

```text
Weight Vector
      │
      ▼
+----------------+
|   BCMC Core    |
| Prefix Offsets |
+----------------+
      │
      ▼
Canonical Prefix Representation

(weights[], offsets[])

      │

      ▼
+--------------------+
|  BCMC Evaluator    |
| Characteristic     |
| Function           |
+--------------------+
      │
      ├──────── Cell Query
      ├──────── Row Query
      └──────── Column Query

      ▼
Application-specific Observer
```

The canonical mathematical object is the matrix `M` itself. The BCMC Core
computes the **canonical prefix representation** `(weights[], offsets[])`, which
is a lossless representation of the canonical BCMC matrix and is the internal
representation used by the hardware architecture.

The BCMC Evaluator computes arbitrary elements of the BCMC matrix on demand. It
is an **architectural concept, not a module**: all of its mathematics lives in a
single purely combinational `rtl/bcmc_cell.v`, because `M(i, j)` depends on
nothing but its own arguments. Cell, row, column and matrix are four
**projections** of that one function, and neither rows nor columns are
privileged; `bcmc_row.v` and `bcmc_column.v` add no arithmetic at all and are
verified to be nothing but replicated cells.

Traversal order and application semantics are intentionally excluded from the BCMC definition and belong to downstream observer implementations.

The Core is a **transform, not an accelerator**. It has no GPIO, no timers, no
observers, no bus interface and no matrix output. Its only state is the prefix
accumulator and the row index, and because the hypothesis `0 ≤ wᵢ ≤ N` of Lemma 2
guarantees `offset + weight < 2N`, its `mod N` is one comparator and one
subtractor — never a divider. The hypothesis of the theorem is the reason the
hardware is cheap. See `docs/Hardware_Architecture.md` and `rtl/README.md`.

---

# Development Roadmap

See `dev/ROADMAP.md` for the detailed plan, including a note on the things that
are deliberately _not_ in it.

```
Research
────────
v0.1  Mathematics
v0.2  Core
v0.3  Evaluator

Engineering
───────────
v0.4  SoC Integration
v0.5  Reference Observers

Release
───────
v1.0  Stable BCMC IP
```

## v0.1 — the mathematics ✔

- Mathematical specification
- Formal proof
- Verification
- Hardware architecture

## v0.2 — the BCMC Core ✔

- Python reference model and its own test suite
- BCMC Prefix Stream Interface, with a cycle-accurate timing contract
- BCMC Core RTL: one prefix accumulator, no RAM, no divider
- Equivalence against the reference model under Verilator and Icarus Verilog

## v0.3 — the BCMC Evaluator ✔

- The characteristic function in hardware: purely combinational, no clock, no state
- `mod N` as one comparator and one conditional add — the mirror of the Core's subtract
- Exhaustive verification: **every** query for `N ≤ 8`, swept to `N ≤ 40`
- Row and column projections, proven to be nothing but replicated cells
- Row conservation and the Balance Theorem checked on assembled RTL matrices

## v0.4 — SoC Integration

- Register map, written as a specification rather than as documentation of an
  implementation ✔
- Python peripheral model, validated against the same reference vectors that
  drive the RTL ✔
- `bcmc_context.v`: the persistent BCMC context that software and the Core
  share — with no `N` port and no `C` port, so "it contains no BCMC mathematics"
  is checkable rather than merely asserted ✔
- `bcmc_wb.v`: a Wishbone B4 Classic slave with `err` for every access that is
  not meaningful, verified by replaying the recorded transaction sequences of
  the Python peripheral model rather than by inventing expected values ✔
- `sw/bcmc.{h,c}`: primitives, then composition — the real driver source
  compiled into the Verilator harness and run against the RTL through the bus
  functional model, with geometry refused locally at zero bus cost and state
  refused by the wrapper at exactly one ✔

## v0.5 — Reference Observers

Reference Observers demonstrate ways to consume the BCMC representation. They
are not part of the BCMC definition and may be replaced or extended without
affecting the mathematical or hardware contracts of the primitive.

- The observer contract, written before any observer exists ✔
- Python reference observers — sequential, and a seeded permutation ✔
- A conformance suite that re-derives balance and conservation from the vector
  files, so an observer cannot quietly lose a column ✔
- `sw/bcmc_observer.{h,c}`: a pinned generator, two order builders and a cursor,
  over the driver and nothing else — no allocation, no cached matrix, and a
  visit that costs exactly what `bcmc_read_column()` costs, proved by counting
  bus accesses against the verilated peripheral ✔
- Example integrations: `Application × Traversal` as a Cartesian product —
  `matrix_dump`, `gpio_scheduler` and `heater_controller`, each over either
  reference traversal, with the orthogonality established by diffing program
  output rather than asserted in a comment ✔

## v0.6 — FPGA Reference Platform

- Tang Nano 20K integration
- Resource utilisation report
- Timing closure
- Hardware demonstration
- Programming examples

## v0.7 — Formal Verification

- SymbiYosys property suite
- Core properties
- Cell properties
- Wishbone protocol properties
- Coverage report

## v0.8 — Documentation Polish

- Why BCMC? - Why_BCMC.md - Why would I use BCMC?
- Design rationale - Design_Rationale.md - Why is it designed this way?
- Tradeoffs.md
- Performance notes
- Integration guide
- API reference
- Repository cleanup

## v0.9 — Ecosystem Readiness

- CI improvements
- Packaging
- Release artefacts
- Issue templates
- Contribution guide
- Licensing review

## v1.0 — Stable BCMC IP

- Stable API
- Stable RTL
- Stable driver
- Stable documentation
- Reference FPGA design
- Verification complete
- First public release

## Post-v1.0

- Community review
- Host organization discussion
- Gather feedback
- Address feedback
- Future integrations

## Future

- AXI-Lite wrapper
- Streaming observer
- DMA observer
- Multi-instance BCMC

# Design Philosophy

BCMC deliberately separates **construction** from **interpretation**.

The BCMC algorithm constructs a canonical mathematical object.

Applications determine how that object is observed.

This separation allows the same canonical representation to support multiple independent observers, including sequential traversal, seeded pseudorandom traversal, DMA engines, software interfaces and custom hardware accelerators, while preserving all proven mathematical properties.

---

# License

This project is open source. License information will be added prior to the first public release.

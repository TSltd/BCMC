# Balanced Cyclic Matrix Construction (BCMC)

> Reference implementation of Balanced Cyclic Matrix Construction (BCMC): mathematical specification, formal proof, Verilog IP cores, FPGA implementations and reference validation.

**BCMC** is a deterministic construction of binary matrices from integer weight vectors. It exactly preserves prescribed row weights while producing a globally balanced column occupancy distribution.

The project consists of two complementary parts:

- a mathematically rigorous specification and proof of the BCMC construction,
- an open-source hardware implementation targeting FPGA and System-on-Chip integration.

The long-term goal is to establish BCMC as a reusable hardware primitive for deterministic balanced scheduling, event generation and sparse incidence construction.

---

# Project Status

**Current release:** **v0.2**

✔ Mathematical specification complete

✔ Balance Theorem formally proved

✔ Computational verification complete

✔ Hardware architecture defined

✔ BCMC Core RTL complete, lint-clean and verified against the Python reference
model by two independent simulators

⏳ BCMC Evaluator RTL next (v0.3)

---

# Repository Structure

```text
docs/           Mathematical specification, proof and hardware architecture

rtl/            Generic, hardware-agnostic BCMC RTL modules

sim/            Simulation harnesses
  CMakeLists.txt    Verilator flow (primary)
  Makefile          Icarus Verilog flow (second opinion)
  vectors/          Test vectors generated from the Python reference model
  waves/            VCD output

validation/     Python reference implementation and its own test suite

scripts/        lint, format and the full end-to-end pipeline

dev/            Roadmap and development notes

fpga/           Board-specific FPGA projects

examples/       Demonstration applications
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
python3 validation/reference.py                 # the executable specification
cd validation && python3 test_reference.py      # is the specification right?
cd validation && python3 gen_vectors.py         # emit sim/vectors/*.txt

./scripts/lint.sh                               # verilator -Wall, iverilog -Wall

mkdir -p sim/build && cd sim/build              # Verilator: RTL == Python
cmake .. && make && ctest --output-on-failure

cd sim && make                                  # Icarus: the same, independently
cd sim && make gtkwave                          # look at a waveform
```

Requirements: Python 3, Verilator ≥ 5, CMake ≥ 3.20, a C++17 compiler, and
optionally Icarus Verilog and gtkwave.

---

# Documentation

| Document                              | Description                                    |
| ------------------------------------- | ---------------------------------------------- |
| `docs/BCMC.md`                        | Formal mathematical definition of BCMC         |
| `docs/Proof.md`                       | Complete proof of the Balance Theorem          |
| `docs/Verification.md`                | Exhaustive and randomized verification         |
| `docs/Hardware_Architecture.md`       | Hardware architecture and IP specification     |
| `docs/Motivation_and_Applications.md` | Motivation, design philosophy and applications |

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

The BCMC Evaluator computes arbitrary elements of the BCMC matrix on demand.

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

## v0.3 — the BCMC Evaluator

- Cell evaluator
- Row evaluator
- Column evaluator
- Matrix-level equivalence tests

## v0.4 — making it addressable

- Memory-mapped peripheral interface
- Wishbone bus support
- Software driver

## v1.0

- Tang Nano 20K demonstration
- Timing closure and resource report
- Reference applications
- Initial stable release

---

# Design Philosophy

BCMC deliberately separates **construction** from **interpretation**.

The BCMC algorithm constructs a canonical mathematical object.

Applications determine how that object is observed.

This separation allows the same canonical representation to support multiple independent observers, including sequential traversal, seeded pseudorandom traversal, DMA engines, software interfaces and custom hardware accelerators, while preserving all proven mathematical properties.

---

# License

This project is open source. License information will be added prior to the first public release.

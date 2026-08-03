# Roadmap

Each stage is finished only when the stage above it is still green. The order is
not arbitrary: it is the order of dependency.

```
Proof  ->  Python reference  ->  test vectors  ->  RTL  ->  simulators  ->  FPGA
```

---

```
v0.1  -- the mathematics                                            [ done ]
├── Mathematics complete
├── Proof complete
├── Verification complete
└── Architecture complete

v0.2  -- the BCMC Core                                              [ done ]
├── Python reference model            validation/reference.py
├── Reference model validated         validation/test_reference.py
├── Test vector generator             validation/gen_vectors.py
├── BCMC Prefix Stream Interface      docs/Hardware_Architecture.md
├── BCMC Core RTL                     rtl/bcmc_core.v
├── Prefix accumulator                one register and one conditional subtract
├── Verilator harness (RTL == Python) sim/bcmc_core_test.cpp
└── Icarus testbench (second opinion) sim/tb_core.v

v0.3  -- the BCMC Evaluator
├── Python reference already in place  bcmc_cell / bcmc_row / bcmc_column
├── Cell evaluator                    rtl/bcmc_cell.v
├── Row evaluator                     rtl/bcmc_row.v
├── Column evaluator                  rtl/bcmc_column.v
├── Evaluator                         rtl/bcmc_evaluator.v
└── Matrix-level equivalence tests

v0.4  -- making it addressable
├── Wishbone interface
├── AXI-Lite interface (optional)
└── Software driver

v1.0  -- silicon and story
├── Tang Nano 20K demo
├── Timing closure and resource report
├── Documentation
├── Example applications
└── First release
```

---

## Notes on things deliberately _not_ in the plan

**No Offset RAM.** An earlier draft of v0.2 listed one. There is no RAM in the
BCMC Core and there should not be. The recurrence

```
offset[0]   = 0
offset[i+1] = (offset[i] + weight[i]) mod N
```

carries all of its state in a single accumulator: computing `offset[i+1]` needs
`offset[i]` and nothing earlier. Storing the offsets would be storing the
Core's _output_, which is the consumer's business, not the Core's. The Core
streams. Whoever needs the offsets later can buffer them, and in v0.3 the
Evaluator will consume them as they arrive without buffering them at all.

**No divider.** `mod N` is a comparison and a subtraction, because the
hypothesis `0 ≤ wᵢ ≤ N` of Lemma 2 guarantees `offset + weight < 2N`. The
hypothesis of the theorem is the reason the hardware is cheap.

**No `final_offset` output.** `P_C mod N` belongs to no row, evaluates no
matrix element and appears nowhere in the proof. Exposing it would invite
chaining two Cores, which is not the same construction.

**No `bcmc_prefix.v` yet.** The Core is one module because it is small enough
to be one module. Hierarchy gets introduced when the code demonstrates a need
for it, not in anticipation of one.

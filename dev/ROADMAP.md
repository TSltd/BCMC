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

v0.3  -- the BCMC Evaluator                                         [ done ]
├── Python reference already in place  bcmc_cell / bcmc_row / bcmc_column
├── The characteristic function        rtl/bcmc_cell.v
├── Wrap logic                        one comparator and one conditional add
├── Exhaustive verification           every query for N <= 8, swept to N <= 40
├── Row projection    (replication)   rtl/bcmc_row.v
├── Column projection (replication)   rtl/bcmc_column.v
├── Compositional equivalence         every bit vs a separate bcmc_cell
├── Verilator harnesses               sim/bcmc_{cell,row,column}_test.cpp
└── Icarus testbenches                sim/tb_{cell,row,column}.v

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
streams. Whoever needs the offsets later can buffer them, and v0.3 bore this
out: `bcmc_cell` takes an offset as an argument and keeps nothing at all.

**No divider.** `mod N` is a comparison and a subtraction, because the
hypothesis `0 ≤ wᵢ ≤ N` of Lemma 2 guarantees `offset + weight < 2N`. The
hypothesis of the theorem is the reason the hardware is cheap.

**No `final_offset` output.** `P_C mod N` belongs to no row, evaluates no
matrix element and appears nowhere in the proof. Exposing it would invite
chaining two Cores, which is not the same construction.

**No `bcmc_prefix.v` yet.** The Core is one module because it is small enough
to be one module. Hierarchy gets introduced when the code demonstrates a need
for it, not in anticipation of one.

**No `bcmc_evaluator.v`, ever.** An earlier draft of v0.3 listed one, between
the row and column modules and their caller. "The BCMC Evaluator" is an
architectural concept -- the half of the design that evaluates the
characteristic function -- and a concept does not imply a Verilog module. All of
its mathematics is in `bcmc_cell.v`. A front end that chose between cell, row
and column queries would be a bus adapter, and it belongs to v0.4 with the bus
it adapts to.

**v0.3 was not three milestones.** The plan above reads cell, row, column, but
the work was `cell -> replicate -> replicate`: the only new RTL design in v0.3
is the cell. `bcmc_row.v` and `bcmc_column.v` are `generate` loops over it and
add no arithmetic whatsoever, which is why the verification effort went into
proving that claim -- every bit of both, compared against a separately
instantiated `bcmc_cell` -- rather than into re-testing the cell through two
wrappers.

**Neither rows nor columns are privileged.** Matrix, row, column and cell are
four projections of one function. The column projection is the one an allocator
usually wants, because a column is a scheduling slot and the Balance Theorem is
a statement about its popcount; that makes it useful, not fundamental.

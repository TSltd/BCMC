# Roadmap

Each stage is finished only when the stage above it is still green. The order is
not arbitrary: it is the order of dependency.

```
Proof  ->  Python reference  ->  test vectors  ->  RTL  ->  simulators  ->  FPGA
```

---

v0.1–v0.3 Mathematical primitive

v0.4–v0.5 Integration

v1.0 Release

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

v0.4  -- SoC Integration
├── v0.4a  the register map is a specification          [ done ]
│   ├── Register map (the contract)      docs/Register_Map.md
│   ├── Python peripheral model          validation/bcmc_periph.py
│   └── Conformance suite                validation/test_periph.py
├── v0.4b  the BCMC context                            [ done ]
│   ├── MAX_C x (weight, offset)         rtl/bcmc_context.v
│   ├── Storage and arbitration only     no N port, no C port, no mathematics
│   ├── Verilator harness                sim/bcmc_context_test.cpp
│   └── Icarus testbench                 sim/tb_context.v
├── v0.4c  the Wishbone wrapper                        [ done ]
│   ├── Canonical bus transactions       docs/Transaction_Sequences.md
│   ├── Classic Wishbone B4 slave        rtl/bcmc_wb.v
│   ├── Recorded bus conversations       validation/gen_wb_vectors.py
│   ├── Bus functional model             sim/common/wb_bfm.{h,cpp}
│   ├── Verilator harness (RTL == model) sim/bcmc_wb_test.cpp
│   └── Icarus testbench                 sim/tb_wb.v
└── v0.4d  the software driver                         [ done ]
    ├── C99 driver, no malloc, no OS     sw/bcmc.{h,c}
    ├── Primitives, then composition     bcmc_load() adds no logic of its own
    ├── The driver against the RTL       sim/bcmc_driver_test.cpp
    └── Two compilers, not two sims      gcc/clang, as C99 and as C++17

v0.5  -- Reference Observers
│   Reference Observers demonstrate ways to consume the BCMC representation.
│   They are not part of the BCMC definition and may be replaced or extended
│   without affecting the mathematical or hardware contracts of the primitive.
├── v0.5a  what an observer is                         [ done ]
│   ├── The observer contract            docs/Observers.md
│   ├── Python reference observers       validation/observers.py
│   └── Conformance suite                validation/test_observers.py
├── v0.5b  the observer API                            [ done ]
│   ├── Traversal over the driver        sw/bcmc_observer.{h,c}
│   ├── Sequential column iterator       the identity traversal
│   ├── Deterministic permutation        a seeded bijection of 0 .. N-1
│   └── Observers against the RTL        sim/bcmc_observer_test.cpp
└── v0.5c  example integrations                        [ done ]
    │   Application × Traversal is a Cartesian product: three applications,
    │   two reference traversals, six programs, three source files.
    ├── The application that only looks  examples/matrix_dump/
    ├── One tick, one column, one port   examples/gpio_scheduler/
    ├── Balanced scheduling              examples/heater_controller/
    ├── Traversal chosen, never written  examples/common/example_traversal.{h,c}
    ├── The host seam, twice             sim/example_host.cpp
    │                                    examples/common/example_host_mmio.c
    └── Orthogonality, by diff           scripts/run_examples.sh

v0.6  -- FPGA Reference Platform

    ├── Tang Nano 20K integration
    ├── Resource utilisation report
    ├── Timing closure
    ├── Hardware demonstration
    └── Programming examples

v0.7  -- Formal Verification

    ├── SymbiYosys property suite
    ├── Core properties
    ├── Cell properties
    ├── Wishbone protocol properties
    └── Coverage report

v0.8  -- Documentation Polish

    ├── Why BCMC? - Why_BCMC.md - Why would I use BCMC?
    ├── Design rationale - Design_Rationale.md - Why is it designed this way?
    ├── Tradeoffs.md
    ├── Performance notes
    ├── Integration guide
    ├── API reference
    └── Repository cleanup

v0.9  -- Ecosystem Readiness

    ├── CI improvements
    ├── Packaging
    ├── Release artefacts
    ├── Issue templates
    ├── Contribution guide
    └── Licensing review

v1.0  -- Stable BCMC IP

    ├── Stable API
    ├── Stable RTL
    ├── Stable driver
    ├── Stable documentation
    ├── Reference FPGA design
    ├── Verification complete
    └── First public release

Post-v1.0

    ├── Contact host organization
    ├── Gather feedback
    ├── Address requested changes
    └── Decide whether to pursue contribution

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

**The register map is not documentation.** v0.4a produced no RTL on purpose.
`docs/Register_Map.md` is a specification in exactly the sense `docs/BCMC.md` is
one, and `validation/bcmc_periph.py` is its reference model in exactly the sense
`validation/reference.py` is. Where the model and the wrapper disagree, the
document decides; where the document is silent, that is a bug in the document.
The peripheral model contains no mathematics of its own -- every matrix bit it
returns comes from `reference.py` -- so `sim/bcmc_wb_test.cpp` will be able to
replay transactions that were validated before any bus existed.

**The transaction sequences were executed before the wrapper existed.**
`docs/Transaction_Sequences.md` is prose, so it was replayed against
`validation/bcmc_periph.py` — every sequence, every failure row, and a check that
each `err` leaves the peripheral bit-for-bit unchanged. It found two defects, and
neither was in the RTL, because there was no RTL: one in the new document, and one
in `Register_Map.md` itself, whose programming sequence said "wait until
`STATUS.BUSY == 0`" when `BUSY` is already `0` after reset and `VALID` is the bit
that means a matrix exists. The model's own driver helper was polling the same
wrong bit. A specification that has never been executed is just prose, and that
applies to the specification of the bus exactly as it applied to the mathematics.

**`bcmc_context.v` is not a RAM.** It was called `bcmc_store.v` for about an
hour. The name mattered: it is the persistent BCMC context, the object that
exists between software and hardware, and it has three clients rather than a
port -- software writes weights, the Core writes offsets, the Evaluator reads
both. This is also where the RAM that v0.2 refused to put in the Core belongs.
The Core still streams; the peripheral is simply the first component that has a
reason to own the canonical prefix representation.

**`bcmc_context.v` has no `N` port and no `C` port.** v0.4b was asked for a
module with no BCMC mathematics in it, and a comment claiming so would have been
worth nothing. Both equations need `N`, and the balance and conservation
properties need `C`, so the module is given neither: it cannot express the
mathematics even by accident, and the claim becomes structurally checkable rather
than merely asserted. Two design questions fall out of that absence for free.
Software cannot write an offset, because no port exists through which to try --
the read-only `OFFSET` window below is a fact about the port list, not a rule the
module enforces. And `load_start` clears the _whole_ offset window rather than the
first `C` lanes, because it does not know `C`, which is exactly what the register
map requires. The only arithmetic left in the file is `wr_ptr + 1`, which is
addressing.

**No writable `OFFSET` window.** Offsets are read-only over the bus. Letting
software write them would make `offset[0] = 0` an accident of initialisation
rather than a structural fact, and would admit matrices that BCMC cannot
construct -- so the Balance Theorem would no longer describe the peripheral's
output. This is the same door that leaving out `P_C mod N` closes.

**No matrix RAM, and no row window.** A column is bounded by `MAX_C`, so it fits
in a fixed number of registers; a row is `N` bits with `N` a run-time value, so
it does not. The register map therefore exposes `CELL` and `COLUMN[k]` and no
row projection, which is a fact about address spaces rather than about
mathematics -- `rtl/bcmc_row.v` exists and is unaffected.

**No `DONE` bit and no traversal registers.** A completion pulse is useless to
polling software, so `BUSY` and `VALID` carry the state instead and `IRQ` is
level-sensitive with write-1-to-clear. A "next column" register would be a
traversal order, and traversal belongs to observers in v0.5, not to the bus.

**The bus vectors are a recorded conversation, not a table.** Every earlier
vector file is a list of instances of the mathematics. `wb_*.txt` could not be:
the thing under test in v0.4c is a protocol, and a protocol has an order.
`validation/gen_wb_vectors.py` therefore drives `validation/bcmc_periph.py`
through the sequences of `docs/Transaction_Sequences.md` and writes down what
happened -- five ops (`Z`, `L`, `R`, `W`, `P`), read identically by both
simulators.

**The `P` op exists because the model has no clock.** `BcmcPeripheral(latency=k)`
makes the next `k` _accesses_ observe `BUSY`; hardware needs `C + 4` _clocks_.
A recorded `STATUS` read taken just after `START` cannot be replayed literally
against wires, so what gets recorded is not the value but the wait. That is the
one place where the recording is a translation rather than a transcript, and it
is confined to a single op so that nothing else has to be.

**Every access costs three clocks, deliberately.** `bcmc_wb` qualifies a new
access with `!wb_ack_o && !wb_err_o`, so the response is exactly one cycle wide
even if the master holds `stb`. That is a property of the slave, not a courtesy
of the master, and a master that drops `stb` the instant it sees `ack` can never
tell whether the slave has it. Both masters therefore hold the request one clock
beyond the response and inspect it. A mutation removing the qualifier survived
the suite until they did.

**A tripped `$stop` must fail the run.** Under a batch `vvp` with no terminal,
`$stop` prints `** Continue **` and the simulation proceeds -- so a design whose
own assertions were screaming could still reach its `PASS` line. `sim/Makefile`
now treats any `stop called` in the transcript as a failure, whatever the
testbench concluded. This was found by a mutation that reported as surviving and
had in fact been detected three times over: the detection was right, the verdict
was wrong, and the hole was in the harness rather than in the vectors.

**`bcmc_load()` contains no logic of its own.** v0.4d is primitives and then
composition, the same shape as `bcmc_cell -> bcmc_row / bcmc_column`: eight
primitives, each one bus access, and a convenience function built only out of
them. A claim like that is worth nothing unless it is measurable, so the harness
counts accesses -- `bcmc_load(w, n, c)` costs exactly `C + 5`, which is what the
primitives cost when nothing else happens. There is one stated exception:
`bcmc_start()` is a read-modify-write, because `bcmc_wb` reloads `IRQ_EN` from
_every_ `CTRL` write and a lone `START` would silently disarm the interrupt.

**Geometry is refused locally; state never is.** `MAX_C` is discovered once from
`CAPS`, so an out-of-range index is a fact the driver already holds: it returns
`BCMC_ERANGE` after _zero_ accesses. `BUSY` and `VALID` are not knowable without
asking, so the driver does not guess -- it issues the access, lets the wrapper
`ERR`, and reports `BCMC_EREFUSED` after _exactly one_. Both counts are asserted,
which is what stops the driver from drifting into either duplicating the
wrapper's state machine or pretending to know the part's geometry.

**An observer proves nothing new, and that is the point.** v0.5a could have been
a pair of loops; instead it is a contract, a reference model and a conformance
suite, because the interesting claim about a traversal is a negative one -- that
it changes nothing. `validation/test_observers.py` therefore re-derives row
conservation and the balance multiset from the vector files under _every_
traversal it can build, and checks that a permuted pass is a pure reordering of
the sequential one. Balance is a property of the matrix; smoothness is a property
of the observer; and the second must not be bought with the first.

**The shuffle has a negative control.** A Fisher-Yates loop that draws from the
whole range instead of the prefix still returns a bijection, so every structural
test passes and the distribution is quietly wrong. The uniformity suite runs that
exact bug alongside the real shuffle and _requires_ it to be rejected: 5743.9
against a threshold of 80, where the correct shuffle scores 23.3. A statistical
test nobody has watched fail is not a test.

**Two compilers instead of two simulators.** Every other component is checked by
Verilator and Icarus reading the same vectors. A C driver cannot be: Verilog
simulators do not compile C. The independent second opinion for v0.4d is
therefore a second toolchain rather than a second simulator -- `sw/bcmc.c` is
built by gcc and clang, as C99 and again as C++17, and the harness compiles the
real file rather than a copy of it.

**The permutation is a table before it is code.** `sw/bcmc_observer.c` reshuffles
`0 .. n-1` in C, and `validation/observers.py` reshuffles it in Python, and the
one thing the harness must never do is shuffle it a third time in C++ to decide
who is right. `validation/gen_observer_vectors.py` therefore writes the Python
permutations down -- 11 seeds over 40 lengths, plus the raw generator draws --
and `sim/bcmc_observer_test.cpp` reads them before it simulates anything. This is
the same rule as everywhere else in the project, applied to a function that has
no mathematics in it: the expected answer comes from the model that was
validated, never from the harness. Two implementations agreeing is worth nothing
if the referee is a third implementation nobody checked.

**A cursor is not a transaction, and the harness proves it by counting.**
`sw/bcmc_observer.h` claims that building an order, starting a pass, peeking at
`pi(t)` and rewinding all cost _zero_ bus accesses, and that a visit costs
exactly what `bcmc_read_column()` costs and not one access more. Those are the
only claims an observer can make that are worth making -- it adds no traffic --
and they are unfalsifiable in prose, so the harness meters the bus around every
call. The `1 + ceil(MAX_C/32)` figure for a visit is the same one v0.4d pinned
for the primitive underneath it, unchanged by having a traversal on top.

**The observer allocates nothing, by policy.** Every buffer in the API belongs to
the caller: the order array, the bitmap the bijection check scratches in, the
words a visit is decoded into. An observer that allocated would be the beginning
of an allocator, and an allocator is an _application_ of BCMC rather than a part
of it -- the moment `sw/` owns memory, the question of which traversal is the
right one stops being the caller's. That is also why `bcmc_order_is_bijection()`
takes a scratch bitmap and clears it itself: a caller reusing one buffer across a
whole sweep is the expected case, and the harness hands it a dirty one to make
sure.

**An example is an application, not an observer.** v0.5c was originally planned
as `examples/sequential_observer/` and `examples/random_observer/` -- one
directory per traversal. That layout had the axes the wrong way round. A
traversal is not something an example demonstrates by containing it; it is
something an example demonstrates by _not_ containing it. So the directories are
named for what they are for -- `matrix_dump/`, `gpio_scheduler/`,
`heater_controller/` -- and the traversal is a command-line option in all three,
which is what makes `Application × Traversal` visibly a product rather than a
list. Six programs from three source files. Had the original layout been built,
each observer would have arrived with an application welded to it, and the two
would never have been separable again.

**Orthogonality is checked with a diff, not asserted in prose.** The claim of
v0.5c -- an observer contributes order and nothing else -- is a claim about two
programs, and no single testbench can hold both. `scripts/run_examples.sh`
therefore runs each application twice with only the traversal changed and
compares the _outputs_: `--summary` must be byte-identical (P1-P4 held) and the
running log must differ (the traversal was genuinely different). The second
comparison is the one that matters. Two identical summaries prove nothing if
the two traversals were secretly the same traversal, which is also why
`ex_traversal_init()` refuses an unknown name instead of quietly falling back to
the sequential one: a silent fallback would make every claim in the tree
unfalsifiable by making the negative control pass for the wrong reason.

**Never a second model of the peripheral.** The examples are ordinary C
programs, which makes it tempting to give them a software stand-in for the
hardware so they can be run anywhere. `sim/example_host.cpp` is instead a cable
to the verilated `rtl/bcmc_wb.v` -- the same RTL the whole project has been
checking -- because an application that passed against a fake would only
establish that two pieces of software agree. There are exactly two hosts, and
neither of them is a model: one drives wires in simulation, the other drives
memory-mapped registers on a real part.

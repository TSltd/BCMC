# BCMC Examples

Three applications, two traversals, one peripheral.

These programs exist to demonstrate a single claim, and it is a claim about
structure rather than about performance:

> An observer contributes **order**, and nothing else.

Everything in this directory is arranged so that the claim can be _checked_
instead of asserted. The applications never implement a traversal; the
traversals never learn what an application does with a visit. Which means the
two axes are independent, and if they are independent then the set of programs
here is a Cartesian product:

| Application         | `--traversal sequential` | `--traversal permuted` | What it does with a column                        |
| ------------------- | ------------------------ | ---------------------- | ------------------------------------------------- |
| `matrix_dump`       | ✔                        | ✔                      | prints it                                         |
| `gpio_scheduler`    | ✔                        | ✔                      | writes it to a port, one tick per column          |
| `heater_controller` | ✔                        | ✔                      | fires triacs on it, one mains half-cycle per tick |

Six programs, three source files. The six cells are the six ctest entries
`example_<app>_<traversal>`, and `scripts/run_examples.sh` compares the cells
against each other rather than against numbers a test author chose.

---

## The dependency chain

```text
   Application            examples/matrix_dump/     purpose
                          examples/gpio_scheduler/  "what a visit means"
                          examples/heater_controller/
        |
        v
   Observer API           sw/bcmc_observer.{h,c}    order
                          "which column next"
        |
        v
   BCMC driver            sw/bcmc.{h,c}             transactions
                          load, probe, read column
        |
        v
   Host seam              sim/example_host.cpp      wires
                          examples/common/example_host_mmio.c
        |
        v
   Peripheral             rtl/bcmc_wb.v             the matrix
```

Every arrow points down and none point back up. Specifically:

- An **application** calls `bcmc_observer_next()` and is handed a column index
  and a bitmap. It does not know whether the index came from a counter or a
  shuffle, and there is no way for it to find out that does not amount to
  cheating.
- An **observer** yields indices and forwards `bcmc_read_column()`. It does not
  know that one caller is printing and another is switching mains voltage.
- The **driver** does not know an observer exists. The register map has no "next
  column" register, deliberately, so traversal cannot leak into the hardware.
- The **peripheral** evaluates `M(i, j)` on demand. Nothing above it holds the
  matrix.

The applications choose a traversal by **name**, at the command line, and the
whole of the coupling between the two axes is one file:
`examples/common/example_traversal.c`. `ex_traversal_init()` turns a name into
an enum; `ex_traversal_begin()` turns the enum into a `bcmc_observer_t`. Note
what is absent from both: any loop over indices, any arithmetic on a column
number, any random number. That file _selects_; `sw/bcmc_observer.c` _supplies_.
Below it, no application logic mentions `sequential` or `permuted` at all.

---

## The applications

### `matrix_dump` — the application that only looks

The honest one. It visits every column, prints the rows that are active, and
then prints three tallies: columns visited, activations per row, and the load
histogram. It performs no output beyond `stdout`, which makes it the reference
against which the other two are read.

```console
$ example_matrix_dump --n 12 --weights 5,3,7,1,4
```

### `gpio_scheduler` — one tick, one column, one port write

The minimum useful consumer. A tick advances the observer, and the column
bitmap is copied **straight to the port word**: no decode, no lookup, no
scheduling policy. The column _is_ the port word, which is the point of
representing a schedule as a matrix in the first place. It reports demanded
on-ticks against actual on-ticks per channel (P2) and the simultaneity
histogram against the `q + 1` bound (the Balance Theorem).

```console
$ example_gpio_scheduler --n 8 --weights 4,4,4,4 --rounds 3
```

### `heater_controller` — why balance is the whole point

The application that would be hard to write any other way. `C` resistive loads
share a supply; each has a demanded duty cycle `w_i / N`; a mains zero-cross is
the tick. Two properties are doing real work here:

- **Row conservation** (P2) means each heater receives exactly its demanded
  duty over a period — the power is _exact_, not approximated by a PI loop.
- **Balance** means the number of heaters conducting in any one half-cycle
  never exceeds `q + 1`. `C` independent duty-cycle controllers give no such
  bound; they can, and eventually will, align.

```console
$ example_heater_controller --n 50 --weights 25,25,10,40,15 --rounds 2
```

---

## The two hosts

The applications talk to a peripheral through one seam,
`examples/common/example_host.h`, which is two functions wide:

| Host                                  | Used by                    | Talks to                      |
| ------------------------------------- | -------------------------- | ----------------------------- |
| `sim/example_host.cpp`                | `ctest`, `run_examples.sh` | the verilated `rtl/bcmc_wb.v` |
| `examples/common/example_host_mmio.c` | a real target              | memory-mapped registers       |

There is deliberately **no software model of the peripheral**. The simulation
host is a cable to the real RTL, not a stand-in for it: an example that passed
against a fake peripheral would only prove that two pieces of software agree.

---

## No allocation

Neither `sw/` nor `examples/` calls `malloc`, by policy. Every buffer here is a
static or stack array bounded by `EX_MAX_N` and `EX_MAX_C` from
`examples/common/example_config.h`, and every bound is visible in a declaration
rather than argued for in a comment. This is the same rule the observer API
follows: a permutation of `N` indices is storage the caller provides, which is
why it appears in the signature.

---

## Checking the claim

`--summary` prints only the quantities a traversal **cannot** change — per-row
totals, per-column loads in ascending index order, and the load histogram, each
application saying it in its own vocabulary. It withholds the running visit log,
which _is_ the traversal, and it withholds the name of the traversal: a summary
that named its own traversal could never be byte-identical to the other one, and
the check below would be vacuous.

```bash
./scripts/run_examples.sh          # builds, then runs the product
./scripts/run_examples.sh --no-build
```

For each application, over two geometries — one with `r != 0` and one with
`r == 0` — the script asserts two things:

| Comparison                         | Expected   | What it establishes                     |
| ---------------------------------- | ---------- | --------------------------------------- |
| `--summary` sequential vs permuted | identical  | P1–P4: order changed nothing observable |
| running log sequential vs permuted | **differ** | the order really did change             |

The second is the negative control, and it is not decoration. Two identical
summaries prove nothing if the traversals were secretly the same traversal.

The script ends with `== EXAMPLES GREEN (n comparisons)` and is step 11 of
`scripts/run_sim.sh` — the only step that checks a claim no testbench can,
because it is a claim about two programs and not about one.

---

## An example is an application, not an observer

Nothing in this directory is part of the BCMC definition.

> Reference Observers demonstrate ways to consume the BCMC representation. They
> are not part of the BCMC definition and may be replaced or extended without
> affecting the mathematical or hardware contracts of the primitive.

The observers live in `sw/bcmc_observer.{h,c}` and are specified in
`docs/Observers.md`. This directory contributes **no traversal strategy at
all** — adding one here would put a traversal inside an application and destroy
the very orthogonality the directory exists to demonstrate. New strategies go
into the observer library, and every application here acquires them for free,
because none of them knows what a traversal is.

---

## Options common to all three

```text
  --n N                 row length, and the length of one pass
  --weights w0,w1,...   the BCMC context; C is how many there are
  --traversal WHICH     sequential | permuted     (default sequential)
  --seed S              hex or decimal seed for --traversal permuted
  --rounds R            rounds to run             (default 1)
  --summary             print only what a traversal cannot change
  --help                this text
```

`--rounds` is described for the domain in each program's help text — _passes_ for
`gpio_scheduler`, _periods_ for `heater_controller`, _rounds_ for `matrix_dump` —
but the flag is spelled the same in all three, because the loop it controls is
the same loop.

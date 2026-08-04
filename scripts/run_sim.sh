#!/usr/bin/env bash
#===========================================================================
# scripts/run_sim.sh -- the whole pipeline, in the order it depends on
#
#     Proof  ->  Python reference  ->  vectors  ->  RTL  ->  simulators
#
# Nothing downstream is trusted until everything upstream of it has passed.
# In particular the RTL is never compared against numbers written by hand: the
# expected offsets and bits come from validation/reference.py, which is itself
# checked against validation/verify_conjecture.py and against exhaustive
# enumeration.
#
# Both halves of the BCMC definition are covered: bcmc_core (the prefix
# transform) and bcmc_cell (the characteristic function). bcmc_row and
# bcmc_column add no mathematics -- they are replication of the cell, and the
# testbenches prove that by comparing every one of their bits against a
# separately instantiated bcmc_cell.
#
# bcmc_context adds no mathematics either; it is where the representation
# lives. Its testbenches therefore cannot check a formula, so they instantiate
# a real bcmc_core alongside it and check the composition: the offsets that end
# up in the window are the Core's, and the Core's offsets are reference.py's.
#
# The register map has the same shape one level up:
#
#     docs/Register_Map.md  ->  bcmc_periph.py  ->  rtl/bcmc_wb.v
#
# so the peripheral model is validated against the reference vectors here,
# before any bus RTL is compiled, for the same reason the mathematics was.
# The bus suites are then RECORDED from that model rather than written: what
# the simulators replay is a conversation bcmc_periph.py actually had.
#
# The observers are checked in the same place, and for a related reason. They
# are not part of the BCMC definition -- they are ways of consuming it -- so
# validation/test_observers.py proves no new theorem: it proves that a traversal
# contributes order and nothing else. The same matrices, read in a different
# sequence, yield the same events. The balance and conservation properties are
# re-derived from the vector files rather than assumed, so an observer that
# quietly dropped a column would be caught by the mathematics it does not touch.
#
# The driver is the one thing here that is software, so the rule that covers it
# is not "two simulators" -- Verilog simulators do not compile C -- but two
# COMPILERS, at the warning level a bare-metal build would use. It is then run
# against the real bcmc_wb inside the Verilator step, so what is checked is a
# driver talking to a peripheral rather than a driver talking to a model of one.
#
# The last step is the examples, and it is the only one that checks a claim no
# testbench can. Every other step asks "is this component right?"; step 11 asks
# "does it matter which observer the application was given?", and answers it by
# running the same application twice in two orders and diffing the reports. The
# answer must be no, and the diff must be empty.
#
#   ./scripts/run_sim.sh            # everything except the soak files
#   ./scripts/run_sim.sh --big      # ... including the soak files
#   ./scripts/run_sim.sh --quick    # skip the Icarus path
#===========================================================================


set -u -o pipefail

cd "$(dirname "$0")/.." || exit 1

ROOT=$PWD
BIG=0
QUICK=0

for arg in "$@"; do
    case "$arg" in
        --big)   BIG=1 ;;
        --quick) QUICK=1 ;;
        *) echo "usage: $0 [--big] [--quick]"; exit 2 ;;
    esac
done

step() { echo; echo "=============================================================="; \
         echo "== $*"; echo "=============================================================="; }

die() { echo; echo "FAILED: $*"; exit 1; }

#---------------------------------------------------------------------------
step "1/11  the reference model is self-consistent"
#---------------------------------------------------------------------------

python3 validation/reference.py       || die "validation/reference.py"

#---------------------------------------------------------------------------
step "2/11  the reference model agrees with the mathematics"
#---------------------------------------------------------------------------

( cd validation && python3 test_reference.py ) || die "validation/test_reference.py"

#---------------------------------------------------------------------------
step "3/11  regenerate the vectors from the reference model"
#---------------------------------------------------------------------------

if [ "$BIG" -eq 1 ]; then
    ( cd validation && python3 gen_vectors.py --big ) || die "gen_vectors.py --big"
else
    ( cd validation && python3 gen_vectors.py )       || die "gen_vectors.py"
fi

#---------------------------------------------------------------------------
step "4/11  the peripheral model satisfies the register map"
#---------------------------------------------------------------------------

( cd validation && python3 bcmc_periph.py ) || die "validation/bcmc_periph.py"
( cd validation && python3 test_periph.py ) || die "validation/test_periph.py"

#---------------------------------------------------------------------------
step "5/11  the reference observers satisfy the observer contract"
#---------------------------------------------------------------------------

( cd validation && python3 observers.py )      || die "validation/observers.py"
( cd validation && python3 test_observers.py ) || die "validation/test_observers.py"

# The C observer in sw/ is a second implementation of the same traversal, so it
# needs the Python one written down before it can be compared to it. These are
# the permutations themselves, not matrices: sim/bcmc_observer_test.cpp reads
# them so that no second shuffle is ever written in C++.
( cd validation && python3 gen_observer_vectors.py ) || die "gen_observer_vectors.py"

#---------------------------------------------------------------------------
step "6/11  record the bus conversations from the peripheral model"
#---------------------------------------------------------------------------

( cd validation && python3 gen_wb_vectors.py ) || die "gen_wb_vectors.py"

#---------------------------------------------------------------------------
step "7/11  lint the RTL"
#---------------------------------------------------------------------------

./scripts/lint.sh || die "scripts/lint.sh"

#---------------------------------------------------------------------------
step "8/11  sw/ and examples/ are portable C, under every compiler present"
#---------------------------------------------------------------------------

# The driver has no platform, so a compiler is the only thing it needs, and any
# compiler will do. Warnings are errors here: -Wconversion in particular,
# because the register map is narrow fields inside 32-bit words and a silent
# narrowing is exactly the bug sw/bcmc.c must not have.
#
# It is also compiled as C++, which is not idle: a driver whose header cannot
# be included from C++ is a driver half its callers cannot use.
CFLAGS_STRICT="-Wall -Wextra -Wpedantic -Wconversion -Wshadow
               -Wstrict-prototypes -Wmissing-prototypes -Werror"

# sw/bcmc_observer.c travels with the driver and is held to the same standard.
# It is not part of the BCMC definition, but a reference that does not compile
# everywhere is not a reference anyone can pick up.
BCMC_C_UNITS="bcmc bcmc_observer"

# The examples are held to it too. The three applications and the shared layer
# are ordinary C99 with no platform of their own, and the point of the host seam
# is that this is true -- so it is checked, unit by unit, under every compiler on
# the machine. examples/common/example_host_mmio.c is the reason the list is
# worth having: it is the bare-metal host, it is never linked into anything
# here, and compiling it is the only evidence that the seam has two sides.
EX=examples
BCMC_EX_UNITS="$EX/common/example_config.c
               $EX/common/example_traversal.c
               $EX/common/example_host_mmio.c
               $EX/matrix_dump/matrix_dump.c
               $EX/gpio_scheduler/gpio_scheduler.c
               $EX/heater_controller/heater_controller.c"

found_cc=0
for cc in gcc clang tcc; do
    command -v "$cc" >/dev/null 2>&1 || continue
    found_cc=1
    echo "-- $cc -std=c99"
    for unit in $BCMC_C_UNITS; do
        # shellcheck disable=SC2086
        "$cc" -std=c99 $CFLAGS_STRICT -Isw -c sw/$unit.c -o /tmp/${unit}_$cc.o \
            || die "$cc rejected sw/$unit.c"
    done
    for src in $BCMC_EX_UNITS; do
        # shellcheck disable=SC2086
        "$cc" -std=c99 $CFLAGS_STRICT -Isw -I$EX/common -c "$src" \
            -o "/tmp/$(basename "$src" .c)_$cc.o" \
            || die "$cc rejected $src"
    done
done
[ "$found_cc" -eq 1 ] || die "no C compiler found"

for cxx in g++ clang++; do
    command -v "$cxx" >/dev/null 2>&1 || continue
    echo "-- $cxx -std=c++17 (the headers only)"
    for unit in $BCMC_C_UNITS; do
        printf '#include "%s.h"\nint main(void) { return 0; }\n' "$unit" \
            >/tmp/bcmc_hdr.cpp
        "$cxx" -std=c++17 -Wall -Wextra -Werror -Isw /tmp/bcmc_hdr.cpp \
            -o /tmp/bcmc_hdr || die "$cxx rejected sw/$unit.h"
    done
done

#---------------------------------------------------------------------------
step "9/11  Verilator: RTL == Python, and the driver == the register map"
#---------------------------------------------------------------------------

mkdir -p sim/build sim/waves
( cd sim/build && cmake .. >/dev/null && make -j"$(nproc)" >/dev/null ) \
    || die "building the Verilator harness"
( cd sim/build && ctest --output-on-failure ) || die "ctest"

#---------------------------------------------------------------------------
step "10/11  Icarus Verilog: the second opinion"
#---------------------------------------------------------------------------

if [ "$QUICK" -eq 1 ]; then
    echo "skipped (--quick)"
else
    ( cd sim && make test ) || die "sim/make test"
    if [ "$BIG" -eq 1 ]; then
        echo
        echo "-- 10k soak under Icarus (slow) --"
        ( cd sim && make soak ) || die "sim/make soak"
    fi
fi

#---------------------------------------------------------------------------
step "11/11  the observer does not matter to the application"
#---------------------------------------------------------------------------

# Everything above checks a component against a specification. This checks the
# separation between two of them: the three applications are each run under both
# reference traversals, and the parts of their reports that describe the matrix
# rather than the visit order are required to be byte-identical. The visit logs
# are required to differ, so the comparison cannot pass vacuously.
#
# --no-build because step 9 has already built every target in sim/.
./scripts/run_examples.sh --no-build || die "scripts/run_examples.sh"

echo
echo "=============================================================="
echo "== ALL GREEN"
echo "=============================================================="
echo
echo "Waveforms: $ROOT/sim/waves/  (gtkwave sim/waves/bcmc_core.vcd)"

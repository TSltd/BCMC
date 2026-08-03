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
step "1/6  the reference model is self-consistent"
#---------------------------------------------------------------------------

python3 validation/reference.py       || die "validation/reference.py"

#---------------------------------------------------------------------------
step "2/6  the reference model agrees with the mathematics"
#---------------------------------------------------------------------------

( cd validation && python3 test_reference.py ) || die "validation/test_reference.py"

#---------------------------------------------------------------------------
step "3/6  regenerate the vectors from the reference model"
#---------------------------------------------------------------------------

if [ "$BIG" -eq 1 ]; then
    ( cd validation && python3 gen_vectors.py --big ) || die "gen_vectors.py --big"
else
    ( cd validation && python3 gen_vectors.py )       || die "gen_vectors.py"
fi

#---------------------------------------------------------------------------
step "4/6  lint the RTL"
#---------------------------------------------------------------------------

./scripts/lint.sh || die "scripts/lint.sh"

#---------------------------------------------------------------------------
step "5/6  Verilator: RTL == Python"
#---------------------------------------------------------------------------

mkdir -p sim/build sim/waves
( cd sim/build && cmake .. >/dev/null && make -j"$(nproc)" >/dev/null ) \
    || die "building the Verilator harness"
( cd sim/build && ctest --output-on-failure ) || die "ctest"

#---------------------------------------------------------------------------
step "6/6  Icarus Verilog: the second opinion"
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

echo
echo "=============================================================="
echo "== ALL GREEN"
echo "=============================================================="
echo
echo "Waveforms: $ROOT/sim/waves/  (gtkwave sim/waves/bcmc_core.vcd)"

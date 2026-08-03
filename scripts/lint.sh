#!/usr/bin/env bash
#===========================================================================
# scripts/lint.sh -- lint every synthesisable source in rtl/
#
# The RTL is expected to be warning-free, not merely warning-tolerated. A
# warning here is a design question that has not been answered yet.
#
#   ./scripts/lint.sh
#===========================================================================

set -u -o pipefail

cd "$(dirname "$0")/.." || exit 1

VERILATOR=${VERILATOR:-verilator}
IVERILOG=${IVERILOG:-iverilog}

status=0

#---------------------------------------------------------------------------
# Verilator: the strict pass. -Wall includes width, unused and undriven
# checks, which is most of what catches real bugs in small RTL.
#---------------------------------------------------------------------------

# rtl/ still contains empty placeholder files for modules that are deliberately
# not written yet (see rtl/README.md). An empty file is reported, not linted.
if command -v "$VERILATOR" >/dev/null 2>&1; then
    echo "== verilator --lint-only -Wall =="
    for f in rtl/*.v; do
        top=$(basename "$f" .v)
        printf '  %-24s ' "$f"
        if [ ! -s "$f" ]; then
            echo "empty (not written yet)"
            continue
        fi
        if out=$("$VERILATOR" --lint-only -Wall --top-module "$top" "$f" 2>&1); then

            echo "clean"
        else
            echo "WARNINGS"
            echo "$out" | sed 's/^/      /'
            status=1
        fi
    done
else
    echo "== verilator not found, skipping =="
    status=1
fi

#---------------------------------------------------------------------------
# Icarus: a second front end. Different tools object to different things.
#
# -Wno-timescale because the RTL deliberately declares no `timescale: a
# timescale is a property of a simulation, not of hardware.
#---------------------------------------------------------------------------

if command -v "$IVERILOG" >/dev/null 2>&1; then
    echo
    echo "== iverilog -g2005 -Wall (syntax and elaboration only) =="
    for f in rtl/*.v; do
        top=$(basename "$f" .v)
        printf '  %-24s ' "$f"
        if [ ! -s "$f" ]; then
            echo "empty (not written yet)"
            continue
        fi
        if out=$("$IVERILOG" -g2005 -Wall -Wno-timescale -t null \
                             -s "$top" "$f" 2>&1) && [ -z "$out" ]; then

            echo "clean"
        else
            echo "WARNINGS"
            echo "$out" | sed 's/^/      /'
            status=1
        fi
    done
else
    echo
    echo "== iverilog not found, skipping =="
fi

echo
if [ "$status" -eq 0 ]; then
    echo "LINT CLEAN"
else
    echo "LINT FAILED"
fi
exit "$status"

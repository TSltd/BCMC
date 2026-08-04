#!/usr/bin/env bash
#===========================================================================
# scripts/run_examples.sh -- the Application x Traversal product, as a diff
#
# The examples exist to demonstrate one claim:
#
#     an observer contributes ORDER, and nothing else.
#
# ctest already runs every application under every traversal and fails on the
# word MISMATCH, so each process checks itself. But a process only ever ran one
# traversal, so no single process can compare two. That is this script's job,
# and it is the only place the claim is actually tested rather than asserted.
#
# HOW IT TESTS IT
#
# Every application takes --summary, which prints only the facts that do not
# depend on pi: which columns exist, what each contains, how often each row was
# activated, the load histogram, the peak simultaneous load. The traversal name
# is deliberately withheld from a summary, so the outputs are comparable by
# construction. Then:
#
#     application X under traversal A   --summary
#     application X under traversal B   --summary
#     diff
#
# Any difference at all is a failure. Which means:
#
#   P1 coverage      -- a traversal that dropped or repeated a column would move
#                       an activation count
#   P2 conservation   -- a traversal that changed how often a row fires would
#                       move the per-row totals
#   P3 balance        -- a traversal that changed the load multiset would move
#                       the histogram
#   P4 equivalence    -- and the diff being empty IS P4
#
# The running logs, by contrast, must differ -- if they did not, the permuted
# traversal would not be permuting anything -- so the script checks that too.
# A test that would pass against a broken observer is worse than no test.
#
# WHY IT COMPARES PROGRAM OUTPUT AND NOT INTERNAL STATE
#
# Because a user's confidence comes from the program, not from the library. The
# strongest form this claim can take is that two runs of the same controller,
# scheduling the same load on the same hardware in two different orders, produce
# byte-identical reports. That is checkable by anyone with a diff.
#
#   ./scripts/run_examples.sh            # build if needed, then check
#   ./scripts/run_examples.sh --no-build # use the binaries already there
#===========================================================================

set -u -o pipefail

cd "$(dirname "$0")/.." || exit 1

BUILD=1

for arg in "$@"; do
    case "$arg" in
        --no-build) BUILD=0 ;;
        *) echo "usage: $0 [--no-build]"; exit 2 ;;
    esac
done

die() { echo; echo "FAILED: $*"; exit 1; }

APPS="matrix_dump gpio_scheduler heater_controller"
TRAVERSALS="sequential permuted"

# Two geometries, deliberately different in the one way that matters to the
# Balance Theorem: the first has W = qN + r with r > 0, so both q and q+1 appear
# in the load histogram; the second divides exactly, so every column carries the
# same load. A bug that only showed up when the matrix was perfectly regular --
# or only when it was not -- would be caught by exactly one of these.
#
#   N = 12, weights 5,3,7,1,4   W = 20 = 1*12 + 8   loads {1,2}
#   N =  8, weights 4,4,4,4     W = 16 = 2*8  + 0   loads {2}
GEOMETRIES=(
    "--n 12 --weights 5,3,7,1,4"
    "--n 8 --weights 4,4,4,4"
)

BUILDDIR=sim/build
OUT=$(mktemp -d) || die "mktemp"
trap 'rm -rf "$OUT"' EXIT

if [ "$BUILD" -eq 1 ]; then
    echo "-- building the examples"
    mkdir -p "$BUILDDIR"
    ( cd "$BUILDDIR" && cmake .. >/dev/null ) || die "cmake"
    for app in $APPS; do
        ( cd "$BUILDDIR" && make -j"$(nproc)" "example_$app" >/dev/null ) \
            || die "building example_$app"
    done
fi

for app in $APPS; do
    [ -x "$BUILDDIR/example_$app" ] || die "$BUILDDIR/example_$app is not built"
done

checks=0

for gi in "${!GEOMETRIES[@]}"; do
    geom=${GEOMETRIES[$gi]}
    echo
    echo "=============================================================="
    echo "== geometry $gi: $geom"
    echo "=============================================================="

    for app in $APPS; do

        # --- the part that must not change --------------------------------
        for t in $TRAVERSALS; do
            # shellcheck disable=SC2086
            "./$BUILDDIR/example_$app" $geom --traversal "$t" --rounds 3 \
                --summary >"$OUT/${app}_${gi}_${t}.sum" 2>"$OUT/${app}_${gi}_${t}.err" \
                || die "example_$app --traversal $t exited nonzero"

            # An application reports its own failures in words, and the report
            # is on stdout, so grep the summary rather than trust the status.
            if grep -q MISMATCH "$OUT/${app}_${gi}_${t}.sum"; then
                echo
                cat "$OUT/${app}_${gi}_${t}.sum"
                die "example_$app under $t reported a MISMATCH"
            fi
            if [ -s "$OUT/${app}_${gi}_${t}.err" ]; then
                echo
                cat "$OUT/${app}_${gi}_${t}.err"
                die "example_$app under $t wrote to stderr"
            fi
        done

        ref=sequential
        for t in $TRAVERSALS; do
            [ "$t" = "$ref" ] && continue
            if ! diff -u "$OUT/${app}_${gi}_${ref}.sum" \
                         "$OUT/${app}_${gi}_${t}.sum" >"$OUT/d"; then
                echo
                cat "$OUT/d"
                die "$app: $ref and $t disagree about a property of the matrix"
            fi
            echo "-- $app: $ref == $t  (summaries byte-identical)"
            checks=$((checks + 1))
        done

        # --- the part that must change ------------------------------------
        #
        # The negative control. If the two running logs were identical then the
        # comparison above would be vacuous: an observer that ignored its
        # permutation would pass every test in this file. So the visit order is
        # required to differ, and this is the one place in the repository where
        # two outputs being the same is the failure.
        for t in $TRAVERSALS; do
            # shellcheck disable=SC2086
            "./$BUILDDIR/example_$app" $geom --traversal "$t" --rounds 1 \
                >"$OUT/${app}_${gi}_${t}.log" 2>/dev/null \
                || die "example_$app --traversal $t exited nonzero"
        done

        for t in $TRAVERSALS; do
            [ "$t" = "$ref" ] && continue
            if diff -q "$OUT/${app}_${gi}_${ref}.log" \
                       "$OUT/${app}_${gi}_${t}.log" >/dev/null; then
                die "$app: the $t log is identical to the $ref log -- the observer is not permuting"
            fi
            echo "-- $app: $ref != $t  (visit order differs, as it must)"
            checks=$((checks + 1))
        done
    done
done

[ "$checks" -gt 0 ] || die "no comparisons were made"

echo
echo "=============================================================="
echo "== EXAMPLES GREEN  ($checks comparisons)"
echo "=============================================================="
echo
echo "Same matrix, same driver, same RTL, different order:"
echo "the reports agree and the logs do not."

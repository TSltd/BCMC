#!/usr/bin/env bash
#===========================================================================
# scripts/format.sh -- whitespace hygiene, and formatting where a formatter
# is actually available
#
#   ./scripts/format.sh            # report problems, change nothing
#   ./scripts/format.sh --fix      # fix what can be fixed safely
#
# There is deliberately no house Verilog formatter imposed here. Reformatting
# RTL churns diffs and hides real changes, and the layout of these files
# carries meaning: the datapath is written to read like the recurrence it
# implements. What is enforced is only what is objectively wrong -- tabs,
# trailing whitespace, missing final newlines, over-long lines.
#
# If verible-verilog-format is installed it is used for Verilog, and
# clang-format for C++, but only when --fix is given.
#===========================================================================

set -u -o pipefail

cd "$(dirname "$0")/.." || exit 1

FIX=0
[ "${1:-}" = "--fix" ] && FIX=1

MAXLEN=100

# Files whose layout is ours to police. Makefiles need tabs, so they are out.
mapfile -t FILES < <(
    ls rtl/*.v rtl/*.vh sim/*.v sim/*.cpp sim/common/*.cpp sim/common/*.h \
       validation/reference.py validation/test_reference.py \
       validation/gen_vectors.py 2>/dev/null
)

status=0

report() { printf '  %-32s %s\n' "$1" "$2"; status=1; }

echo "== whitespace =="
for f in "${FILES[@]}"; do
    [ -f "$f" ] || continue

    if grep -qP '\t' "$f"; then
        report "$f" "contains tab characters"
        [ "$FIX" -eq 1 ] && sed -i 's/\t/    /g' "$f"
    fi

    if grep -qP '[ \t]+$' "$f"; then
        report "$f" "trailing whitespace"
        [ "$FIX" -eq 1 ] && sed -i 's/[ \t]*$//' "$f"
    fi

    if [ -s "$f" ] && [ "$(tail -c 1 "$f" | wc -l)" -eq 0 ]; then
        report "$f" "no newline at end of file"
        [ "$FIX" -eq 1 ] && printf '\n' >>"$f"
    fi

    long=$(awk -v n="$MAXLEN" 'length($0) > n {c++} END {print c+0}' "$f")
    if [ "$long" -gt 0 ]; then
        report "$f" "$long line(s) longer than $MAXLEN columns"
    fi
done
[ "$status" -eq 0 ] && echo "  clean"

#---------------------------------------------------------------------------
# Optional formatters
#---------------------------------------------------------------------------

echo
echo "== formatters =="

if command -v verible-verilog-format >/dev/null 2>&1; then
    if [ "$FIX" -eq 1 ]; then
        echo "  verible-verilog-format: formatting rtl/ and sim/"
        verible-verilog-format --inplace rtl/*.v sim/*.v
    else
        echo "  verible-verilog-format: available (use --fix to apply)"
    fi
else
    echo "  verible-verilog-format: not installed, skipping"
fi

if command -v clang-format >/dev/null 2>&1; then
    if [ "$FIX" -eq 1 ]; then
        echo "  clang-format: formatting sim/"
        clang-format -i sim/*.cpp sim/common/*.cpp sim/common/*.h
    else
        echo "  clang-format: available (use --fix to apply)"
    fi
else
    echo "  clang-format: not installed, skipping"
fi

echo
if [ "$status" -eq 0 ]; then
    echo "FORMAT CLEAN"
elif [ "$FIX" -eq 1 ]; then
    echo "FIXED what could be fixed; re-run to confirm"
else
    echo "FORMAT ISSUES (re-run with --fix)"
fi

# Long lines are advisory, so --fix always reports success.
[ "$FIX" -eq 1 ] && exit 0
exit "$status"

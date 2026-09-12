#!/usr/bin/env bash
#
# mutate_bank_isolation.sh -- the negative control for section 5.5's invariant.
#
# The property is: a bank is only ever filled while it is EMPTY, so **the fill can
# never write the bank the read path is using**. Nothing mathematical can check it
# -- a half-advanced in-place shuffle is still a permutation, so O1 sees nothing
# wrong -- which is why the module carries a structural assertion and why that
# assertion has to be shown to work rather than assumed to.
#
# This script builds a *mutated* copy in a temporary directory, in which a shuffle
# write addresses the bank being READ instead of the bank being filled. The
# mutation is exactly the mistake the invariant exists to catch: the bank selector
# is still right, so the first assertion form is satisfied, and only the
# address form catches it.
#
# It then requires the run to FAIL. A mutated build that passes would mean the
# invariant is not verified by anything, and the mutation would be invisible until
# it corrupted a traversal in the field.
#
# It is NOT part of `make test`: it deliberately builds something that must fail,
# and it says so with its own PASS line for the runner's grep.
#
set -u

root=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

src="$root/rtl/bcmc_src_shuffled.v"
mut="$work/bcmc_src_shuffled.v"

# The mutation: the write address takes its bank from playing_q.
sed 's/f_iaddr = {fill_bank_q,/f_iaddr = {playing_q,/' "$src" > "$mut"

if cmp -s "$src" "$mut"; then
    echo "FAIL mutate_bank_isolation: the mutation did not apply."
    echo "     The write address has changed shape -- update this script, and"
    echo "     check that the invariant is still expressed by an assertion."
    exit 1
fi

if ! command -v iverilog >/dev/null 2>&1; then
    echo "SKIP mutate_bank_isolation: no iverilog"
    exit 0
fi

if ! iverilog -g2005 -Wall -Wno-timescale -s tb_src_shuffled \
        -o "$work/tb.vvp" "$root/sim/tb_src_shuffled.v" "$mut" >"$work/build.log" 2>&1
then
    echo "FAIL mutate_bank_isolation: the mutated build does not elaborate:"
    sed -n '1,6p' "$work/build.log"
    exit 1
fi

out=$(cd "$root/sim" && vvp "$work/tb.vvp" +vectors=vectors/srcshuf_edge.txt \
        2>&1 </dev/null)

if echo "$out" | grep -q 'tb_src_shuffled: PASS'; then
    echo "FAIL mutate_bank_isolation: the mutated build PASSED."
    echo "     The bank-isolation invariant is not verified by anything: a fill"
    echo "     may write the bank being read and nothing notices."
    exit 1
fi

# The assertion should be what stopped it, not an incidental mismatch.
if echo "$out" | grep -q 'bcmc_src_shuffled: ERROR'; then
    echo "  the structural assertion fired:"
    echo "$out" | grep -A1 'bcmc_src_shuffled: ERROR' | head -2 | sed 's/^/    /'
else
    echo "  NOTE: the run failed, but not with the module's own assertion --"
    echo "        the corpus caught it. That is a weaker result: it means the"
    echo "        structural check did not fire."
fi

echo "mutate_bank_isolation: PASS  the mutation is detected"

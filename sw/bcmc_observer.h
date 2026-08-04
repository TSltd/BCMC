//===========================================================================
// bcmc_observer.h -- traversal, and nothing but traversal
//
// docs/Observers.md is the specification; validation/observers.py is its
// reference model; this is the C restatement. Where the three disagree, the
// document is right, the model is the tiebreak for anything the document left
// as an algorithm, and this file is the bug.
//
// WHAT AN OBSERVER IS
//
// A finite sequence of visits. A visit is a column index j; the result of
// visiting j is R(j), the rows active in that column, in ascending order. A
// pass is exactly N visits. So an observer is completely described by
//
//     pi : { 0 .. N-1 }  ->  { 0 .. N-1 },     pi(t) = the column at step t
//
// and everything below is either a way of building pi, a way of stepping
// through it, or a way of decoding what a step returned.
//
// WHAT AN OBSERVER IS NOT
//
// Observers are not part of the BCMC definition. They demonstrate ways to
// consume the representation and may be replaced or extended without affecting
// the mathematical or hardware contracts of the primitive. The formal content
// of that sentence is P4 in docs/Observers.md -- any two conforming observers
// emit the same events and differ only in order -- and it is the reason there
// is no privileged traversal anywhere below this line.
//
// In particular this file contains NO BCMC MATHEMATICS. It never computes an
// offset, never evaluates M(i, j), and never decides whether a bit is set: it
// asks rtl/bcmc_wb.v through sw/bcmc.h and reports the answer. The only
// arithmetic here is indexing, shuffling and popcount, none of which is a
// statement about BCMC.
//
// PRIMITIVES, THEN COMPOSITION
//
// The same shape as sw/bcmc.h, for the same reason.
//
//   * the generator and the bounded draw are pure functions of their state;
//   * an order builder fills a caller's buffer and touches no bus;
//   * a decode turns COLUMN words into row indices and touches no bus;
//   * the cursor is the only thing here that performs a bus access, and it
//     performs exactly one composition -- bcmc_read_column() -- per step.
//
// NO ALLOCATION
//
// sw/ has no allocator, by policy, so nothing here has hidden storage. A
// permutation of N indices is N words, and those N words appear in the
// signature of the function that fills them and in the signature of the cursor
// that walks them. The caller owns them for the lifetime of the pass.
//
// THREE BUFFERS, AND WHY EACH IS THE CALLER'S
//
//     order[N]        the traversal, pi, as an array
//     words[ceil(MAX_C/32)]   one column, as a bitmap  (bcmc_column_words())
//     rows[...]       the decoded ascending row indices, if wanted at all
//
// A consumer that only needs the bitmap never provides the third; a consumer
// that walks sequentially can pass a null order and never provides the first.
//
//===========================================================================

#ifndef BCMC_SW_BCMC_OBSERVER_H
#define BCMC_SW_BCMC_OBSERVER_H

#include <stdbool.h>
#include <stdint.h>

#include "bcmc.h"

#ifdef __cplusplus
extern "C" {
#endif

//---------------------------------------------------------------------------
// The random source
//
// A 32-bit SplitMix, pinned exactly by docs/Observers.md. It is not here
// because observers are random -- O3 forbids that -- but because "a shuffle"
// is not a specification: two implementations of Fisher-Yates disagree unless
// the generator and the loop direction are written down. This one is written
// down, so validation/observers.py and this file are obliged to produce the
// same permutation from the same seed, and sim/bcmc_observer_test.cpp checks
// that index for index.
//
// It has no forbidden seeds, including zero, and needs no 64-bit arithmetic.
// It is emphatically not a cryptographic generator and must not be used as one.
//---------------------------------------------------------------------------

typedef struct {
    uint32_t state;
} bcmc_rng_t;

// state <- seed. Total: every 32-bit value is a legal seed.
void bcmc_rng_seed(bcmc_rng_t *rng, uint32_t seed);

// One step of the generator. All arithmetic modulo 2^32, all shifts logical.
uint32_t bcmc_rng_next(bcmc_rng_t *rng);

// Uniform on 0 .. m INCLUSIVE, by rejection on a power-of-two mask. Rejection
// rather than next() % (m + 1), because the modulo is biased and, more to the
// point, because the bias is not specified by anything and the rejection loop
// is specified by five lines of docs/Observers.md.
//
// Terminates with probability 1 and in expectation in under two draws, since
// the mask is the smallest 2^k - 1 that is at least m.
uint32_t bcmc_rng_uniform(bcmc_rng_t *rng, uint32_t m);

//---------------------------------------------------------------------------
// Building pi -- no bus access, no allocation
//
// Both fill order[0 .. n-1] with a permutation of 0 .. n-1. Both are pure
// functions of their declared inputs, which is O3.
//---------------------------------------------------------------------------

// Reference observer 1: pi(t) = t. The control case, and the cheapest possible
// consumer -- see bcmc_observer_init_sequential(), which needs no buffer at
// all. This function exists for callers that want the identity as data, for
// instance to perturb it.
bcmc_status_t bcmc_order_sequential(uint32_t *order, uint32_t n);

// Reference observer 2: a seeded Fisher-Yates shuffle of the identity,
// downward, with j drawn from 0 .. i inclusive. j == i is a legal outcome and
// means the element stays put; excluding it would give a different and
// non-uniform distribution, and would silently disagree with the Python
// reference.
bcmc_status_t bcmc_order_permuted(uint32_t *order, uint32_t n, uint32_t seed);

// O1, discharged on demand. An order this returns false for is not a
// traversal, and a pass over it would visit some column twice and some column
// never.
//
// The cursor does NOT call this. Checking a bijection needs a bitmap of n
// bits, and a driver that allocated one would be breaking the rule above, so
// the scratch appears here in the signature: nscratch must be at least
// (n + 31) / 32 words. Its contents on return are unspecified.
//
// A caller that built its order with the two functions above does not need
// this. A caller that wrote its own does.
bool bcmc_order_is_bijection(const uint32_t *order, uint32_t n, uint32_t *scratch,
                             uint32_t nscratch);

//---------------------------------------------------------------------------
// Decoding a visit -- no bus access, no allocation
//
// bcmc_read_column() answers with a bitmap: row r in bit r % 32 of word r / 32.
// These two turn that into the two things a consumer actually wants, and
// neither needs to know C: the register map specifies that rows at or above C
// read as zero, so the bits above C are already absent rather than stale.
//---------------------------------------------------------------------------

// L(j): the occupancy of the visited column. This is the quantity the Balance
// Theorem is about, so it is worth having as a name rather than as a loop in
// every caller. It is a popcount and no more; the balance is the matrix's doing.
uint32_t bcmc_column_load(const uint32_t *words, uint32_t nwords);

// R(j): the active rows, ascending, into rows[0 .. *nrows - 1].
//
// Ascending order is fixed by docs/Observers.md so that two conforming
// observers with the same pi produce byte-identical output. It is a convention
// -- the mathematics knows only the set -- but it is a convention this file is
// bound by.
//
// BCMC_ERANGE if the column holds more than max_rows rows, and in that case
// *nrows is not written: a partially decoded column is indistinguishable from
// a lighter one, and silently truncating R(j) would break O2.
bcmc_status_t bcmc_column_rows(const uint32_t *words, uint32_t nwords, uint32_t *rows,
                               uint32_t max_rows, uint32_t *nrows);

//---------------------------------------------------------------------------
// The pass
//
// A cursor over pi. Its entire state is the device, the order, N and the step
// counter -- there is no cached column, no cached matrix and no cached status,
// because an observer must not hold the matrix. M is evaluated on demand by
// the peripheral from (weights[], offsets[]), so there is nothing here that
// could go stale and nothing here that would have to answer for coherence.
//
// `order` is borrowed, not copied. It must outlive the cursor, and it must not
// change during a pass -- doing so would violate O1 mid-traversal, and this
// header has no way to notice.
//---------------------------------------------------------------------------

typedef struct {
    bcmc_dev_t     *dev;    // never null after a successful init
    const uint32_t *order;  // borrowed; null means the identity traversal
    uint32_t        n;      // N: the length of a pass
    uint32_t        t;      // the next step, 0 .. n
} bcmc_observer_t;

// A pass over an arbitrary conforming order. `order` must be a bijection of
// 0 .. n-1 and must remain valid until the pass ends; O1 is the caller's
// obligation, and bcmc_order_is_bijection() is how to discharge it.
//
// `dev` must have been probed. No bus access.
bcmc_status_t bcmc_observer_init(bcmc_observer_t *ob, bcmc_dev_t *dev,
                                 const uint32_t *order, uint32_t n);

// A pass over pi(t) = t, with no order buffer at all. Identical in every
// respect to bcmc_observer_init() with the array bcmc_order_sequential()
// would have written -- the array is simply not worth storing when the
// function that reads it is `t`.
bcmc_status_t bcmc_observer_init_sequential(bcmc_observer_t *ob, bcmc_dev_t *dev,
                                            uint32_t n);

// True once every column has been visited exactly once. There is no way to
// visit N + 1 times: bcmc_observer_next() refuses past the end rather than
// wrapping, because a second lap is a second pass and the caller should say so.
bool bcmc_observer_at_end(const bcmc_observer_t *ob);

// Back to step 0. The order is unchanged, so this is the same pass again --
// which is what makes a permuted observer reproducible without re-shuffling.
void bcmc_observer_rewind(bcmc_observer_t *ob);

// One visit.
//
// Writes the column index to *col and the column bitmap to words[], then
// advances. `nwords` must be at least bcmc_column_words(dev), exactly as
// bcmc_read_column() requires and for the same reason.
//
// Costs whatever bcmc_read_column() costs and not one access more: two index
// writes and ceil(MAX_C / 32) reads. The cursor adds no traffic of its own,
// and sim/bcmc_observer_test.cpp counts accesses to keep it that way.
//
// BCMC_ENOTREADY at the end of a pass -- not an error, but not a visit either,
// so it is distinguishable from a column that happens to be empty. Anything
// the peripheral refuses is reported unchanged: an observer knows nothing
// about bus errors beyond the status codes the driver hands it.
bcmc_status_t bcmc_observer_next(bcmc_observer_t *ob, uint32_t *col, uint32_t *words,
                                 uint32_t nwords);

// The column pi(t) would visit at step `t`, without visiting it and without a
// bus access. Useful for planning a schedule ahead of consuming it; not part
// of a pass, and it does not advance the cursor.
bcmc_status_t bcmc_observer_peek(const bcmc_observer_t *ob, uint32_t t, uint32_t *col);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // BCMC_SW_BCMC_OBSERVER_H

//===========================================================================
// bcmc_observer.c -- the reference observers
//
// Read this file next to docs/Observers.md, because it is a transcription of
// it and nothing else. The specification pins the generator, the bounded draw
// and the shuffle line by line, precisely so that this file and
// validation/observers.py are obliged to produce the same permutation from the
// same seed. sim/bcmc_observer_test.cpp checks that index for index against
// vectors the Python model wrote.
//
// WHAT THIS FILE IS NOT ALLOWED TO CONTAIN
//
//   no offset arithmetic      bcmc_core.v computes offsets
//   no evaluation of M(i,j)   bcmc_cell.v evaluates cells
//   no raw bus access         sw/bcmc.h is the only way out of here
//   no allocation             every buffer arrives in a signature
//   no cached matrix          M is evaluated on demand; there is nothing to stale
//
// The arithmetic that IS here -- a mixing function, a rejection loop, a swap
// and a popcount -- is about ORDER and about DECODING, and neither is a
// statement about BCMC. That is the whole content of "balance is a property of
// the matrix, smoothness is a property of the observer": nothing below can
// create balance the matrix lacks, and nothing below can destroy balance it has.
//===========================================================================

#include "bcmc_observer.h"

//---------------------------------------------------------------------------
// The random source
//
// docs/Observers.md, "The random source", transcribed. Every constant and
// every shift is from that block; none of them may be tuned, because
// validation/observers.py contains the same five lines and the two must agree.
//---------------------------------------------------------------------------

void bcmc_rng_seed(bcmc_rng_t *rng, uint32_t seed)
{
    if (rng == NULL) {
        return;
    }
    rng->state = seed;
}

uint32_t bcmc_rng_next(bcmc_rng_t *rng)
{
    uint32_t z;

    if (rng == NULL) {
        return 0u;
    }

    rng->state += 0x9E3779B9u;
    z = rng->state;
    z = (z ^ (z >> 16)) * 0x21F0AAADu;
    z = (z ^ (z >> 15)) * 0x735A2D97u;
    z = z ^ (z >> 15);
    return z;
}

uint32_t bcmc_rng_uniform(bcmc_rng_t *rng, uint32_t m)
{
    uint32_t mask;
    uint32_t x;

    if (rng == NULL) {
        return 0u;
    }
    // The rejection loop must not be entered for m == 0: there is exactly one
    // value to return and no bits to draw.
    if (m == 0u) {
        return 0u;
    }

    // The smallest (2^k - 1) that is at least m, by smearing every set bit
    // downwards. No loop over k, no log, no floating point.
    mask = m;
    mask |= mask >> 1;
    mask |= mask >> 2;
    mask |= mask >> 4;
    mask |= mask >> 8;
    mask |= mask >> 16;

    // Rejection, not modulo. Since mask < 2*m + 1, at least half of the draws
    // are accepted, so this terminates with probability 1 and in expectation
    // in under two iterations.
    for (;;) {
        x = bcmc_rng_next(rng) & mask;
        if (x <= m) {
            return x;
        }
    }
}

//---------------------------------------------------------------------------
// Building pi
//
// N >= 1 is the hypothesis of the Balance Theorem, inherited rather than
// weakened: a pass is exactly N visits, and there is no such thing as a pass
// over no columns. It is a range refusal and costs no bus access, because an
// observer that has been handed N already knows it -- the same rule sw/bcmc.h
// applies to geometry.
//---------------------------------------------------------------------------

bcmc_status_t bcmc_order_sequential(uint32_t *order, uint32_t n)
{
    uint32_t t;

    if (order == NULL) {
        return BCMC_EINVAL;
    }
    if (n == 0u) {
        return BCMC_ERANGE;
    }

    for (t = 0u; t < n; t++) {
        order[t] = t;
    }
    return BCMC_OK;
}

bcmc_status_t bcmc_order_permuted(uint32_t *order, uint32_t n, uint32_t seed)
{
    bcmc_rng_t    rng;
    bcmc_status_t s;
    uint32_t      i;
    uint32_t      j;
    uint32_t      tmp;

    // The identity first, so that the shuffle below is exactly the four lines
    // of docs/Observers.md and not five.
    s = bcmc_order_sequential(order, n);
    if (s != BCMC_OK) {
        return s;
    }

    bcmc_rng_seed(&rng, seed);

    // Downward, drawing j from 0 .. i INCLUSIVE. j == i is legal and means the
    // element stays put. Drawing from 0 .. n-1 instead would still produce a
    // bijection -- so O1 would not notice -- but the distribution would not be
    // uniform, and it would silently disagree with the Python reference.
    // validation/test_observers.py runs that exact bug as a negative control.
    for (i = n - 1u; i >= 1u; i--) {
        j        = bcmc_rng_uniform(&rng, i);
        tmp      = order[i];
        order[i] = order[j];
        order[j] = tmp;
    }
    return BCMC_OK;
}

bool bcmc_order_is_bijection(const uint32_t *order, uint32_t n, uint32_t *scratch,
                             uint32_t nscratch)
{
    uint32_t need;
    uint32_t t;
    uint32_t v;

    if (order == NULL || scratch == NULL || n == 0u) {
        return false;
    }

    need = (n + 31u) / 32u;
    if (nscratch < need) {
        return false;
    }

    for (t = 0u; t < need; t++) {
        scratch[t] = 0u;
    }

    // O1 is two claims -- in range, and each exactly once -- and the seen
    // bitmap decides both in one pass. n entries hitting n distinct values
    // below n is a bijection; there is nothing further to check.
    for (t = 0u; t < n; t++) {
        v = order[t];
        if (v >= n) {
            return false;
        }
        if ((scratch[v / 32u] & (1u << (v % 32u))) != 0u) {
            return false;
        }
        scratch[v / 32u] |= (uint32_t)(1u << (v % 32u));
    }
    return true;
}

//---------------------------------------------------------------------------
// Decoding a visit
//
// Neither of these knows C, and neither needs to: docs/Register_Map.md
// specifies that COLUMN bits at or above C read as zero, so a bit that is set
// is a row that exists. Reintroducing C here would be caching a piece of the
// peripheral's state, which is exactly what sw/bcmc.h refuses to do.
//---------------------------------------------------------------------------

uint32_t bcmc_column_load(const uint32_t *words, uint32_t nwords)
{
    uint32_t count = 0u;
    uint32_t k;
    uint32_t w;

    if (words == NULL) {
        return 0u;
    }

    for (k = 0u; k < nwords; k++) {
        // Kernighan: one iteration per set bit, no table, no builtin. L(j) is
        // at most MAX_C, so this is bounded by the geometry.
        for (w = words[k]; w != 0u; w &= (w - 1u)) {
            count++;
        }
    }
    return count;
}

bcmc_status_t bcmc_column_rows(const uint32_t *words, uint32_t nwords, uint32_t *rows,
                               uint32_t max_rows, uint32_t *nrows)
{
    uint32_t count;
    uint32_t k;
    uint32_t b;
    uint32_t at = 0u;

    if (words == NULL || nrows == NULL) {
        return BCMC_EINVAL;
    }
    if (rows == NULL && max_rows != 0u) {
        return BCMC_EINVAL;
    }

    // Counted before anything is written, so a column that does not fit leaves
    // the caller's buffer alone. A half-decoded R(j) is indistinguishable from
    // a lighter column, and handing one back would break O2 silently.
    count = bcmc_column_load(words, nwords);
    if (count > max_rows) {
        return BCMC_ERANGE;
    }

    // Ascending row order, fixed by docs/Observers.md so that two conforming
    // observers with the same pi produce byte-identical output. Word k holds
    // rows 32k .. 32k+31, bit b holding row 32k + b, so scanning words then
    // bits in increasing order is already ascending.
    for (k = 0u; k < nwords; k++) {
        for (b = 0u; b < 32u; b++) {
            if ((words[k] & (1u << b)) != 0u) {
                rows[at] = (k * 32u) + b;
                at++;
            }
        }
    }

    *nrows = at;
    return BCMC_OK;
}

//---------------------------------------------------------------------------
// The pass
//
// The cursor is four fields and no cleverness. Everything it could have cached
// -- the column, the matrix, the last status, C -- belongs to the peripheral,
// which is the only thing that can be right about it.
//---------------------------------------------------------------------------

bcmc_status_t bcmc_observer_init(bcmc_observer_t *ob, bcmc_dev_t *dev,
                                 const uint32_t *order, uint32_t n)
{
    if (ob == NULL || dev == NULL || order == NULL) {
        return BCMC_EINVAL;
    }
    // Probed, because a visit is a bcmc_read_column() and that needs the
    // geometry. Failing here rather than at the first step means a caller
    // learns about it before it has begun a pass it cannot finish.
    if (!dev->probed) {
        return BCMC_EINVAL;
    }
    if (n == 0u) {
        return BCMC_ERANGE;
    }

    // O1 is NOT checked here. It cannot be without a bitmap of n bits, and
    // this file does not allocate. bcmc_order_is_bijection() is the caller's
    // way to discharge it, with the scratch in the signature where it belongs.
    ob->dev   = dev;
    ob->order = order;
    ob->n     = n;
    ob->t     = 0u;
    return BCMC_OK;
}

bcmc_status_t bcmc_observer_init_sequential(bcmc_observer_t *ob, bcmc_dev_t *dev,
                                            uint32_t n)
{
    if (ob == NULL || dev == NULL) {
        return BCMC_EINVAL;
    }
    if (!dev->probed) {
        return BCMC_EINVAL;
    }
    if (n == 0u) {
        return BCMC_ERANGE;
    }

    // A null order means pi(t) = t. The identity is the one permutation whose
    // array is pure redundancy, so the cheapest possible consumer of this
    // peripheral is a counter and no storage whatsoever.
    ob->dev   = dev;
    ob->order = NULL;
    ob->n     = n;
    ob->t     = 0u;
    return BCMC_OK;
}

bool bcmc_observer_at_end(const bcmc_observer_t *ob)
{
    if (ob == NULL) {
        return true;
    }
    return ob->t >= ob->n;
}

void bcmc_observer_rewind(bcmc_observer_t *ob)
{
    if (ob == NULL) {
        return;
    }
    ob->t = 0u;
}

bcmc_status_t bcmc_observer_next(bcmc_observer_t *ob, uint32_t *col, uint32_t *words,
                                 uint32_t nwords)
{
    bcmc_status_t s;
    uint32_t      j;

    if (ob == NULL || ob->dev == NULL || col == NULL || words == NULL) {
        return BCMC_EINVAL;
    }
    // A pass is exactly N visits. Refusing rather than wrapping is what makes
    // O1 observable from outside: an N+1st visit would be a second visit to
    // some column, and no conforming observer has one.
    if (ob->t >= ob->n) {
        return BCMC_ENOTREADY;
    }

    j    = (ob->order != NULL) ? ob->order[ob->t] : ob->t;
    *col = j;

    // The one bus access in this file, and it is a call rather than an access:
    // the observer does not know an address, a wire or an error code that
    // sw/bcmc.h did not hand it. Whatever the peripheral refuses is reported
    // unchanged, with the cursor left where it was -- a refused visit is not a
    // visit, so retrying resumes the pass rather than skipping a column.
    s = bcmc_read_column(ob->dev, j, words, nwords);
    if (s != BCMC_OK) {
        return s;
    }

    ob->t++;
    return BCMC_OK;
}

bcmc_status_t bcmc_observer_peek(const bcmc_observer_t *ob, uint32_t t, uint32_t *col)
{
    if (ob == NULL || col == NULL) {
        return BCMC_EINVAL;
    }
    if (t >= ob->n) {
        return BCMC_ERANGE;
    }

    *col = (ob->order != NULL) ? ob->order[t] : t;
    return BCMC_OK;
}

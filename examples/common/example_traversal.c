//===========================================================================
// example_traversal.c -- see example_traversal.h
//
// Note what is absent: any loop over indices, any arithmetic on a column
// number, any random number. This file selects; sw/bcmc_observer.c supplies.
//===========================================================================

#include "example_traversal.h"

#include <stdio.h>
#include <string.h>

bool ex_traversal_init(ex_traversal_t *tr, const char *name, uint32_t seed,
                       uint32_t *order, uint32_t norder)
{
    if (tr == NULL || name == NULL) {
        return false;
    }

    if (strcmp(name, "sequential") == 0) {
        tr->kind = EX_TRAVERSAL_SEQUENTIAL;
    } else if (strcmp(name, "permuted") == 0) {
        tr->kind = EX_TRAVERSAL_PERMUTED;
    } else {
        // Not a silent fallback. A misspelled traversal that quietly became
        // the sequential one would make every claim in this tree unfalsifiable
        // -- two "different" traversals would agree because they were the same.
        (void)fprintf(stderr,
                      "unknown traversal '%s'; the reference traversals are "
                      "'sequential' and 'permuted'\n",
                      name);
        return false;
    }

    tr->seed   = seed;
    tr->order  = order;
    tr->norder = norder;
    return true;
}

bcmc_status_t ex_traversal_begin(const ex_traversal_t *tr, bcmc_observer_t *ob,
                                 bcmc_dev_t *dev, uint32_t n)
{
    bcmc_status_t st;

    if (tr == NULL || ob == NULL || dev == NULL) {
        return BCMC_EINVAL;
    }

    switch (tr->kind) {
    case EX_TRAVERSAL_SEQUENTIAL:
        // No buffer, because pi(t) = t needs no storage to be read from. The
        // application cannot tell that this branch was cheaper than the other.
        return bcmc_observer_init_sequential(ob, dev, n);

    case EX_TRAVERSAL_PERMUTED:
        if (tr->order == NULL || tr->norder < n) {
            return BCMC_ERANGE;
        }
        st = bcmc_order_permuted(tr->order, n, tr->seed);
        if (st != BCMC_OK) {
            return st;
        }
        return bcmc_observer_init(ob, dev, tr->order, n);

    default:
        return BCMC_EINVAL;
    }
}

const char *ex_traversal_name(const ex_traversal_t *tr)
{
    if (tr == NULL) {
        return "none";
    }
    return (tr->kind == EX_TRAVERSAL_PERMUTED) ? "permuted" : "sequential";
}

//===========================================================================
// example_traversal.h -- choosing a traversal, which is not the same as having
//                        one
//
// This is the only file in examples/ that names a traversal, and it is 60 lines
// of switch statement. It implements nothing: sw/bcmc_observer.h supplies the
// traversals, docs/Observers.md specifies them, and this header exists so that
// an application can be given one without ever mentioning which.
//
// WHY THE APPLICATIONS DO NOT DO THIS THEMSELVES
//
// Each application in this tree wants the same two lines:
//
//     ex_traversal_begin(&tr, &ob, &dev, n);
//     while (bcmc_observer_next(&ob, &col, words, nwords) == BCMC_OK) { ... }
//
// If instead each application chose between bcmc_observer_init_sequential()
// and bcmc_order_permuted() itself, then every application would contain a
// small amount of traversal policy, and "the application does not know how the
// matrix is traversed" would be false in three places at once. Concentrating
// the choice here makes the claim structural: grep for bcmc_order_ in
// examples/ and this file is the only hit.
//
// APPLICATION x TRAVERSAL IS A PRODUCT
//
// Three applications and two traversals are six programs, and no cell of that
// table needed writing. Adding a third traversal -- a stride, a Gray code, a
// priority order, anything satisfying O1 -- means adding one case below and
// nothing else anywhere. Adding a fourth application means using this header
// and nothing else anywhere. That independence is what v0.5c is for, and
// scripts/run_examples.sh checks it by running the whole product and diffing
// what must not have changed.
//
// NO ALLOCATION HERE EITHER
//
// A permuted traversal needs N words to hold pi. sw/ has no allocator by
// policy, and neither does this tree: the buffer appears in ex_traversal_init()
// as the caller's array, exactly as it appears in bcmc_order_permuted(). A
// sequential traversal needs no buffer at all, and passing one is not an error
// -- it simply goes unused, which is the honest way to make the two
// interchangeable.
//
//===========================================================================

#ifndef BCMC_EXAMPLES_EXAMPLE_TRAVERSAL_H
#define BCMC_EXAMPLES_EXAMPLE_TRAVERSAL_H

#include <stdbool.h>
#include <stdint.h>

#include "bcmc.h"
#include "bcmc_observer.h"

#ifdef __cplusplus
extern "C" {
#endif

// The reference traversals of docs/Observers.md, and nothing else. This enum
// is deliberately not open-ended: a traversal that is not in sw/ is not one
// this tree can select, because examples/ is where BCMC is consumed and not
// where it is extended.
typedef enum {
    EX_TRAVERSAL_SEQUENTIAL = 0,  // pi(t) = t
    EX_TRAVERSAL_PERMUTED         // pi = a seeded Fisher-Yates shuffle
} ex_traversal_kind_t;

typedef struct {
    ex_traversal_kind_t kind;
    uint32_t            seed;    // ignored unless kind is EX_TRAVERSAL_PERMUTED
    uint32_t           *order;   // borrowed; may be null for SEQUENTIAL
    uint32_t            norder;  // words available in `order`
} ex_traversal_t;

// Maps a name -- "sequential" or "permuted" -- onto a traversal. Returns false
// for anything else, with a message on stderr, so that a typo on the command
// line is not silently a sequential pass.
//
// `order` and `norder` are the caller's buffer for pi. It must be at least N
// words for a permuted traversal and is not touched here: pi is built in
// ex_traversal_begin(), where N is known.
bool ex_traversal_init(ex_traversal_t *tr, const char *name, uint32_t seed,
                       uint32_t *order, uint32_t norder);

// Builds pi if the traversal needs one, then starts a pass over it. No bus
// access: sw/bcmc_observer.h promises that building an order and starting a
// pass are free, and sim/bcmc_observer_test.cpp counts accesses to keep it so.
//
// BCMC_ERANGE if a permuted traversal was given fewer than n words to work in.
bcmc_status_t ex_traversal_begin(const ex_traversal_t *tr, bcmc_observer_t *ob,
                                 bcmc_dev_t *dev, uint32_t n);

// For logging. Never null.
const char *ex_traversal_name(const ex_traversal_t *tr);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // BCMC_EXAMPLES_EXAMPLE_TRAVERSAL_H

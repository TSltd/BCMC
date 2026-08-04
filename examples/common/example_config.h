//===========================================================================
// example_config.h -- the command line the three applications share
//
// Every application here needs the same four things: a BCMC context to
// program (N and the weights), a traversal to consume it with, a number of
// rounds to run, and a choice between a running log and a summary. Writing
// that parser three times would have been three chances to make the three
// applications subtly incomparable, and scripts/run_examples.sh compares them.
//
// WHY --summary EXISTS
//
// It is the difference between what an observer changes and what it must not.
// The running log is a sequence of visits, so it depends on pi and is supposed
// to: that is the whole observable effect of choosing a traversal. The summary
// is everything that does not depend on pi -- which columns exist, what each
// one holds, how often each row was activated -- and it is required to be
// byte-for-byte identical under every traversal. That is P2, P3 and P4 of
// docs/Observers.md, restated as a diff over program output.
//
// GEOMETRY IS THE PART'S, AND THE HYPOTHESIS IS THE CALLER'S
//
// The bounds below are this tree's buffers, not BCMC's limits. The real limit
// on C is the peripheral's MAX_C, discovered from CAPS by bcmc_probe(), and the
// applications check against that rather than against EX_MAX_C. The one BCMC
// rule enforced here is 0 <= w_i <= N -- the hypothesis of Lemma 2, which is
// why the hardware needs no divider. A caller that breaks it is not asking for
// a BCMC matrix, so it is refused here rather than at the bus.
//
//===========================================================================

#ifndef BCMC_EXAMPLES_EXAMPLE_CONFIG_H
#define BCMC_EXAMPLES_EXAMPLE_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

// Buffers in this tree, and nothing more fundamental than that. EX_MAX_C is
// the widest context these examples will hold; EX_MAX_N is the longest pass
// they will record for a summary.
#define EX_MAX_C 64u
#define EX_MAX_N 128u

typedef struct {
    uint32_t    n;                  // N: the row length, and the length of a pass
    uint32_t    c;                  // C: rows, i.e. how many weights follow
    uint32_t    weights[EX_MAX_C];  // w_0 .. w_{c-1}, each 0 <= w_i <= n
    const char *traversal;          // "sequential" or "permuted"
    uint32_t    seed;               // for the permuted traversal only
    uint32_t    rounds;             // passes, periods, or whatever the app calls them
    bool        summary;            // print only what a traversal cannot change
} ex_config_t;

// N = 12, C = 5, weights 5 3 7 1 4, so W = 20 = 1*12 + 8: eight columns of
// load 2 and four of load 1. Small enough to read by eye and unbalanced enough
// that a bug in the construction would be visible in the dump.
void ex_config_defaults(ex_config_t *cfg);

// Parses argv into `cfg`, starting from the defaults. Returns false on a bad
// option, on a value outside the buffers above, or on a weight exceeding N,
// having already explained itself on stderr. `--help` prints the usage and
// returns false with *helped set, so main() can exit 0 rather than 2.
bool ex_config_parse(ex_config_t *cfg, int argc, char **argv, bool *helped);

// The options this header understands, and what the shared ones mean. `rounds`
// is named by the application, because "a round" is the one thing here that is
// application-specific: a pass, a mains period, a tick budget.
void ex_config_usage(const char *prog, const char *rounds_name,
                     const char *extra_lines);

// One line describing the context, so that every log in this tree says what it
// was looking at. W and the balance figures q and r are printed because they
// are what the Balance Theorem predicts, and having them beside the observed
// loads is the point of the exercise.
void ex_config_print(const ex_config_t *cfg, const char *rounds_name);

#endif  // BCMC_EXAMPLES_EXAMPLE_CONFIG_H

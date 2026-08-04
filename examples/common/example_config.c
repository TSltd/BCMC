//===========================================================================
// example_config.c -- see example_config.h
//
// Plain C99 argument handling, no getopt: getopt_long is a GNU extension and
// these programs are meant to compile for a target that has no getopt at all.
//===========================================================================

#include "example_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//---------------------------------------------------------------------------
// One unsigned number, strictly. strtoul accepts leading spaces and a sign,
// neither of which belongs on the command line of a scheduler, so the whole
// token is required to have been consumed and to have been digits.
//---------------------------------------------------------------------------

static bool parse_u32(const char *s, int base, uint32_t *out)
{
    char         *end = NULL;
    unsigned long v;

    if (s == NULL || *s == '\0' || *s == '-' || *s == '+') {
        return false;
    }
    v = strtoul(s, &end, base);
    if (end == NULL || *end != '\0' || v > 0xFFFFFFFFul) {
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

// "5,3,7,1,4" -> weights[0..4], c = 5. A trailing or doubled comma is an
// error rather than a zero weight: a zero weight is legal and meaningful (an
// inactive row), so it has to be written down as one.
static bool parse_weights(const char *s, ex_config_t *cfg)
{
    uint32_t count = 0;

    while (*s != '\0') {
        char     tok[16];
        uint32_t k = 0;

        while (*s != '\0' && *s != ',') {
            if (k + 1u >= sizeof tok) {
                (void)fprintf(stderr, "--weights: '%s' is not a number\n", s);
                return false;
            }
            tok[k++] = *s++;
        }
        tok[k] = '\0';

        if (count >= EX_MAX_C) {
            (void)fprintf(stderr, "--weights: at most %lu weights\n",
                          (unsigned long)EX_MAX_C);
            return false;
        }
        if (!parse_u32(tok, 10, &cfg->weights[count])) {
            (void)fprintf(stderr, "--weights: '%s' is not a number\n", tok);
            return false;
        }
        count++;

        if (*s == ',') {
            s++;
            if (*s == '\0') {
                (void)fprintf(stderr, "--weights: trailing comma\n");
                return false;
            }
        }
    }

    if (count == 0u) {
        (void)fprintf(stderr, "--weights: at least one weight is required\n");
        return false;
    }
    cfg->c = count;
    return true;
}

//---------------------------------------------------------------------------

void ex_config_defaults(ex_config_t *cfg)
{
    uint32_t i;

    if (cfg == NULL) {
        return;
    }
    for (i = 0; i < EX_MAX_C; i++) {
        cfg->weights[i] = 0;
    }
    cfg->n          = 12u;
    cfg->c          = 5u;
    cfg->weights[0] = 5u;
    cfg->weights[1] = 3u;
    cfg->weights[2] = 7u;
    cfg->weights[3] = 1u;
    cfg->weights[4] = 4u;
    cfg->traversal  = "sequential";
    cfg->seed       = 0x9E3779B9u;
    cfg->rounds     = 1u;
    cfg->summary    = false;
}

void ex_config_usage(const char *prog, const char *rounds_name, const char *extra_lines)
{
    (void)printf("usage: %s [options]\n"
                 "\n"
                 "  --n N                 row length, and the length of one pass\n"
                 "  --weights w0,w1,...   the BCMC context; C is how many there are\n"
                 "  --traversal WHICH     sequential | permuted     (default "
                 "sequential)\n"
                 "  --seed S              hex or decimal seed for --traversal "
                 "permuted\n"
                 "  --rounds R            %s to run                 (default 1)\n"
                 "  --summary             print only what a traversal cannot change\n"
                 "  --help                this text\n",
                 (prog != NULL) ? prog : "example",
                 (rounds_name != NULL) ? rounds_name : "rounds");
    if (extra_lines != NULL) {
        (void)fputs(extra_lines, stdout);
    }
}

bool ex_config_parse(ex_config_t *cfg, int argc, char **argv, bool *helped)
{
    int i;

    if (cfg == NULL || argv == NULL) {
        return false;
    }
    if (helped != NULL) {
        *helped = false;
    }
    ex_config_defaults(cfg);

    for (i = 1; i < argc; i++) {
        const char *a    = argv[i];
        const char *next = ((i + 1) < argc) ? argv[i + 1] : NULL;

        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            if (helped != NULL) {
                *helped = true;
            }
            return false;
        }
        if (strcmp(a, "--summary") == 0) {
            cfg->summary = true;
            continue;
        }

        if (next == NULL) {
            (void)fprintf(stderr, "%s needs a value\n", a);
            return false;
        }

        if (strcmp(a, "--n") == 0) {
            if (!parse_u32(next, 10, &cfg->n)) {
                (void)fprintf(stderr, "--n: '%s' is not a number\n", next);
                return false;
            }
        } else if (strcmp(a, "--weights") == 0) {
            if (!parse_weights(next, cfg)) {
                return false;
            }
        } else if (strcmp(a, "--traversal") == 0) {
            cfg->traversal = next;
        } else if (strcmp(a, "--seed") == 0) {
            // Hex is the natural way to write a seed, and decimal is the
            // natural way to write a small one, so base 0 accepts both.
            if (!parse_u32(next, 0, &cfg->seed)) {
                (void)fprintf(stderr, "--seed: '%s' is not a number\n", next);
                return false;
            }
        } else if (strcmp(a, "--rounds") == 0) {
            if (!parse_u32(next, 10, &cfg->rounds)) {
                (void)fprintf(stderr, "--rounds: '%s' is not a number\n", next);
                return false;
            }
        } else {
            (void)fprintf(stderr, "unknown option '%s' (try --help)\n", a);
            return false;
        }
        i++;
    }

    //-----------------------------------------------------------------------
    // What is checked here, and what deliberately is not
    //
    // The buffer bounds are this tree's; the weight bound is the mathematics.
    // C against the peripheral's MAX_C is NOT checked here, because this file
    // has no peripheral -- that is bcmc_probe()'s answer, and the applications
    // ask it after opening the host.
    //-----------------------------------------------------------------------

    if (cfg->n == 0u || cfg->n > EX_MAX_N) {
        (void)fprintf(stderr, "N must be 1 .. %lu\n", (unsigned long)EX_MAX_N);
        return false;
    }
    if (cfg->c == 0u || cfg->c > EX_MAX_C) {
        (void)fprintf(stderr, "C must be 1 .. %lu\n", (unsigned long)EX_MAX_C);
        return false;
    }
    if (cfg->rounds == 0u) {
        (void)fprintf(stderr, "--rounds must be at least 1\n");
        return false;
    }
    {
        uint32_t k;
        for (k = 0; k < cfg->c; k++) {
            if (cfg->weights[k] > cfg->n) {
                // Lemma 2 assumes 0 <= w_i <= N, and the hardware relies on it:
                // that hypothesis is why `mod N` is a compare and a subtract
                // rather than a divider. Outside it there is no BCMC matrix to
                // observe, so there is nothing for an application to do.
                (void)fprintf(stderr,
                              "weight[%lu] = %lu exceeds N = %lu; BCMC requires "
                              "0 <= w_i <= N\n",
                              (unsigned long)k, (unsigned long)cfg->weights[k],
                              (unsigned long)cfg->n);
                return false;
            }
        }
    }
    return true;
}

void ex_config_print(const ex_config_t *cfg, const char *rounds_name)
{
    uint32_t total = 0;
    uint32_t k;

    if (cfg == NULL) {
        return;
    }
    for (k = 0; k < cfg->c; k++) {
        total += cfg->weights[k];
    }

    (void)printf("N = %lu  C = %lu  W = %lu  weights =",
                 (unsigned long)cfg->n, (unsigned long)cfg->c, (unsigned long)total);
    for (k = 0; k < cfg->c; k++) {
        (void)printf(" %lu", (unsigned long)cfg->weights[k]);
    }
    (void)printf("\n");

    // The Balance Theorem, stated before anything is observed, so that the
    // loads printed below can be read against a prediction rather than merely
    // admired. Nothing in this tree computes a matrix bit; this is arithmetic
    // on W and N, which is the theorem's statement and not its proof.
    (void)printf("balance: W = %lu*N + %lu, so %lu columns of load %lu and %lu of "
                 "load %lu\n",
                 (unsigned long)(total / cfg->n), (unsigned long)(total % cfg->n),
                 (unsigned long)(total % cfg->n), (unsigned long)(total / cfg->n + 1u),
                 (unsigned long)(cfg->n - (total % cfg->n)),
                 (unsigned long)(total / cfg->n));

    // The one line that names the traversal is withheld from a summary, and
    // for the reason the summary exists: scripts/run_examples.sh diffs summaries
    // across traversals, so a summary that said which traversal produced it
    // could never be identical to the other one and the check would be
    // vacuous. What a traversal cannot change is printed; what it is, is not.
    if (!cfg->summary) {
        (void)printf("traversal = %s", cfg->traversal);
        if (strcmp(cfg->traversal, "permuted") == 0) {
            (void)printf(" (seed 0x%08lX)", (unsigned long)cfg->seed);
        }
        (void)printf("  %s = %lu\n", (rounds_name != NULL) ? rounds_name : "rounds",
                     (unsigned long)cfg->rounds);
    } else {
        (void)printf("%s = %lu\n", (rounds_name != NULL) ? rounds_name : "rounds",
                     (unsigned long)cfg->rounds);
    }
}


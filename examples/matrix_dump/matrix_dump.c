//===========================================================================
// matrix_dump -- the application that does nothing but look
//
// This is the smallest useful thing an application can be: it visits every
// column exactly once, and prints what it found. There is no actuator, no
// deadline and no policy, so nothing here can hide a traversal dependency
// behind application behaviour. If the summary of a permuted run ever differs
// from the summary of a sequential run, the difference is in BCMC or in the
// observer, and not in this file.
//
// WHAT IT PRINTS, AND WHY IN TWO FORMS
//
// The running log is per-visit: step t, the column index j the observer handed
// over, its load L(j), and the rows R(j). It is deliberately order-dependent --
// that sequence *is* the traversal, and reading two logs side by side is how a
// human sees that the two observers disagree about order and about nothing else.
//
// The summary is per-column, printed in ascending column order regardless of
// the order of arrival, followed by the per-row activation counts and the load
// histogram. Every number in it is a property of the matrix (docs/BCMC.md),
// so it must come out byte-identical under every traversal:
//
//   P1 coverage        -- N columns visited, none twice
//   P2 row conservation -- row i activated exactly w_i times in a pass
//   P3 balance          -- the load histogram matches W = qN + r
//   P4 equivalence      -- and all of the above, whichever observer was used
//
// scripts/run_examples.sh diffs those summaries. This program's job is to make
// that diff meaningful, which it does by storing what it sees rather than
// printing it as it arrives.
//
// NO ALLOCATION
//
// The record below is a static array sized by the buffer bounds in
// example_config.h. sw/ has no allocator by policy, and an example that needed
// one to consume a BCMC matrix would be quietly arguing that the policy is
// unrealistic. It is not: the record of a pass is bounded by N and C, both of
// which are known before the pass starts.
//
//===========================================================================

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bcmc.h"
#include "bcmc_observer.h"
#include "example_config.h"
#include "example_host.h"
#include "example_traversal.h"

// The widest column bitmap these buffers will hold, in 32-bit words.
#define EX_MAX_WORDS ((EX_MAX_C + 31u) / 32u)

// The record of one pass. Indexed by column, not by step, which is the whole
// point: the arrival order is forgotten on purpose.
static uint8_t  g_rows[EX_MAX_N][EX_MAX_C];  // rows of column j, ascending
static uint8_t  g_nrows[EX_MAX_N];           // L(j), as counted from the bitmap
static uint8_t  g_seen[EX_MAX_N];            // how many times column j arrived
static uint32_t g_acts[EX_MAX_C];            // activations of row i over the run

static void usage(const char *prog) {
    ex_config_usage(prog, "passes",
                    "  This application only reads. --passes > 1 simply repeats\n"
                    "  the pass, which is a cheap check that rewinding an\n"
                    "  observer restores it exactly.\n");
}

int main(int argc, char **argv) {
    ex_config_t     cfg;
    ex_traversal_t  tr;
    bcmc_dev_t      dev;
    bcmc_observer_t ob;
    bcmc_status_t   st;
    uint32_t        order[EX_MAX_N];
    uint32_t        words[EX_MAX_WORDS];
    uint32_t        nwords;
    uint32_t        round;
    uint32_t        i;
    uint32_t        j;
    bool            helped = false;

    ex_config_defaults(&cfg);
    if (!ex_config_parse(&cfg, argc, argv, &helped)) {
        if (helped) {
            usage(argv[0]);
            return 0;
        }
        return 2;
    }

    // The peripheral is reached through the host seam, so this same source file
    // runs against the verilated bcmc_wb under sim/ and against real silicon on
    // a board. It is never linked against a software model of BCMC.
    st = example_host_open(&dev);
    if (st != BCMC_OK) {
        fprintf(stderr, "matrix_dump: cannot open host: %s\n", bcmc_strstatus(st));
        return 1;
    }

    // C is the part's answer, not ours. EX_MAX_C bounds this program's arrays;
    // dev.max_c bounds the hardware, and it is the one that decides.
    if (cfg.c > dev.max_c) {
        fprintf(stderr, "matrix_dump: C = %lu exceeds the part's MAX_C = %lu\n",
                (unsigned long)cfg.c, (unsigned long)dev.max_c);
        example_host_close();
        return 1;
    }

    nwords = bcmc_column_words(&dev);
    if (nwords > EX_MAX_WORDS) {
        fprintf(stderr, "matrix_dump: the part's columns need %lu words\n",
                (unsigned long)nwords);
        example_host_close();
        return 1;
    }

    st = bcmc_load(&dev, cfg.weights, cfg.n, cfg.c);
    if (st != BCMC_OK) {
        fprintf(stderr, "matrix_dump: load refused: %s\n", bcmc_strstatus(st));
        example_host_close();
        return 1;
    }

    // The application names a traversal here and never again. Everything below
    // this line talks to bcmc_observer_next() and cannot tell which one it got.
    if (!ex_traversal_init(&tr, cfg.traversal, cfg.seed, order, EX_MAX_N)) {
        example_host_close();
        return 2;
    }
    st = ex_traversal_begin(&tr, &ob, &dev, cfg.n);
    if (st != BCMC_OK) {
        fprintf(stderr, "matrix_dump: traversal refused: %s\n", bcmc_strstatus(st));
        example_host_close();
        return 1;
    }

    memset(g_rows, 0, sizeof g_rows);
    memset(g_nrows, 0, sizeof g_nrows);
    memset(g_seen, 0, sizeof g_seen);
    memset(g_acts, 0, sizeof g_acts);

    printf("matrix_dump\n");
    ex_config_print(&cfg, "passes");

    for (round = 0; round < cfg.rounds; round++) {
        // Rewinding is how a pass is repeated. The observer is a cursor over a
        // table, so rewinding is free and cannot re-order anything.
        bcmc_observer_rewind(&ob);

        if (!cfg.summary && cfg.rounds > 1u) {
            printf("pass %lu\n", (unsigned long)round);
        }

        while (!bcmc_observer_at_end(&ob)) {
            uint32_t col = 0;
            uint32_t rows[EX_MAX_C];
            uint32_t nrows_here = 0;
            uint32_t step        = ob.t;

            st = bcmc_observer_next(&ob, &col, words, nwords);
            if (st != BCMC_OK) {
                fprintf(stderr, "matrix_dump: visit failed: %s\n", bcmc_strstatus(st));
                example_host_close();
                return 1;
            }

            st = bcmc_column_rows(words, nwords, rows, EX_MAX_C, &nrows_here);
            if (st != BCMC_OK) {
                fprintf(stderr, "matrix_dump: column %lu has too many rows: %s\n",
                        (unsigned long)col, bcmc_strstatus(st));
                example_host_close();
                return 1;
            }

            // Record before printing, so the summary is a property of the pass
            // and not of the order the lines came out in.
            g_seen[col]  = (uint8_t)(g_seen[col] + 1u);
            g_nrows[col] = (uint8_t)nrows_here;
            for (i = 0; i < nrows_here; i++) {
                g_rows[col][i] = (uint8_t)rows[i];
                g_acts[rows[i]]++;
            }

            if (!cfg.summary) {
                printf("  step %2lu: column %2lu  load %lu  rows:",
                       (unsigned long)step, (unsigned long)col,
                       (unsigned long)nrows_here);
                for (i = 0; i < nrows_here; i++) {
                    printf(" %lu", (unsigned long)rows[i]);
                }
                printf("\n");
            }
        }
    }

    // ---------------------------------------------------------------------
    // The order-independent part. Nothing below reads `ob`, `tr` or the arrival
    // order; it reads only what the matrix contained.
    // ---------------------------------------------------------------------

    printf("columns\n");
    for (j = 0; j < cfg.n; j++) {
        printf("  column %2lu: visits %lu  load %lu  rows:", (unsigned long)j,
               (unsigned long)g_seen[j], (unsigned long)g_nrows[j]);
        for (i = 0; i < g_nrows[j]; i++) {
            printf(" %lu", (unsigned long)g_rows[j][i]);
        }
        printf("\n");
    }

    printf("rows\n");
    for (i = 0; i < cfg.c; i++) {
        // P2: over `rounds` passes a row must have been activated exactly
        // rounds * w_i times, because a row is a cyclic run of w_i ones.
        unsigned long want = (unsigned long)cfg.rounds * (unsigned long)cfg.weights[i];
        printf("  row %2lu: weight %lu  activations %lu  %s\n", (unsigned long)i,
               (unsigned long)cfg.weights[i], (unsigned long)g_acts[i],
               ((unsigned long)g_acts[i] == want) ? "ok" : "MISMATCH");
    }

    printf("loads\n");
    {
        // The histogram is L(j) counted over the distinct columns, which is the
        // multiset the Balance Theorem constrains to {q, q+1}.
        uint32_t hist[EX_MAX_C + 1u];
        uint32_t maxload = 0;
        uint32_t total   = 0;

        for (i = 0; i <= EX_MAX_C; i++) {
            hist[i] = 0;
        }
        for (j = 0; j < cfg.n; j++) {
            hist[g_nrows[j]]++;
            if (g_nrows[j] > maxload) {
                maxload = g_nrows[j];
            }
            total += g_nrows[j];
        }
        for (i = 0; i <= maxload; i++) {
            if (hist[i] != 0u) {
                printf("  load %lu: %lu columns\n", (unsigned long)i,
                       (unsigned long)hist[i]);
            }
        }
        printf("  total ones = %lu\n", (unsigned long)total);
    }

    example_host_close();
    return 0;
}

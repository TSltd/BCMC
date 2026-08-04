//===========================================================================
// heater_controller -- C heaters on one mains supply, driven by zero-crossings
//
// This is the application the primitive was invented for. A mains period gives
// N half-cycles; there are C heaters; heater i has been asked for w_i of those
// half-cycles, which is its demanded power. A zero-cross interrupt arrives, the
// controller takes the next BCMC column, and energises exactly the heaters in
// R(j) for that half-cycle. Nothing is computed at interrupt time: no
// accumulators, no error terms, no comparisons. The schedule was decided when
// the weights were written, and the half-cycle handler is a read and a store.
//
//   on zero-cross:  visit -> triacs = column
//
// WHY BALANCE IS THE WHOLE POINT
//
// Row conservation makes the power exact: heater i is energised w_i times per
// period however the half-cycles are ordered, so its duty cycle is w_i/N with
// no drift and no long-run averaging. Balance makes the installation cheap: the
// number of heaters conducting in any one half-cycle is q or q+1 where
// W = qN + r, so peak current is bounded by construction. The alternative --
// C independent duty-cycle controllers -- gets the average right and lets the
// peak be anything up to C, which is what oversized supplies and nuisance
// breaker trips are made of. This program prints the observed peak beside q+1
// so the bound can be seen holding.
//
// WHY THE TRAVERSAL IS A COMMAND-LINE OPTION AND CHANGES NOTHING BELOW
//
// Which half-cycle serves which column is a scheduling choice, not a control
// choice. Sequential order is the obvious one. A deterministic permuted order
// is useful in the field: it spreads any interaction with a periodic
// disturbance -- another load on the same phase, a compressor, a second BCMC
// installation -- without touching power or peak, because those are properties
// of the matrix and not of the order it is read in. The controller cannot tell
// which it is being given, and its per-period totals are identical either way.
// That is the claim scripts/run_examples.sh checks by diffing --summary output.
//
// The "triacs" here are a variable. On a board they are a port write in the
// zero-cross ISR, and the substitution is one function.
//
//===========================================================================

#include <stdio.h>
#include <string.h>

#include "bcmc.h"
#include "bcmc_observer.h"
#include "example_config.h"
#include "example_host.h"
#include "example_traversal.h"

#define EX_MAX_WORDS ((EX_MAX_C + 31u) / 32u)

// The switch bank: bit i conducts heater i for the current half-cycle. As in
// gpio_scheduler, the column bitmap is already the value the hardware wants.
static uint32_t g_triacs[EX_MAX_WORDS];

static uint32_t g_on[EX_MAX_C];      // half-cycles delivered to heater i
static uint8_t  g_conc[EX_MAX_N];    // heaters conducting in the half-cycle for column j
static uint8_t  g_served[EX_MAX_N];  // half-cycles that served column j

static void triacs_write(const uint32_t *words, uint32_t nwords) {
    uint32_t k;
    for (k = 0; k < nwords; k++) {
        g_triacs[k] = words[k];
    }
}

static void triacs_off(void) {
    memset(g_triacs, 0, sizeof g_triacs);
}

static void usage(const char *prog) {
    ex_config_usage(prog, "periods",
                    "  N is the number of mains half-cycles in a control period\n"
                    "  (100 for a 50 Hz second), C the number of heaters, and\n"
                    "  weight[i] the half-cycles heater i has been asked for.\n");
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
    uint32_t        period;
    uint32_t        i;
    uint32_t        j;
    uint32_t        peak   = 0;
    bool            helped = false;

    ex_config_defaults(&cfg);
    if (!ex_config_parse(&cfg, argc, argv, &helped)) {
        if (helped) {
            usage(argv[0]);
            return 0;
        }
        return 2;
    }

    st = example_host_open(&dev);
    if (st != BCMC_OK) {
        fprintf(stderr, "heater_controller: cannot open host: %s\n", bcmc_strstatus(st));
        return 1;
    }

    // How many heaters this part can schedule is the part's business, reported
    // through CAPS. The controller asks rather than assumes.
    if (cfg.c > dev.max_c) {
        fprintf(stderr, "heater_controller: %lu heaters exceeds the part's MAX_C = %lu\n",
                (unsigned long)cfg.c, (unsigned long)dev.max_c);
        example_host_close();
        return 1;
    }

    nwords = bcmc_column_words(&dev);
    if (nwords > EX_MAX_WORDS) {
        fprintf(stderr, "heater_controller: the part's columns need %lu words\n",
                (unsigned long)nwords);
        example_host_close();
        return 1;
    }

    // Writing the demanded powers is the only control decision in the program.
    st = bcmc_load(&dev, cfg.weights, cfg.n, cfg.c);
    if (st != BCMC_OK) {
        fprintf(stderr, "heater_controller: load refused: %s\n", bcmc_strstatus(st));
        example_host_close();
        return 1;
    }

    if (!ex_traversal_init(&tr, cfg.traversal, cfg.seed, order, EX_MAX_N)) {
        example_host_close();
        return 2;
    }
    st = ex_traversal_begin(&tr, &ob, &dev, cfg.n);
    if (st != BCMC_OK) {
        fprintf(stderr, "heater_controller: traversal refused: %s\n", bcmc_strstatus(st));
        example_host_close();
        return 1;
    }

    triacs_off();
    memset(g_on, 0, sizeof g_on);
    memset(g_conc, 0, sizeof g_conc);
    memset(g_served, 0, sizeof g_served);

    printf("heater_controller\n");
    ex_config_print(&cfg, "periods");
    printf("heaters = %lu, half-cycles per period = %lu\n", (unsigned long)cfg.c,
           (unsigned long)cfg.n);

    for (period = 0; period < cfg.rounds; period++) {
        // A control period is one pass over the matrix. Rewinding at the top of
        // the period is the whole of the periodic bookkeeping.
        bcmc_observer_rewind(&ob);

        if (!cfg.summary && cfg.rounds > 1u) {
            printf("period %lu\n", (unsigned long)period);
        }

        while (!bcmc_observer_at_end(&ob)) {
            uint32_t col = 0;
            uint32_t rows[EX_MAX_C];
            uint32_t nconc = 0;
            uint32_t hc    = ob.t;

            // --- the zero-cross handler ---------------------------------
            st = bcmc_observer_next(&ob, &col, words, nwords);
            if (st != BCMC_OK) {
                fprintf(stderr, "heater_controller: half-cycle failed: %s\n",
                        bcmc_strstatus(st));
                example_host_close();
                return 1;
            }
            triacs_write(words, nwords);
            // --- end of handler ------------------------------------------

            st = bcmc_column_rows(words, nwords, rows, EX_MAX_C, &nconc);
            if (st != BCMC_OK) {
                fprintf(stderr, "heater_controller: column %lu has too many rows: %s\n",
                        (unsigned long)col, bcmc_strstatus(st));
                example_host_close();
                return 1;
            }

            g_served[col] = (uint8_t)(g_served[col] + 1u);
            g_conc[col]   = (uint8_t)nconc;
            for (i = 0; i < nconc; i++) {
                g_on[rows[i]]++;
            }
            if (nconc > peak) {
                peak = nconc;
            }

            if (!cfg.summary) {
                printf("  half-cycle %3lu: column %2lu  conducting %lu:",
                       (unsigned long)hc, (unsigned long)col, (unsigned long)nconc);
                for (i = 0; i < nconc; i++) {
                    printf(" h%lu", (unsigned long)rows[i]);
                }
                printf("\n");
            }
        }
    }

    // Leave the bank open at the end of the run.
    triacs_off();

    // ---------------------------------------------------------------------
    // Order-independent from here down: power, coverage and peak current are
    // properties of the matrix, so these figures must be identical under any
    // traversal. That is what --summary prints and what run_examples.sh diffs.
    // ---------------------------------------------------------------------

    printf("heaters\n");
    for (i = 0; i < cfg.c; i++) {
        unsigned long want = (unsigned long)cfg.rounds * (unsigned long)cfg.weights[i];
        // Duty in tenths of a percent, so the line is exact integer text and
        // still readable as a power setting.
        unsigned long duty = (cfg.n != 0u) ? ((unsigned long)cfg.weights[i] * 1000ul +
                                              (unsigned long)cfg.n / 2ul) /
                                                 (unsigned long)cfg.n
                                           : 0ul;
        printf("  h%-2lu: demanded %lu/%lu (%lu.%lu%%)  delivered %lu  %s\n",
               (unsigned long)i, (unsigned long)cfg.weights[i], (unsigned long)cfg.n,
               duty / 10ul, duty % 10ul, (unsigned long)g_on[i],
               ((unsigned long)g_on[i] == want) ? "exact" : "MISMATCH");
    }

    printf("half-cycles\n");
    {
        uint32_t missing = 0;
        uint32_t repeated = 0;

        for (j = 0; j < cfg.n; j++) {
            if (g_served[j] == 0u) {
                missing++;
            } else if ((uint32_t)g_served[j] != cfg.rounds) {
                repeated++;
            }
        }
        // P1: a period is a bijection onto the N columns. A controller that
        // dropped or repeated a half-cycle would be delivering the wrong power
        // even with a perfectly balanced matrix, so it is worth stating.
        printf("  columns served %lu of %lu, unserved %lu, mis-served %lu  %s\n",
               (unsigned long)(cfg.n - missing), (unsigned long)cfg.n,
               (unsigned long)missing, (unsigned long)repeated,
               (missing == 0u && repeated == 0u) ? "ok" : "MISMATCH");
    }

    printf("current\n");
    {
        uint32_t hist[EX_MAX_C + 1u];
        uint32_t w = 0;
        uint32_t q = 0;
        uint32_t r = 0;
        uint32_t bound;

        for (i = 0; i <= EX_MAX_C; i++) {
            hist[i] = 0;
        }
        for (i = 0; i < cfg.c; i++) {
            w += cfg.weights[i];
        }
        if (cfg.n != 0u) {
            q = w / cfg.n;
            r = w % cfg.n;
        }
        bound = (r != 0u) ? q + 1u : q;

        for (j = 0; j < cfg.n; j++) {
            hist[g_conc[j]]++;
        }
        for (i = 0; i <= EX_MAX_C; i++) {
            if (hist[i] != 0u) {
                printf("  %lu conducting in %lu half-cycles\n", (unsigned long)i,
                       (unsigned long)hist[i]);
            }
        }
        // The number the supply, the wiring and the breaker are sized for. With
        // C independent controllers this would be C in the worst case; here the
        // Balance Theorem makes it q+1 with no scheduling at run time.
        printf("  peak simultaneous = %lu of %lu heaters, bound q+1 = %lu  %s\n",
               (unsigned long)peak, (unsigned long)cfg.c, (unsigned long)bound,
               (peak == bound) ? "ok" : "MISMATCH");
    }

    example_host_close();
    return 0;
}

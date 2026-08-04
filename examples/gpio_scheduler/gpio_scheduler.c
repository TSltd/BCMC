//===========================================================================
// gpio_scheduler -- one tick, one column, one port write
//
// The point of this application is that there is almost no application. A tick
// arrives, the observer hands over a column, and the column's bitmap is written
// to a GPIO port. The bitmap is not decoded, indexed or re-packed: bit i of the
// column is the pin driving channel i, so the peripheral's output word is
// already the port word. That is the cheapest possible consumer of BCMC, and it
// is what makes the primitive worth having in hardware -- the schedule arrives
// pre-formatted for the pins.
//
//   for each tick:  port = column_words[..]
//
// WHAT THE PROGRAM IS ALLOWED TO KNOW
//
// It knows N (ticks in a pass), C (channels) and the weights (each channel's
// demanded on-ticks per pass). It does not know the order the ticks will
// present columns in, and it never asks: the traversal is chosen once on the
// command line, handed to examples/common/example_traversal.c, and thereafter
// visible only as the order in which bcmc_observer_next() returns.
//
// WHAT BALANCE BUYS HERE
//
// Every channel gets exactly w_i on-ticks per pass however the ticks are
// ordered -- that is row conservation, and it is why duty cycle is exact rather
// than approximate. Balance is the other half: the number of pins high on any
// one tick is q or q+1 and never more, so the worst-case simultaneous drive is
// known at compile time from W = qN + r rather than measured. The summary
// prints the observed peak beside the predicted q+1.
//
// The port is a variable here rather than a register, because this file must
// compile and run on a host as well as make sense on a board. Substituting a
// real MMIO write for gpio_write() is the whole port.
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

// The "port". On a board this is a volatile store to a GPIO data register; the
// interesting part is that the value needs no preparation, because a BCMC
// column already is the bit pattern the pins want.
static uint32_t g_port[EX_MAX_WORDS];

static uint32_t g_acts[EX_MAX_C];  // on-ticks delivered to channel i
static uint8_t  g_load[EX_MAX_N];  // pins high on the tick that served column j
static uint8_t  g_seen[EX_MAX_N];  // ticks that served column j

static void gpio_write(const uint32_t *words, uint32_t nwords) {
    uint32_t k;
    for (k = 0; k < nwords; k++) {
        g_port[k] = words[k];
    }
}

static void usage(const char *prog) {
    ex_config_usage(prog, "passes",
                    "  A pass is N ticks. Each channel i is on for weight[i] of\n"
                    "  them, whatever order the ticks arrive in.\n");
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
    uint32_t        peak = 0;
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
        fprintf(stderr, "gpio_scheduler: cannot open host: %s\n", bcmc_strstatus(st));
        return 1;
    }

    if (cfg.c > dev.max_c) {
        fprintf(stderr, "gpio_scheduler: %lu channels exceeds the part's MAX_C = %lu\n",
                (unsigned long)cfg.c, (unsigned long)dev.max_c);
        example_host_close();
        return 1;
    }

    nwords = bcmc_column_words(&dev);
    if (nwords > EX_MAX_WORDS) {
        fprintf(stderr, "gpio_scheduler: the part's columns need %lu words\n",
                (unsigned long)nwords);
        example_host_close();
        return 1;
    }

    st = bcmc_load(&dev, cfg.weights, cfg.n, cfg.c);
    if (st != BCMC_OK) {
        fprintf(stderr, "gpio_scheduler: load refused: %s\n", bcmc_strstatus(st));
        example_host_close();
        return 1;
    }

    if (!ex_traversal_init(&tr, cfg.traversal, cfg.seed, order, EX_MAX_N)) {
        example_host_close();
        return 2;
    }
    st = ex_traversal_begin(&tr, &ob, &dev, cfg.n);
    if (st != BCMC_OK) {
        fprintf(stderr, "gpio_scheduler: traversal refused: %s\n", bcmc_strstatus(st));
        example_host_close();
        return 1;
    }

    memset(g_port, 0, sizeof g_port);
    memset(g_acts, 0, sizeof g_acts);
    memset(g_load, 0, sizeof g_load);
    memset(g_seen, 0, sizeof g_seen);

    printf("gpio_scheduler\n");
    ex_config_print(&cfg, "passes");
    printf("channels = %lu, ticks per pass = %lu\n", (unsigned long)cfg.c,
           (unsigned long)cfg.n);

    for (round = 0; round < cfg.rounds; round++) {
        bcmc_observer_rewind(&ob);

        if (!cfg.summary && cfg.rounds > 1u) {
            printf("pass %lu\n", (unsigned long)round);
        }

        while (!bcmc_observer_at_end(&ob)) {
            uint32_t col = 0;
            uint32_t rows[EX_MAX_C];
            uint32_t nhigh = 0;
            uint32_t tick  = ob.t;

            // The tick. One visit, one port write; no queue, no arithmetic.
            st = bcmc_observer_next(&ob, &col, words, nwords);
            if (st != BCMC_OK) {
                fprintf(stderr, "gpio_scheduler: tick failed: %s\n", bcmc_strstatus(st));
                example_host_close();
                return 1;
            }
            gpio_write(words, nwords);

            st = bcmc_column_rows(words, nwords, rows, EX_MAX_C, &nhigh);
            if (st != BCMC_OK) {
                fprintf(stderr, "gpio_scheduler: column %lu has too many rows: %s\n",
                        (unsigned long)col, bcmc_strstatus(st));
                example_host_close();
                return 1;
            }

            g_seen[col] = (uint8_t)(g_seen[col] + 1u);
            g_load[col] = (uint8_t)nhigh;
            for (i = 0; i < nhigh; i++) {
                g_acts[rows[i]]++;
            }
            if (nhigh > peak) {
                peak = nhigh;
            }

            if (!cfg.summary) {
                printf("  tick %2lu: column %2lu  port", (unsigned long)tick,
                       (unsigned long)col);
                // Most significant word first, so the bit pattern reads the way
                // a scope would show the pins.
                for (i = nwords; i > 0u; i--) {
                    printf(" %08lx", (unsigned long)g_port[i - 1u]);
                }
                printf("  high %lu:", (unsigned long)nhigh);
                for (i = 0; i < nhigh; i++) {
                    printf(" ch%lu", (unsigned long)rows[i]);
                }
                printf("\n");
            }
        }
    }

    // The port is left low at the end of a run, as any driver of real pins
    // would leave it. The observer has nothing to do with this.
    memset(g_port, 0, sizeof g_port);

    // ---------------------------------------------------------------------
    // Order-independent from here down.
    // ---------------------------------------------------------------------

    printf("channels\n");
    for (i = 0; i < cfg.c; i++) {
        unsigned long want = (unsigned long)cfg.rounds * (unsigned long)cfg.weights[i];
        printf("  ch%-2lu: demanded %lu/%lu per pass  on-ticks %lu  %s\n",
               (unsigned long)i, (unsigned long)cfg.weights[i], (unsigned long)cfg.n,
               (unsigned long)g_acts[i],
               ((unsigned long)g_acts[i] == want) ? "exact" : "MISMATCH");
    }

    printf("simultaneity\n");
    {
        uint32_t hist[EX_MAX_C + 1u];
        uint32_t q = 0;
        uint32_t r = 0;
        uint32_t w = 0;

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
        for (j = 0; j < cfg.n; j++) {
            hist[g_load[j]]++;
        }
        for (i = 0; i <= EX_MAX_C; i++) {
            if (hist[i] != 0u) {
                printf("  %lu pins high on %lu ticks\n", (unsigned long)i,
                       (unsigned long)hist[i]);
            }
        }
        // This is the number a power supply has to be sized for, and balance is
        // the reason it is known in advance rather than measured.
        printf("  peak simultaneous = %lu, predicted q+1 = %lu  %s\n",
               (unsigned long)peak, (unsigned long)(r != 0u ? q + 1u : q),
               (peak == (r != 0u ? q + 1u : q)) ? "ok" : "MISMATCH");
    }

    example_host_close();
    return 0;
}

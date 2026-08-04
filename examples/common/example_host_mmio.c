//===========================================================================
// example_host_mmio.c -- the bare-metal host: two volatile accesses
//
// This is what example_host.h looks like on a target. It is here to be read
// rather than run: nothing in this repository executes it, because this
// repository has no board attached. scripts/run_sim.sh compiles it under every
// C compiler present, at the same warning level as sw/, so that the claim
// "the applications need no operating system" is checked rather than asserted.
//
// Build with the peripheral's address, e.g.
//
//     cc -std=c99 -DEXAMPLE_HOST_MMIO_BASE=0x30000000u ...
//
// WHAT IS NOT HERE
//
// No cache maintenance, no memory barrier, no interrupt handler. Those are
// real concerns on real SoCs and they are deliberately absent, because they are
// properties of the bus fabric rather than of BCMC: a driver that guessed at
// them would be wrong on the next part. `volatile` is the portable minimum, and
// where a platform needs more it belongs in this file and nowhere else -- which
// is the whole reason the seam is one function wide.
//
// A Wishbone err on this kind of target usually becomes a synchronous bus
// fault, so the accessors below may simply never return. sw/bcmc.h says so
// explicitly and leaves room for platforms where they do; the simulator host is
// one of those, which is why the refusal paths are tested there.
//
//===========================================================================

#include <stdint.h>

#include "example_host.h"

#ifndef EXAMPLE_HOST_MMIO_BASE
// Not a default anybody should use -- it is here so the file compiles as a
// portability check. A real build defines it.
#define EXAMPLE_HOST_MMIO_BASE 0x30000000u
#endif

static int mmio_read(void *ctx, uint32_t addr, uint32_t *data)
{
    (void)ctx;
    *data = *(volatile uint32_t *)(uintptr_t)addr;
    return 0;
}

static int mmio_write(void *ctx, uint32_t addr, uint32_t data)
{
    (void)ctx;
    *(volatile uint32_t *)(uintptr_t)addr = data;
    return 0;
}

bcmc_status_t example_host_open(bcmc_dev_t *dev)
{
    bcmc_status_t st = bcmc_attach(dev, EXAMPLE_HOST_MMIO_BASE, mmio_read, mmio_write,
                                   NULL);
    if (st != BCMC_OK) {
        return st;
    }
    // Three reads: ID, VERSION, CAPS. Everything the applications know about
    // the part's geometry comes from the third of them.
    return bcmc_probe(dev);
}

void example_host_close(void)
{
    // A microcontroller has nothing to close. The function exists so that the
    // applications can be written as if it might.
}

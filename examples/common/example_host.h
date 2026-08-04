//===========================================================================
// example_host.h -- the one thing an application cannot be portable about
//
// Everything else in examples/ is portable C99 that would compile for a
// microcontroller, a Linux process or a simulator without a line changed. This
// header is the seam where that stops being true: somebody has to know the
// peripheral's base address and how to reach it.
//
// So the seam is made explicit and made small. An application calls
// example_host_open() once, receives a probed bcmc_dev_t, and from then on
// speaks only sw/bcmc.h and sw/bcmc_observer.h. It never learns what is on the
// other side.
//
//     Application            examples/{matrix_dump,gpio_scheduler,...}
//         |
//     Observer API           sw/bcmc_observer.h      (traversal)
//         |
//     BCMC driver            sw/bcmc.h               (the register map)
//         |
//     Host                   THIS HEADER             (the wires)
//         |
//     Peripheral             rtl/bcmc_wb.v
//
// TWO IMPLEMENTATIONS SHIP WITH THE PROJECT
//
//   sim/example_host.cpp             the verilated rtl/bcmc_wb.v, so that every
//                                    example in this tree runs against real
//                                    RTL rather than against a model of it
//
//   examples/common/example_host_mmio.c
//                                    volatile loads and stores at
//                                    EXAMPLE_HOST_MMIO_BASE, which is what a
//                                    bare-metal target actually needs
//
// The applications cannot tell them apart, and that is the entire claim this
// header exists to make: an application depends on the register map, not on a
// bus.
//
//===========================================================================

#ifndef BCMC_EXAMPLES_EXAMPLE_HOST_H
#define BCMC_EXAMPLES_EXAMPLE_HOST_H

#include "bcmc.h"

#ifdef __cplusplus
extern "C" {
#endif

// Attaches and probes. On return with BCMC_OK, `dev` is usable and its
// geometry (MAX_C, VAL_W, IDX_W) has been discovered from CAPS rather than
// assumed -- so an application that asks for more rows than the part has is
// refused by the driver with BCMC_ERANGE and not by a constant in this tree.
//
// Calling it twice is the host's business; no example does.
bcmc_status_t example_host_open(bcmc_dev_t *dev);

// Releases whatever the host acquired. On a simulator this is where the model
// is finalised and a coverage or waveform file would be flushed; on a
// microcontroller it does nothing at all.
void example_host_close(void);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // BCMC_EXAMPLES_EXAMPLE_HOST_H

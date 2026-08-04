//===========================================================================
// example_host.cpp -- the simulator's implementation of the host seam
//
// The examples in examples/ are portable C99 and know nothing about Verilator.
// They call example_host_open() and get a bcmc_dev_t back. This file is what
// answers that call when an example is built under sim/: it instantiates the
// verilated bcmc_wb, wraps it in the Wishbone BFM the testbenches already use,
// and hands the driver read and write accessors over it.
//
// WHY THIS FILE EXISTS AT ALL
//
// Because the alternative is a second model of the peripheral. An example that
// talked to a software fake would demonstrate nothing: the interesting question
// is whether an application driving the *real* RTL through the *real* driver
// and the *real* observer sees the properties the mathematics promises. So the
// examples are linked against Vbcmc_wb, and every column they print came out of
// the same Verilog that fpga/ synthesises. There is exactly one BCMC in this
// repository, and it is in rtl/.
//
// The board-side counterpart is examples/common/example_host_mmio.c, which does
// the same job with two volatile stores. Between them they are the whole
// portability story: the applications above this seam are unchanged.
//
// WHAT IT DELIBERATELY DOES NOT DO
//
// It does not check anything. bus timeouts and duplicate responses are counted
// and reported at close, because an example that silently ran on a broken bus
// would be misleading, but the assertions about cycle counts, refusals and
// vector agreement belong to sim/bcmc_wb_test.cpp, sim/bcmc_driver_test.cpp and
// sim/bcmc_observer_test.cpp. This is a cable, not a testbench.
//
//===========================================================================

#include <cstdint>
#include <cstdio>

#include "Vbcmc_wb.h"
#include "verilated.h"
#include "wb_bfm.h"

extern "C" {
#include "bcmc.h"
#include "example_host.h"
}

namespace {

// One peripheral, created on the first open and kept until close. The examples
// are single-threaded programs with one device, and pretending otherwise would
// add machinery that no example needs.
struct SimHost {
    Vbcmc_wb*       dut        = nullptr;
    bcmc::WbMaster* bus        = nullptr;
    uint64_t        ticks      = 0;
    uint64_t        timeouts   = 0;
    uint64_t        duplicates = 0;
    bool            open       = false;
};

SimHost g_host;

void note(const bcmc::WbResponse& r) {
    if (r.timeout) g_host.timeouts++;
    if (r.duplicate) g_host.duplicates++;
}

// The two accessors sw/bcmc.c is given. A read or a write here is a full
// Wishbone classic cycle against the verilated part, so the driver's documented
// access counts are real bus traffic and not bookkeeping.
int sim_read(void* ctx, uint32_t addr, uint32_t* data) {
    (void)ctx;
    bcmc::WbResponse r = g_host.bus->read(addr);
    note(r);
    if (!r.ack()) return -1;
    *data = r.data;
    return 0;
}

int sim_write(void* ctx, uint32_t addr, uint32_t data) {
    (void)ctx;
    bcmc::WbResponse r = g_host.bus->write(addr, data);
    note(r);
    return r.ack() ? 0 : -1;
}

}  // namespace

extern "C" bcmc_status_t example_host_open(bcmc_dev_t* dev) {
    bcmc_status_t st;

    if (dev == nullptr) return BCMC_EINVAL;
    if (g_host.open) return BCMC_EINVAL;

    g_host.dut = new Vbcmc_wb;
    g_host.bus = new bcmc::WbMaster(bcmc::wb_signals(g_host.dut, []() {
        // The only per-eval work an example needs: count time, so close() can
        // say how long the run took in clocks. Tracing belongs to the tests.
        g_host.ticks++;
    }));
    g_host.open = true;

    // Bring the part out of reset before anyone talks to it, exactly as a boot
    // sequence would on a board.
    g_host.bus->reset();

    st = bcmc_attach(dev, 0x0, sim_read, sim_write, nullptr);
    if (st != BCMC_OK) {
        example_host_close();
        return st;
    }

    // CAPS is the part's answer to "how wide are you". Everything above this
    // seam sizes itself from what probe returns.
    st = bcmc_probe(dev);
    if (st != BCMC_OK) {
        example_host_close();
        return st;
    }

    return BCMC_OK;
}

extern "C" void example_host_close(void) {
    if (!g_host.open) return;

    if (g_host.timeouts != 0 || g_host.duplicates != 0) {
        // Not a verdict, a warning: the examples are not the bus testbench, but
        // an example that had been talking to a wedged bus must not look clean.
        std::fprintf(stderr, "example_host: bus trouble: %llu timeouts, %llu duplicates\n",
                     static_cast<unsigned long long>(g_host.timeouts),
                     static_cast<unsigned long long>(g_host.duplicates));
    }

    delete g_host.bus;
    g_host.bus = nullptr;

    if (g_host.dut != nullptr) {
        g_host.dut->final();
        delete g_host.dut;
        g_host.dut = nullptr;
    }

    g_host.ticks      = 0;
    g_host.timeouts   = 0;
    g_host.duplicates = 0;
    g_host.open       = false;
}

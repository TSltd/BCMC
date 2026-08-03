//===========================================================================
// wb_bfm.cpp -- the Wishbone B4 Classic master's timing
//
// The whole protocol is here, in tick() and access(). It is short, and it is
// meant to stay short: everything the master knows is in docs/Register_Map.md
// under "Bus protocol", and anything longer than this would mean the harness
// had started to model the slave as well as the bus.
//
// Two details are worth spelling out, because both were arrived at by reading
// rtl/bcmc_wb.v rather than by guessing.
//
// 1. The response is sampled after the rising edge. bcmc_wb registers
//    wb_ack_o and wb_err_o, so they appear in the cycle *after* the one in
//    which `cyc & stb` was seen. Sampling before the edge would read the
//    previous cycle's response and every access would look like a timeout.
//
// 2. Every access is followed by one idle cycle. bcmc_wb qualifies a new
//    access with `!wb_ack_o && !wb_err_o`, which is what makes the response
//    exactly one cycle wide and stops a held `stb` being serviced twice.
//    Dropping `stb` for a cycle is therefore not politeness, it is the
//    handshake. Back-to-back accesses cost two clocks each, and the tick
//    counter reports that honestly.
//
// 3. The request is held for one clock after the response, and that clock is
//    inspected. This is the only way the master can check note 2 rather than
//    merely rely on it: with `cyc` and `stb` still high, a slave that had
//    forgotten to qualify the access would answer a second time. Nothing but
//    an extra clock is spent, and `duplicate` says what was seen.
//
//===========================================================================

#include "wb_bfm.h"

namespace bcmc {

WbMaster::WbMaster(WbSignals signals, int timeout_ticks)
    : sig_(std::move(signals)), timeout_ticks_(timeout_ticks) {}

void WbMaster::tick() {
    sig_.set_clk(0);
    sig_.eval();
    if (sig_.after_eval) sig_.after_eval();

    sig_.set_clk(1);
    sig_.eval();
    if (sig_.after_eval) sig_.after_eval();

    ++ticks_;
}

void WbMaster::drive_idle() {
    sig_.set_stb(0);
    sig_.set_cyc(0);
    sig_.set_we(0);
    sig_.set_adr(0);
    sig_.set_dat(0);
    sig_.set_sel(kSelWord);
}

void WbMaster::reset(int cycles) {
    drive_idle();
    sig_.set_rst(1);
    for (int i = 0; i < cycles; ++i) tick();
    sig_.set_rst(0);
    tick();
}

void WbMaster::idle(int cycles) {
    drive_idle();
    for (int i = 0; i < cycles; ++i) tick();
}

WbResponse WbMaster::access(uint32_t adr, bool we, uint32_t data, uint8_t sel) {
    sig_.set_adr(adr);
    sig_.set_dat(we ? data : 0);
    sig_.set_sel(sel);
    sig_.set_we(we ? 1 : 0);
    sig_.set_stb(1);
    sig_.set_cyc(1);

    WbResponse r;
    r.timeout = true;
    for (int i = 0; i < timeout_ticks_; ++i) {
        tick();
        if (sig_.get_ack() || sig_.get_err()) {
            r.timeout = false;
            r.err     = sig_.get_err();
            r.data    = sig_.get_dat();
            break;
        }
    }

    // One more clock with the request still held -- see note 3. A second
    // response here means the slave serviced one request twice.
    if (!r.timeout) {
        tick();
        r.duplicate = sig_.get_ack() || sig_.get_err();
    }

    // The mandatory idle cycle -- see note 2 in the file header.
    drive_idle();
    tick();

    ++accesses_;
    return r;
}

WbResponse WbMaster::read(uint32_t adr, uint8_t sel) {
    return access(adr, false, 0, sel);
}

WbResponse WbMaster::write(uint32_t adr, uint32_t data, uint8_t sel) {
    return access(adr, true, data, sel);
}

}  // namespace bcmc

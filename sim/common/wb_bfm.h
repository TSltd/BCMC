//===========================================================================
// wb_bfm.h -- Wishbone B4 Classic master, as a bus functional model
//
// rtl/bcmc_wb.v is the first BCMC module whose primary job is communication
// rather than computation, so its harness needs something the earlier ones did
// not: a model of the *other* side of the wire. This is that model. It knows
// the bus protocol and nothing at all about BCMC.
//
// The protocol implemented here is the one docs/Register_Map.md specifies:
// Classic (non-pipelined) Wishbone B4, 32-bit, word granularity, with a
// single-cycle response in the cycle after `cyc & stb`. There are no bursts,
// no pipelining and no retry. A cycle ends in exactly one of three ways:
//
//     ack   -- the access was legal and, if a read, `data` is meaningful
//     err   -- the access was refused; docs/Register_Map.md says which
//     none  -- nothing came back before the timeout, which is always a bug
//
// The third case is why access() reports a timeout instead of hanging or
// asserting: a harness that can distinguish "refused" from "never answered"
// gives a far better failure message than one that stops the simulation.
//
// THE RESPONSE IS EXACTLY ONE CYCLE WIDE
//
// A fourth outcome is watched for even though it must never happen: a second
// response to the same request. bcmc_wb qualifies a new access with
// `!wb_ack_o && !wb_err_o`, so a `stb` that stays high across the response
// cycle is not serviced again. That is a property of the slave, not a
// courtesy of the master, and a master that drops `stb` the instant it sees
// `ack` can never tell whether the slave has it. So access() holds the
// request one cycle longer than it needs to and reports what it saw in
// WbResponse::duplicate. Every access costs three clocks because of it, which
// is a cheap price for an entire class of bug the suite would otherwise miss.
//
// ERR IS A RESULT, NOT AN EXCEPTION
//
// Roughly a third of docs/Transaction_Sequences.md is failure cases (F1..F11),
// so the erring access is not the exceptional path here -- it is half the
// specification. access() therefore returns WbResponse for every outcome and
// leaves the judgement to the caller, who alone knows which was expected.
//
// BINDING TO A VERILATED TOP
//
// The DUT is reached through std::function accessors rather than a template
// parameter, so the protocol logic can live in wb_bfm.cpp and be compiled
// once. wb_signals() builds the accessors for any Verilated model whose ports
// carry the canonical Wishbone names:
//
//     WbMaster bus(wb_signals(dut));
//     bus.reset();
//     WbResponse r = bus.read(0x000);
//     if (r.ack() && r.data != 0x42434D43u) { ... }
//
// To record a waveform, pass a hook that runs after every eval():
//
//     WbMaster bus(wb_signals(dut, [&]{ trace->dump(bus.ticks()); }));
//
//===========================================================================

#ifndef BCMC_SIM_WB_BFM_H
#define BCMC_SIM_WB_BFM_H

#include <cstdint>
#include <functional>
#include <type_traits>
#include <utility>

namespace bcmc {

// Every access in docs/Register_Map.md is word-granular: any other `sel`
// pattern is error E3. Tests that mean to provoke E3 pass their own value.
constexpr uint8_t kSelWord = 0xF;

// The outcome of one bus cycle. Exactly one of ack(), err and timeout holds;
// `duplicate` is orthogonal to all three and must always be false.
struct WbResponse {
    bool     err       = false;  // the slave asserted wb_err_o
    bool     timeout   = false;  // neither ack nor err arrived in time
    bool     duplicate = false;  // it answered the same request twice
    uint32_t data      = 0;      // wb_dat_o, meaningful only on an acked read

    bool ack() const { return !err && !timeout; }
};

// The DUT, reduced to the handful of operations the protocol needs. Kept as a
// plain struct of callables so that WbMaster itself need not be a template.
struct WbSignals {
    std::function<void()>        eval;        // settle combinational logic
    std::function<void()>        after_eval;  // optional: dump a trace sample
    std::function<void(uint8_t)> set_clk;
    std::function<void(uint8_t)> set_rst;
    std::function<void(uint32_t)> set_adr;
    std::function<void(uint32_t)> set_dat;
    std::function<void(uint8_t)>  set_sel;
    std::function<void(uint8_t)>  set_we;
    std::function<void(uint8_t)>  set_stb;
    std::function<void(uint8_t)>  set_cyc;
    std::function<uint32_t()>     get_dat;
    std::function<bool()>         get_ack;
    std::function<bool()>         get_err;
};

// Binds `dut` by its port names. The static_casts are deliberate: Verilator
// picks a port's C type from its declared width, so wb_adr_i is narrower than
// the uint32_t addresses the tests speak in, and an unaligned or out-of-region
// address must be allowed to truncate exactly as the hardware would.
//
// Verilator declares the top-level ports as references into the root model, so
// decltype of a port is a reference type; std::decay_t recovers the value type
// that the cast needs.
template <class T>
using PortType = typename std::decay<T>::type;

template <class Dut>
WbSignals wb_signals(Dut* dut, std::function<void()> after_eval = nullptr) {
    WbSignals s;
    s.eval       = [dut]() { dut->eval(); };
    s.after_eval = std::move(after_eval);
    s.set_clk    = [dut](uint8_t v) { dut->wb_clk_i = v; };
    s.set_rst    = [dut](uint8_t v) { dut->wb_rst_i = v; };
    s.set_adr    = [dut](uint32_t v) {
        dut->wb_adr_i = static_cast<PortType<decltype(dut->wb_adr_i)> >(v);
    };
    s.set_dat = [dut](uint32_t v) {
        dut->wb_dat_i = static_cast<PortType<decltype(dut->wb_dat_i)> >(v);
    };
    s.set_sel = [dut](uint8_t v) { dut->wb_sel_i = v; };
    s.set_we  = [dut](uint8_t v) { dut->wb_we_i = v; };
    s.set_stb = [dut](uint8_t v) { dut->wb_stb_i = v; };
    s.set_cyc = [dut](uint8_t v) { dut->wb_cyc_i = v; };
    s.get_dat = [dut]() { return static_cast<uint32_t>(dut->wb_dat_o); };
    s.get_ack = [dut]() { return dut->wb_ack_o != 0; };
    s.get_err = [dut]() { return dut->wb_err_o != 0; };
    return s;
}

// A Wishbone B4 Classic master. One instance owns the clock, so tick counts
// are also cycle counts and a test can report what a sequence cost.
class WbMaster {
public:
    // `timeout_ticks` bounds the wait for a response. bcmc_wb answers in one
    // cycle; the generous default exists so that a genuinely stuck slave is
    // reported as stuck rather than mistaken for a slow one.
    explicit WbMaster(WbSignals signals, int timeout_ticks = 64);

    // One clock: low, eval, high, eval. Combinational outputs are read after
    // the rising edge, which is where a Classic slave presents its response.
    void tick();

    // Holds reset with the bus idle, then releases it and settles one cycle.
    void reset(int cycles = 4);

    // Runs the clock with no access in progress. The way to let a transform
    // finish without polling it, and the way to give an IRQ time to appear.
    void idle(int cycles = 1);

    // One complete cycle. Returns however it ended; judging that is the
    // caller's job.
    WbResponse access(uint32_t adr, bool we, uint32_t data, uint8_t sel = kSelWord);

    WbResponse read(uint32_t adr, uint8_t sel = kSelWord);
    WbResponse write(uint32_t adr, uint32_t data, uint8_t sel = kSelWord);

    uint64_t ticks() const { return ticks_; }
    uint64_t accesses() const { return accesses_; }
    int      timeout_ticks() const { return timeout_ticks_; }

private:
    void drive_idle();

    WbSignals sig_;
    int       timeout_ticks_;
    uint64_t  ticks_    = 0;
    uint64_t  accesses_ = 0;
};

}  // namespace bcmc

#endif  // BCMC_SIM_WB_BFM_H

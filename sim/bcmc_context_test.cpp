//===========================================================================
// bcmc_context_test.cpp -- the context stores the representation faithfully
//
// rtl/bcmc_context.v contains no mathematics, so this harness cannot check it
// against reference.py the way bcmc_core_test.cpp and bcmc_cell_test.cpp do.
// What it checks instead is that the context is a faithful place to keep the
// canonical prefix representation -- and it still refuses to invent an answer:
// every weight written and every offset expected comes from a matrix_*.txt
// file, which validation/gen_vectors.py produced from validation/reference.py.
// The only values in this file that come from nowhere else are the sentinels
// in the arbitration suite, where the expected answer is "what was written"
// or "zero", which is storage semantics rather than BCMC.
//
// To do that, the harness verilates TWO tops and plays the part the Wishbone
// wrapper will play in v0.4c: it streams the stored weights into a real
// bcmc_core and lets the returning offsets land in the context.
//
//     bcmc_context.core_weight  ->  bcmc_core.weight_in
//     bcmc_core.offset_out      ->  bcmc_context.offset_in
//
// A pass therefore says something stronger than "the register file works": it
// says the Core and the context compose into the canonical prefix
// representation that reference.py computed, with the offsets landing in the
// right places, in the right order, and nothing lost to a handshake.
//
// Six checks:
//
//   1. Reset.             Both windows read zero; the arbiter is idle.
//   2. Weight window.     What software wrote is what software reads back,
//                         and a transform does not disturb it.
//   3. Offset window.     After a transform the window is reference.py's
//                         offsets, in order, with OFFSET[C .. MAX_C-1] zero.
//   4. Two views, one     The indexed software ports and the flat Evaluator
//      truth.             vectors agree at every lane, always.
//   5. No stale offsets.  Running a smaller C after a larger one leaves no
//                         offset behind from the larger. The register map
//                         requires this; clearing the window whole at
//                         load_start is what delivers it.
//   6. Arbitration.       While the Core owns the context, software cannot
//                         write it; an out-of-range index writes nothing and
//                         reads zero; an unowned offset is dropped.
//
// Check 6 needs a second elaboration of the same module, with -DSYNTHESIS, so
// that the simulation assertions are compiled out. Every input it drives is
// one the register map already denies at the bus, so the assertions would
// (correctly) stop the run. The assertions are the wrapper's contract; the
// guards are the defence in depth behind them. To observe the defence you
// have to silence the alarm.
//
// Plus order and gap invariance: weights are written forwards, backwards and
// shuffled, and streamed with three different idle-gap patterns. Storage
// cannot care, which is exactly what is being tested.
//
// Usage:
//     bcmc_context_test [--limit K] [--vcd PATH] <matrix_vectors.txt>...
//===========================================================================

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <random>
#include <string>
#include <vector>

#include "Vbcmc_context.h"
#include "Vbcmc_context_nc.h"
#include "Vbcmc_core.h"
#include "verilated.h"
#include "vectors.h"

#if VM_TRACE
#include "verilated_vcd_c.h"
#endif

// The elaborated geometry. Set by sim/CMakeLists.txt, which passes the same
// numbers to Verilator as -G, so the two can never drift apart.
#ifndef BCMC_CONTEXT_MAX_C
#define BCMC_CONTEXT_MAX_C 32
#endif
#ifndef BCMC_CONTEXT_VAL_W
#define BCMC_CONTEXT_VAL_W 16
#endif

namespace {

constexpr uint32_t kMaxC = BCMC_CONTEXT_MAX_C;
constexpr uint32_t kValW = BCMC_CONTEXT_VAL_W;

//---------------------------------------------------------------------------
// Reading a lane out of a flat port
//
// Verilog-2005 has no array ports, so both windows leave the module as flat
// vectors with row i in bits [VAL_W*i +: VAL_W]. Verilator presents anything
// wider than 64 bits as an array of 32-bit words, so the unpacking is done
// here rather than in the RTL. Bit by bit, so that no width is assumed.
//---------------------------------------------------------------------------

template <typename Port>
auto port_word(const Port& p, uint32_t w) -> decltype(p[0]) {
    return p[w];
}
uint32_t port_word(const uint32_t& p, uint32_t w) { return w == 0 ? p : 0u; }
uint32_t port_word(const uint64_t& p, uint32_t w) {
    return w == 0 ? static_cast<uint32_t>(p)
                  : (w == 1 ? static_cast<uint32_t>(p >> 32) : 0u);
}

template <typename Port>
uint32_t lane(const Port& p, uint32_t i) {
    uint32_t       value = 0;
    const uint32_t lsb   = kValW * i;
    for (uint32_t b = 0; b < kValW; ++b) {
        const uint32_t bit = (port_word(p, (lsb + b) / 32) >> ((lsb + b) % 32)) & 1u;
        value |= bit << b;
    }
    return value;
}

//---------------------------------------------------------------------------
// Reporting
//---------------------------------------------------------------------------

int      failures       = 0;
uint64_t values_checked = 0;

void fail(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

void fail(const char* fmt, ...) {
    if (failures < 20) {
        va_list ap;
        va_start(ap, fmt);
        std::vfprintf(stdout, fmt, ap);
        va_end(ap);
    } else if (failures == 20) {
        std::printf("... further failures suppressed\n");
    }
    ++failures;
}

const char* order_name(int pass) {
    switch (pass) {
        case 0:  return "forwards";
        case 1:  return "backwards";
        default: return "shuffled";
    }
}

//---------------------------------------------------------------------------
// The context, wired to a real Core
//
// This class is the whole of the sequencer that v0.4c will put in the bus
// wrapper: it walks the weight window into the Core and lets the offsets fall
// back into the offset window. It is deliberately the only place in this file
// that knows anything about the Prefix Stream Interface.
//---------------------------------------------------------------------------

class Dut {
  public:
    Dut() : ctx_(new Vbcmc_context), core_(new Vbcmc_core) {}

    ~Dut() {
#if VM_TRACE
        if (trace_) {
            trace_->close();
            delete trace_;
        }
#endif
        ctx_->final();
        core_->final();
        delete ctx_;
        delete core_;
    }

    void open_trace(const std::string& path) {
#if VM_TRACE
        Verilated::traceEverOn(true);
        trace_ = new VerilatedVcdC;
        ctx_->trace(trace_, 99);
        trace_->open(path.c_str());
#else
        (void)path;
        std::fprintf(stderr, "warning: built without tracing; --vcd ignored\n");
#endif
    }

    void reset() {
        ctx_->rst          = 1;
        core_->rst         = 1;
        ctx_->sw_we        = 0;
        ctx_->sw_windex    = 0;
        ctx_->sw_wdata     = 0;
        ctx_->sw_rindex    = 0;
        ctx_->core_rindex  = 0;
        ctx_->load_start   = 0;
        core_->start       = 0;
        core_->weight_valid = 0;
        core_->N           = 1;
        core_->C           = 0;
        tick();
        tick();
        ctx_->rst  = 0;
        core_->rst = 0;
        tick();
    }

    // ---- client 1: software ------------------------------------------------

    void write_weight(uint32_t index, uint32_t value) {
        ctx_->sw_we     = 1;
        ctx_->sw_windex = index;
        ctx_->sw_wdata  = value;
        tick();
        ctx_->sw_we = 0;
    }

    uint32_t read_weight(uint32_t index) {
        ctx_->sw_rindex = index;
        settle();
        return ctx_->sw_weight;
    }

    uint32_t read_offset(uint32_t index) {
        ctx_->sw_rindex = index;
        settle();
        return ctx_->sw_offset;
    }

    // ---- client 3: the Evaluator ------------------------------------------

    uint32_t flat_weight(uint32_t i) {
        settle();
        return lane(ctx_->weights_flat, i);
    }
    uint32_t flat_offset(uint32_t i) {
        settle();
        return lane(ctx_->offsets_flat, i);
    }

    bool loading() {
        settle();
        return ctx_->loading != 0;
    }

    // ---- client 2: the Core ----------------------------------------------
    //
    // Runs one transform end to end, returning the offset stream as the Core
    // emitted it. `gap` idle cycles are inserted between weights; the Prefix
    // Stream Interface promises the values do not depend on them.
    std::vector<uint32_t> run_transform(uint32_t N, uint32_t C, uint32_t gap) {
        std::vector<uint32_t> stream;

        core_->N         = N;
        core_->C         = C;
        core_->start     = 1;
        ctx_->load_start = 1;
        tick();
        core_->start     = 0;
        ctx_->load_start = 0;

        uint32_t index = 0;
        uint32_t wait  = 0;

        // Generous bound: C weights, each preceded by at most `gap` idle
        // cycles, plus the framing cycles at either end.
        const uint64_t budget = 8ull + static_cast<uint64_t>(C) * (gap + 2);

        for (uint64_t cycle = 0; cycle < budget; ++cycle) {
            // weight_valid is illegal while the Core is not busy, so it is
            // gated on busy rather than merely on having weights left.
            const bool present = core_->busy && (index < C) && (wait == 0);

            ctx_->core_rindex   = index;
            core_->weight_valid = present ? 1 : 0;

            tick();

            if (core_->offset_valid) stream.push_back(core_->offset_out);

            if (present) {
                ++index;
                wait = gap;
            } else if (wait > 0) {
                --wait;
            }

            if (core_->done) break;
        }

        core_->weight_valid = 0;

        // The context releases ownership on load_done, which the harness wires
        // to the Core's done pulse; one more cycle retires it.
        tick();
        return stream;
    }

  private:
    // The combinational links between the two modules. Called before every
    // edge, so each module sees the other's registered outputs settled.
    void link() {
        core_->weight_in   = ctx_->core_weight;
        ctx_->offset_in    = core_->offset_out;
        ctx_->offset_valid = core_->offset_valid;
        ctx_->load_done    = core_->done;
    }

    void settle() {
        link();
        ctx_->eval();
        core_->eval();
        link();
        ctx_->eval();
        core_->eval();
    }

    void tick() {
        settle();
        ctx_->clk = 1;
        core_->clk = 1;
        ctx_->eval();
        core_->eval();
#if VM_TRACE
        if (trace_) trace_->dump(time_);
#endif
        time_ += 5;
        ctx_->clk = 0;
        core_->clk = 0;
        ctx_->eval();
        core_->eval();
#if VM_TRACE
        if (trace_) trace_->dump(time_);
#endif
        time_ += 5;
        settle();
    }

    Vbcmc_context* ctx_  = nullptr;
    Vbcmc_core*    core_ = nullptr;
#if VM_TRACE
    VerilatedVcdC* trace_ = nullptr;
#endif
    uint64_t time_ = 0;
};

//---------------------------------------------------------------------------
// 1. Reset
//---------------------------------------------------------------------------

void check_reset(Dut& dut) {
    dut.reset();

    if (dut.loading()) fail("FAIL [reset]  loading is set after reset\n");

    for (uint32_t i = 0; i < kMaxC; ++i) {
        ++values_checked;
        if (dut.read_weight(i) != 0) {
            fail("FAIL [reset]  WEIGHT[%u] = %u after reset, expected 0\n", i,
                 dut.read_weight(i));
        }
        if (dut.read_offset(i) != 0) {
            fail("FAIL [reset]  OFFSET[%u] = %u after reset, expected 0\n", i,
                 dut.read_offset(i));
        }
    }
}

//---------------------------------------------------------------------------
// 4. Two views, one truth
//
// Every lane, both windows, indexed port against flat vector. An index above
// MAX_C has no lane to disagree with, so it is checked separately: it reads 0.
//---------------------------------------------------------------------------

void check_views(Dut& dut, const std::string& where) {
    for (uint32_t i = 0; i < kMaxC; ++i) {
        const uint32_t w_indexed = dut.read_weight(i);
        const uint32_t w_flat    = dut.flat_weight(i);
        const uint32_t o_indexed = dut.read_offset(i);
        const uint32_t o_flat    = dut.flat_offset(i);
        values_checked += 2;

        if (w_indexed != w_flat) {
            fail("FAIL [%s]  WEIGHT[%u]: software reads %u, the Evaluator sees %u\n",
                 where.c_str(), i, w_indexed, w_flat);
        }
        if (o_indexed != o_flat) {
            fail("FAIL [%s]  OFFSET[%u]: software reads %u, the Evaluator sees %u\n",
                 where.c_str(), i, o_indexed, o_flat);
        }
    }

    for (uint32_t i = kMaxC; i < kMaxC + 3; ++i) {
        ++values_checked;
        if (dut.read_weight(i) != 0 || dut.read_offset(i) != 0) {
            fail("FAIL [%s]  index %u is above MAX_C = %u but does not read 0\n",
                 where.c_str(), i, kMaxC);
        }
    }
}

//---------------------------------------------------------------------------
// Preconditions, owned by the testbench
//---------------------------------------------------------------------------

bool preconditions_ok(const std::string& file, const bcmc::MatrixCase& c) {
    if (c.N < 1) {
        fail("FAIL %s:%d  precondition N >= 1 violated (N = %u)\n", file.c_str(),
             c.line, c.N);
        return false;
    }
    if (c.C > kMaxC) {
        fail("FAIL %s:%d  C = %u exceeds the elaborated MAX_C = %u\n", file.c_str(),
             c.line, c.C, kMaxC);
        return false;
    }
    if (c.weights.size() != c.C || c.offsets.size() != c.C) {
        fail("FAIL %s:%d  malformed case: C = %u but %zu weights, %zu offsets\n",
             file.c_str(), c.line, c.C, c.weights.size(), c.offsets.size());
        return false;
    }
    for (uint32_t i = 0; i < c.C; ++i) {
        if (c.weights[i] > c.N) {
            fail("FAIL %s:%d  precondition weight <= N violated in row %u "
                 "(weight = %u, N = %u)\n",
                 file.c_str(), c.line, i, c.weights[i], c.N);
            return false;
        }
    }
    return true;
}

//---------------------------------------------------------------------------
// 2, 3 and 5: one case, programmed and run
//
// `fresh` says whether the context was reset immediately before. When it was
// not, whatever the previous case left behind is still in both windows, which
// is what makes check 5 -- no stale offsets -- a real check.
//---------------------------------------------------------------------------

void run_case(Dut& dut, const std::string& file, const bcmc::MatrixCase& c,
              int pass, bool fresh) {
    const uint32_t gaps[3] = {0, 1, 3};
    const uint32_t gap     = gaps[pass % 3];

    // ---- 2. the weight window, written in one of three orders --------------

    std::vector<uint32_t> order(c.C);
    std::iota(order.begin(), order.end(), 0u);
    if (pass == 1) {
        std::reverse(order.begin(), order.end());
    } else if (pass == 2) {
        std::mt19937 rng(0x43545800u ^ c.N ^ (c.C << 8));
        std::shuffle(order.begin(), order.end(), rng);
    }

    for (const uint32_t i : order) dut.write_weight(i, c.weights[i]);

    for (uint32_t i = 0; i < c.C; ++i) {
        ++values_checked;
        const uint32_t got = dut.read_weight(i);
        if (got != c.weights[i]) {
            fail("FAIL %s:%d  WEIGHT[%u] read back %u, wrote %u (pass: %s)\n",
                 file.c_str(), c.line, i, got, c.weights[i], order_name(pass));
        }
    }

    // ---- the transform ----------------------------------------------------

    const std::vector<uint32_t> stream = dut.run_transform(c.N, c.C, gap);

    // The stream itself, before asking where it landed: this is the Core's
    // output, and reference.py says what it must be.
    if (stream.size() != c.C) {
        fail("FAIL %s:%d  the Core emitted %zu offsets, C = %u (gap %u)\n",
             file.c_str(), c.line, stream.size(), c.C, gap);
    } else {
        for (uint32_t i = 0; i < c.C; ++i) {
            ++values_checked;
            if (stream[i] != c.offsets[i]) {
                fail("FAIL %s:%d  the Core's offset %u is %u, reference.py says %u\n",
                     file.c_str(), c.line, i, stream[i], c.offsets[i]);
            }
        }
    }

    // ---- 3. the offset window --------------------------------------------

    for (uint32_t i = 0; i < c.C; ++i) {
        ++values_checked;
        const uint32_t got = dut.read_offset(i);
        if (got != c.offsets[i]) {
            fail("FAIL %s:%d  OFFSET[%u] = %u, reference.py says %u "
                 "(pass: %s, gap %u)\n",
                 file.c_str(), c.line, i, got, c.offsets[i], order_name(pass), gap);
        }
    }

    // ---- 5. no stale offsets ---------------------------------------------
    //
    // Every lane above C, whether or not an earlier and larger case wrote it.

    for (uint32_t i = c.C; i < kMaxC; ++i) {
        ++values_checked;
        const uint32_t got = dut.read_offset(i);
        if (got != 0) {
            fail("FAIL %s:%d  OFFSET[%u] = %u above C = %u, expected 0 "
                 "(context was %s)\n",
                 file.c_str(), c.line, i, got, c.C, fresh ? "fresh" : "reused");
        }
    }

    // ---- 2, again: a transform must not disturb the weight window ---------

    for (uint32_t i = 0; i < c.C; ++i) {
        ++values_checked;
        const uint32_t got = dut.read_weight(i);
        if (got != c.weights[i]) {
            fail("FAIL %s:%d  WEIGHT[%u] became %u across the transform, wrote %u\n",
                 file.c_str(), c.line, i, got, c.weights[i]);
        }
    }

    // ---- 4. the Evaluator sees the same thing software does --------------

    check_views(dut, file);
}

//---------------------------------------------------------------------------
// 6. Arbitration
//
// Driven against the -DSYNTHESIS elaboration, whose assertions are compiled
// out: every stimulus below is one the register map denies at the bus, so the
// assertions would stop the run rather than let the guard be observed.
//
// This suite drives no Core. It needs none: the context cannot tell an offset
// from any other VAL_W-bit number, which is the point of the module.
//---------------------------------------------------------------------------

class Arbiter {
  public:
    Arbiter() : dut_(new Vbcmc_context_nc) {}
    ~Arbiter() {
        dut_->final();
        delete dut_;
    }

    void reset() {
        dut_->rst          = 1;
        dut_->sw_we        = 0;
        dut_->sw_windex    = 0;
        dut_->sw_wdata     = 0;
        dut_->sw_rindex    = 0;
        dut_->core_rindex  = 0;
        dut_->load_start   = 0;
        dut_->load_done    = 0;
        dut_->offset_valid = 0;
        dut_->offset_in    = 0;
        tick();
        tick();
        dut_->rst = 0;
        tick();
    }

    void write_weight(uint32_t index, uint32_t value) {
        dut_->sw_we     = 1;
        dut_->sw_windex = index;
        dut_->sw_wdata  = value;
        tick();
        dut_->sw_we = 0;
    }

    void push_offset(uint32_t value) {
        dut_->offset_valid = 1;
        dut_->offset_in    = value;
        tick();
        dut_->offset_valid = 0;
    }

    void load_start() {
        dut_->load_start = 1;
        tick();
        dut_->load_start = 0;
    }

    void load_done() {
        dut_->load_done = 1;
        tick();
        dut_->load_done = 0;
    }

    uint32_t weight(uint32_t index) {
        dut_->sw_rindex = index;
        dut_->eval();
        return dut_->sw_weight;
    }

    uint32_t offset(uint32_t index) {
        dut_->sw_rindex = index;
        dut_->eval();
        return dut_->sw_offset;
    }

    bool loading() {
        dut_->eval();
        return dut_->loading != 0;
    }

  private:
    void tick() {
        dut_->eval();
        dut_->clk = 1;
        dut_->eval();
        dut_->clk = 0;
        dut_->eval();
    }

    Vbcmc_context_nc* dut_ = nullptr;
};

void check_arbitration() {
    Arbiter a;

    // ---- while the Core owns the context, software cannot write it --------

    a.reset();
    a.write_weight(0, 0x1234);
    ++values_checked;
    if (a.weight(0) != 0x1234) {
        fail("FAIL [arbitration]  an idle context refused a weight write\n");
    }

    a.load_start();
    ++values_checked;
    if (!a.loading()) {
        fail("FAIL [arbitration]  load_start did not take ownership\n");
    }

    a.write_weight(0, 0x5678);
    ++values_checked;
    if (a.weight(0) != 0x1234) {
        fail("FAIL [arbitration]  a weight write while loading changed WEIGHT[0] "
             "to 0x%X\n",
             a.weight(0));
    }

    a.load_done();
    ++values_checked;
    if (a.loading()) {
        fail("FAIL [arbitration]  load_done did not release ownership\n");
    }

    a.write_weight(0, 0x5678);
    ++values_checked;
    if (a.weight(0) != 0x5678) {
        fail("FAIL [arbitration]  the context stayed locked after load_done\n");
    }

    // ---- an offset arriving unowned is dropped ---------------------------

    a.reset();
    a.push_offset(0xBEEF);
    ++values_checked;
    if (a.offset(0) != 0) {
        fail("FAIL [arbitration]  an offset was stored while not loading "
             "(OFFSET[0] = 0x%X)\n",
             a.offset(0));
    }

    // ---- an out-of-range index writes nothing, and reads zero ------------

    a.reset();
    a.write_weight(kMaxC, 0xDEAD);
    a.write_weight(kMaxC + 7, 0xDEAD);
    for (uint32_t i = 0; i < kMaxC; ++i) {
        ++values_checked;
        if (a.weight(i) != 0) {
            fail("FAIL [arbitration]  a write to index %u landed in WEIGHT[%u] "
                 "(0x%X)\n",
                 kMaxC, i, a.weight(i));
        }
    }
    ++values_checked;
    if (a.weight(kMaxC) != 0 || a.offset(kMaxC) != 0) {
        fail("FAIL [arbitration]  index %u does not read 0\n", kMaxC);
    }

    // ---- the offset window fills in stream order, then stops --------------
    //
    // MAX_C offsets fill it exactly; the next one has nowhere to go and must
    // not wrap around and overwrite lane 0.

    a.reset();
    a.load_start();
    for (uint32_t i = 0; i < kMaxC; ++i) a.push_offset(0x100 + i);
    for (uint32_t i = 0; i < kMaxC; ++i) {
        ++values_checked;
        if (a.offset(i) != 0x100 + i) {
            fail("FAIL [arbitration]  OFFSET[%u] = 0x%X, the stream put 0x%X there\n",
                 i, a.offset(i), 0x100 + i);
        }
    }
    a.push_offset(0xFFFF);
    ++values_checked;
    if (a.offset(0) != 0x100) {
        fail("FAIL [arbitration]  an overrun offset wrapped onto OFFSET[0] (0x%X)\n",
             a.offset(0));
    }

    // ---- load_start clears the whole window ------------------------------

    a.load_done();
    a.load_start();
    for (uint32_t i = 0; i < kMaxC; ++i) {
        ++values_checked;
        if (a.offset(i) != 0) {
            fail("FAIL [arbitration]  load_start left OFFSET[%u] = 0x%X\n", i,
                 a.offset(i));
        }
    }
}

}  // namespace

//---------------------------------------------------------------------------

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);

    std::vector<std::string> files;
    size_t                   limit = 0;   // 0 = no limit
    std::string              vcd;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--limit" && i + 1 < argc) {
            limit = static_cast<size_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (arg == "--vcd" && i + 1 < argc) {
            vcd = argv[++i];
        } else if (arg.rfind("--", 0) == 0 || arg.rfind("+", 0) == 0) {
            continue;   // Verilator's own arguments
        } else {
            files.push_back(arg);
        }
    }

    if (files.empty()) {
        std::fprintf(stderr,
                     "usage: %s [--limit K] [--vcd PATH] <matrix_vectors.txt>...\n",
                     argv[0]);
        return 2;
    }

    Dut dut;
    if (!vcd.empty()) dut.open_trace(vcd);

    check_reset(dut);
    check_views(dut, "reset");
    check_arbitration();

    size_t total_cases = 0;

    for (const std::string& file : files) {
        std::vector<bcmc::MatrixCase> cases;
        try {
            cases = bcmc::load_matrix_vectors(file);
        } catch (const std::exception& e) {
            std::printf("FAIL  %s\n", e.what());
            return 1;
        }

        if (limit != 0 && cases.size() > limit) cases.resize(limit);

        std::vector<size_t> order;
        for (size_t i = 0; i < cases.size(); ++i) {
            if (preconditions_ok(file, cases[i])) order.push_back(i);
        }

        for (int pass = 0; pass < 3; ++pass) {
            if (pass == 1) {
                std::reverse(order.begin(), order.end());
            } else if (pass == 2) {
                std::mt19937 rng(0x42434D43u ^ static_cast<uint32_t>(cases.size()));
                std::shuffle(order.begin(), order.end(), rng);
            }

            // The context is reset once per pass, not once per case: from the
            // second case onwards the windows still hold the previous case,
            // which is what gives check 5 something to find.
            dut.reset();
            bool fresh = true;
            for (const size_t idx : order) {
                run_case(dut, file, cases[idx], pass, fresh);
                fresh = false;
            }
        }

        total_cases += cases.size();
    }

    std::printf("%s  %zu contexts, %llu stored values checked, %d failures\n",
                failures == 0 ? "PASS" : "FAIL", total_cases,
                static_cast<unsigned long long>(values_checked), failures);

    return failures == 0 ? 0 : 1;
}

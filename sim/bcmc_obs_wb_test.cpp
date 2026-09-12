//===========================================================================
// bcmc_obs_wb_test.cpp -- the observer window, on the bus
//
// Replays sim/vectors/obswb_edge.txt against rtl/bcmc_obs_wb.v, one corpus
// cycle at a time. The corpus is the record of what validation/
// observer_periph.py -- the golden model of docs/Observer_Register_Map.md --
// said, and validation/replay_observer_wb.py has already checked that the
// record describes that contract. This harness holds the RTL to the same
// record, so a disagreement is the RTL's.
//
// The two failures this is built to catch are different in kind, and the
// corpus separates them: the bus mechanics (ack, err, data, and a refused
// access leaving no trace) and the engine translation (acceptance in one cycle
// becoming a visit in the next). sim/tb_observer_wb_smoke.v asks those two
// questions of one hand-written sequence; this asks them of the whole corpus.
//
// What is compared, and what cannot be
// ------------------------------------
// Every cycle: wb_ack_o, wb_err_o, running_o, visit_valid_o, column_o, done_o,
// aborted_o, column_bits_o.
//
// Wb_dat_o is compared ONLY on a cycle the corpus records as acknowledged. On
// every other cycle the RTL legitimately holds the previous read's value --
// wb_dat_o is loaded on an accepted access and holds otherwise -- while the
// corpus records 0 there, because the model has no data bus and 0 is a
// placeholder for "no transaction". Comparing them would fail on a held value
// that is not wrong. On an acknowledged cycle the comparison is exact: rdata
// for a read, and 0 for a write, which is what the module loads.
//
// Column_bits is compared only for runs with N >= 1. With N = 0 the query is
// outside the domain the projection is defined for: bcmc_cell's preconditions
// say N >= 1, and the corpus's 0 is the model DECLINING to answer (its
// expected_bits guards N < 1) rather than a prediction. The RTL answers
// something defined but meaningless there -- bcmc_cell.v says as much -- so
// holding it to the model's abstention would be holding it to the wrong thing.
// The refusal of START at N = 0, which is what that run is actually about, is
// checked in full, including the ack, the err and the absence of any trace.
//
// Deliberately NOT compared, because they are not ports: the cursor, visit_q,
// the model's internal latches. The corpus does not carry them, this harness
// cannot see them, and validation/replay_observer_wb.py is where they are the
// oracle. That asymmetry is the same one sim/bcmc_observer_hw_test.cpp uses.
//
// Geometry
// --------
// The peripheral's own reference build geometry, passed to Verilator (-G) and
// to this translation unit (BCMC_OBSWB_*) from sim/CMakeLists.txt so the two
// cannot drift, and matching validation/observer_periph.py's REF_* constants.
// The corpus's first recording reads OBS_CAPS, which reports this geometry, so
// a mismatch fails loudly rather than subtly -- the same guard, for the same
// reason, as the BCMC wrapper's.
//
// Usage:
//     bcmc_obs_wb_test [--limit K] [--vcd PATH] <obswb_vectors.txt>...
//===========================================================================

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "Vbcmc_obs_wb.h"
#include "verilated_vcd_c.h"

namespace {

#ifndef BCMC_OBSWB_MAX_C
#define BCMC_OBSWB_MAX_C 64
#endif
#ifndef BCMC_OBSWB_VAL_W
#define BCMC_OBSWB_VAL_W 16
#endif
#ifndef BCMC_OBSWB_IDX_W
#define BCMC_OBSWB_IDX_W 16
#endif

constexpr int kValW  = BCMC_OBSWB_VAL_W;
constexpr int kIdxW  = BCMC_OBSWB_IDX_W;
constexpr int kMaxC  = BCMC_OBSWB_MAX_C;
constexpr int kWords = (kMaxC * kValW + 31) / 32;

using FlatVec = VlWide<kWords>;

// The context the corpus was recorded with: gen_observer_wb_vectors.py's W2
// and O2. Two rows, each of weight 1 and offset 0. It is deliberately not the
// zero context -- a zeroed one would make every visit's projection zero, and
// the projection is one of the things this harness exists to check.
constexpr uint32_t kRowWeight = 1;
constexpr uint32_t kRowOffset = 0;
constexpr uint32_t kRowCount  = 2;

int       failures       = 0;
long long cycles_checked = 0;

void fail(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::fputs("FAIL  ", stdout);
    std::vfprintf(stdout, fmt, ap);
    va_end(ap);
    ++failures;
}

void clear_flat(FlatVec& v) {
    for (int i = 0; i < kWords; ++i) v[i] = 0;
}

void put_lane(FlatVec& v, int lane, uint32_t value) {
    const int bit   = lane * kValW;
    const int word  = bit / 32;
    const int shift = bit % 32;
    v[word] |= static_cast<uint32_t>(value) << shift;
    if (shift + kValW > 32) {
        v[word + 1] |= static_cast<uint32_t>(value) >> (32 - shift);
    }
}

struct Dut {
    Vbcmc_obs_wb   t;
    VerilatedVcdC* tfp = nullptr;

    void open_trace(const std::string& path) {
        Verilated::traceEverOn(true);
        tfp = new VerilatedVcdC;
        t.trace(tfp, 99);
        tfp->open(path.c_str());
    }

    Dut() {
        t.wb_clk_i = 0;
        t.wb_rst_i = 0;
        t.wb_adr_i = 0;
        t.wb_dat_i = 0;
        t.wb_sel_i = 0;
        t.wb_we_i  = 0;
        t.wb_stb_i = 0;
        t.wb_cyc_i = 0;
        t.obs_n_i     = 0;
        t.obs_c_i     = 0;
        t.obs_valid_i = 0;
        clear_flat(t.obs_weights_flat_i);
        clear_flat(t.obs_offsets_flat_i);
        t.eval();
    }

    // The clock is low while a cycle is observed: the registers are stable, the
    // combinational outputs have settled, and the edge that consumes the
    // inputs has not happened yet.
    void settle() { t.eval(); }

    void edge() {
        t.wb_clk_i = 1;
        t.eval();
        t.wb_clk_i = 0;
        t.eval();
    }

    void reset() {
        t.wb_rst_i = 1;
        t.eval();
        edge();
        edge();
        t.wb_rst_i = 0;
        t.eval();
    }

    // The observation sideband, as the BCMC peripheral drives it.
    void context(uint32_t N, uint32_t C) {
        t.obs_n_i = N;
        t.obs_c_i = C;
        clear_flat(t.obs_weights_flat_i);
        clear_flat(t.obs_offsets_flat_i);
        for (uint32_t lane = 0; lane < kRowCount; ++lane) {
            put_lane(t.obs_weights_flat_i, static_cast<int>(lane), kRowWeight);
            put_lane(t.obs_offsets_flat_i, static_cast<int>(lane), kRowOffset);
        }
    }
};

//---------------------------------------------------------------------------
// The corpus format, as gen_observer_wb_vectors.py writes it
//---------------------------------------------------------------------------

struct Row {
    int      valid = 0, rst = 0, cyc = 0, stb = 0, we = 0;
    uint32_t adr = 0, sel = 0, dat = 0;
    int      ack = 0, err = 0;
    uint32_t rdata = 0;
    int      running = 0, visit_valid = 0;
    uint32_t column = 0;
    int      done = 0, aborted = 0;
    uint64_t bits = 0;
};

struct Run {
    std::string      name;
    uint32_t         N = 0, C = 0, cycles = 0;
    std::vector<Row> rows;
};

// Parses the whole file. A malformed line, a C row before any H row, and a run
// whose row count disagrees with its own H line are all failures: a corpus that
// cannot be read is never something to skip past, and a short read is exactly
// how "the test infrastructure accidentally did nothing" would look.
std::vector<Run> load_obswb(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open " + path);

    std::vector<Run> runs;
    std::string      line;
    int              lineno = 0;

    while (std::getline(in, line)) {
        ++lineno;
        const size_t hash = line.find('#');
        std::string  text = (hash == std::string::npos)
                                ? line : line.substr(0, hash);

        std::istringstream ss(text);
        std::string        tag;
        if (!(ss >> tag)) continue;
        if (tag == "---") continue;

        if (tag == "H") {
            Run         run;
            std::string name;
            uint32_t    N = 0, C = 0, cyc = 0;
            if (!(ss >> name >> N >> C >> cyc)) {
                throw std::runtime_error("line " + std::to_string(lineno) +
                                         ": malformed H line");
            }
            run.name   = name;
            run.N      = N;
            run.C      = C;
            run.cycles = cyc;
            runs.push_back(run);
            continue;
        }

        if (tag == "C") {
            if (runs.empty()) {
                throw std::runtime_error("line " + std::to_string(lineno) +
                                         ": C line before any H line");
            }
            uint32_t valid = 0, rst = 0, cyc = 0, stb = 0, we = 0;
            Row      r;
            if (!(ss >> std::hex >> valid >> rst >> cyc >> stb >> we
                     >> r.adr >> r.sel >> r.dat
                     >> r.ack >> r.err >> r.rdata
                     >> r.running >> r.visit_valid >> r.column
                     >> r.done >> r.aborted >> r.bits)) {
                throw std::runtime_error("line " + std::to_string(lineno) +
                                         ": malformed C line");
            }
            r.valid = static_cast<int>(valid);
            r.rst   = static_cast<int>(rst);
            r.cyc   = static_cast<int>(cyc);
            r.stb   = static_cast<int>(stb);
            r.we    = static_cast<int>(we);
            runs.back().rows.push_back(r);
            continue;
        }

        throw std::runtime_error("line " + std::to_string(lineno) +
                                 ": unknown tag '" + tag + "'");
    }

    if (runs.empty()) throw std::runtime_error("no runs found in " + path);
    for (const Run& r : runs) {
        if (r.rows.size() != r.cycles) {
            throw std::runtime_error(r.name + ": H says " +
                                     std::to_string(r.cycles) +
                                     " cycles, the file has " +
                                     std::to_string(r.rows.size()));
        }
    }
    return runs;
}

// The geometry the corpus was recorded for must fit the widths this build uses,
// or the comparison below would be against the wrong word.
static_assert(kIdxW >= 1, "IDX_W must hold C");

std::string inputs_of(const Row& r) {
    char buf[256];
    std::snprintf(buf, sizeof buf,
                  "valid=%d rst=%d cyc=%d stb=%d we=%d adr=%03x sel=%x "
                  "dat=%08x",
                  r.valid, r.rst, r.cyc, r.stb, r.we, r.adr, r.sel, r.dat);
    return buf;
}

void want_u64(const Run& run, size_t idx, const Row& row, const char* sig,
              uint64_t expect, uint64_t actual) {
    if (expect == actual) return;
    fail("%s cycle %zu: %s is %llu, expected %llu\n      inputs: %s\n",
         run.name.c_str(), idx, sig,
         static_cast<unsigned long long>(actual),
         static_cast<unsigned long long>(expect), inputs_of(row).c_str());
}

void want_u32(const Run& run, size_t idx, const Row& row, const char* sig,
              uint32_t expect, uint32_t actual) {
    want_u64(run, idx, row, sig, expect, actual);
}

// One run: reset, establish the context, then drive each recorded cycle. The
// order per cycle is exactly the corpus's: drive the inputs, let the
// combination settle, compare, and only then take the edge that consumes them.
void replay(Dut& dut, const Run& run) {
    dut.reset();
    dut.context(run.N, run.C);
    dut.settle();

    for (size_t i = 0; i < run.rows.size(); ++i) {
        const Row& row = run.rows[i];

        dut.t.wb_rst_i    = row.rst;
        dut.t.wb_cyc_i    = row.cyc;
        dut.t.wb_stb_i    = row.stb;
        dut.t.wb_we_i     = row.we;
        dut.t.wb_adr_i    = row.adr;
        dut.t.wb_sel_i    = row.sel;
        dut.t.wb_dat_i    = row.dat;
        dut.t.obs_valid_i = row.valid;

        dut.settle();

        want_u32(run, i, row, "wb_ack_o", row.ack, dut.t.wb_ack_o);
        want_u32(run, i, row, "wb_err_o", row.err, dut.t.wb_err_o);
        want_u32(run, i, row, "running_o", row.running, dut.t.running_o);
        want_u32(run, i, row, "visit_valid_o", row.visit_valid,
                 dut.t.visit_valid_o);
        want_u32(run, i, row, "column_o", row.column, dut.t.column_o);
        want_u32(run, i, row, "done_o", row.done, dut.t.done_o);
        want_u32(run, i, row, "aborted_o", row.aborted, dut.t.aborted_o);

        // wb_dat_o is loaded on an accepted access and holds otherwise, so it
        // is only compared where the corpus says the access was acknowledged.
        if (row.ack) {
            want_u32(run, i, row, "wb_dat_o", row.rdata,
                     static_cast<uint32_t>(dut.t.wb_dat_o));
        }

        // The projection is defined for N >= 1 only; see the header.
        if (run.N >= 1) {
            want_u64(run, i, row, "column_bits_o", row.bits,
                     static_cast<uint64_t>(dut.t.column_bits_o));
        }

        ++cycles_checked;
        dut.edge();
    }
}

}  // namespace

//---------------------------------------------------------------------------

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);

    std::vector<std::string> files;
    size_t                   limit = 0;
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
                     "usage: %s [--limit K] [--vcd PATH] <obswb_vectors.txt>...\n",
                     argv[0]);
        return 2;
    }

    Dut dut;
    if (!vcd.empty()) dut.open_trace(vcd);

    size_t runs = 0;
    for (const std::string& file : files) {
        std::vector<Run> loaded;
        try {
            loaded = load_obswb(file);
        } catch (const std::exception& e) {
            std::printf("FAIL  %s\n", e.what());
            return 1;
        }
        if (limit != 0 && loaded.size() > limit) loaded.resize(limit);
        for (const Run& r : loaded) {
            replay(dut, r);
            ++runs;
        }
    }

    // A corpus that yielded no runs is a failure, not a pass: an empty replay
    // is how a wiring mistake becomes a false green.
    if (runs == 0) {
        std::printf("FAIL  no runs found\n");
        return 1;
    }
    if (failures == 0) {
        std::printf("PASS  %zu runs, %lld cycles checked\n", runs,
                    cycles_checked);
        return 0;
    }
    std::printf("FAIL  %d failures\n", failures);
    return 1;
}

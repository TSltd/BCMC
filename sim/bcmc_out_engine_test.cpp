//===========================================================================
// bcmc_out_engine_test.cpp -- the output engine, every cycle
//
// Replays sim/vectors/outeng_edge.txt against rtl/bcmc_out_engine.v, comparing
// every cycle against the values validation/output_engine.py recorded. The
// corpus is the model's record of what the specification said; the model's own
// suite (validation/test_output_engine.py) has already falsified the
// specification twice, so a disagreement here is the RTL's until proved
// otherwise -- and the first question is which of the five things it is: an RTL
// defect, a specification defect, a model defect, a corpus defect, or a harness
// defect.
//
// What is compared, and why it is the whole contract
// -------------------------------------------------
// `pins_o`, every cycle. That is not a partial comparison dressed up as a
// thorough one: it is the entire observable interface of this block. There is
// one output, and the corpus carries its expected value for every cycle the
// block is driven. `pattern_q` and `valid_q` are model state and are NOT
// carried by the corpus and NOT exported by the module -- adding a port to make
// the comparison easier would replace a contract check with an implementation
// check, and the block's whole point is that its interface is small.
//
// What the corpus carries, and what makes it an external artifact rather than a
// dump of the model:
//
//   * the stimulus -- `rst`, `valid`, `visit_valid`, `column_bits`, and `C` from
//     the run header -- so a replay needs no Python;
//   * the expected `pins_o` per cycle;
//   * the mandatory cases of section 10.4, checked by the generator after it
//     writes the file, including the two the model's mutation battery proved
//     load-bearing: a NON-ZERO held pattern, and an invalidation while something
//     is actually being driven.
//
// The generator additionally checks that this corpus *discriminates*: each of
// the five wiring faults of section 10.6 that the model cannot plant must differ
// from the corpus somewhere. So the corpus's teeth are established before this
// harness runs, and a green here is a green against a corpus that can fail.
//
// Geometry
// --------
// Passed to Verilator (-G) and to this translation unit (BCMC_OUT_*) from
// sim/CMakeLists.txt so the two cannot drift, and matching
// validation/output_engine.py's REF_MAX_C and rtl/bcmc_out_engine.v's defaults.
//
// Usage:
//     bcmc_out_engine_test [--limit K] [--vcd PATH] <outeng_vectors.txt>...
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

#include "Vbcmc_out_engine.h"
#include "verilated_vcd_c.h"

namespace {

#ifndef BCMC_OUT_MAX_C
#define BCMC_OUT_MAX_C 32
#endif
#ifndef BCMC_OUT_IDX_W
#define BCMC_OUT_IDX_W 16
#endif

constexpr int kMaxC = BCMC_OUT_MAX_C;
constexpr int kIdxW = BCMC_OUT_IDX_W;

// A pattern wider than the C++ comparison value would need a size the corpus
// format does not carry; 64 is the width of the value it does.
static_assert(kMaxC <= 64, "the corpus format carries a 64-bit pattern");

int       failures      = 0;
long long cycles_checked = 0;

void fail(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::fputs("FAIL  ", stdout);
    std::vfprintf(stdout, fmt, ap);
    va_end(ap);
    ++failures;
}

struct Dut {
    Vbcmc_out_engine t;
    VerilatedVcdC*   tfp = nullptr;

    void open_trace(const std::string& path) {
        Verilated::traceEverOn(true);
        tfp = new VerilatedVcdC;
        t.trace(tfp, 99);
        tfp->open(path.c_str());
    }

    Dut() {
        t.clk          = 0;
        t.rst          = 0;
        t.column_bits_i = 0;
        t.visit_valid_i = 0;
        t.valid_i       = 0;
        t.C_i           = 0;
        t.eval();
    }

    // The clock is low while a cycle is observed: the registers are stable, the
    // combinational outputs have settled, and the edge that consumes the inputs
    // has not happened yet.
    void settle() { t.eval(); }

    void edge() {
        t.clk = 1;
        t.eval();
        t.clk = 0;
        t.eval();
    }

    // Between runs the device is reset, with the context low: the corpus records
    // rst = 0 on ordinary cycles, so the harness owns the run boundary, and
    // `valid_q` must start at zero or the first cycle of a run would look like a
    // revalidation.
    void reset() {
        t.rst          = 1;
        t.valid_i      = 0;
        t.visit_valid_i = 0;
        t.column_bits_i = 0;
        t.eval();
        edge();
        edge();
        t.rst = 0;
        t.eval();
    }
};

static_assert(kIdxW >= 1, "IDX_W must be at least 1");

//---------------------------------------------------------------------------
// The corpus format, as gen_out_engine_vectors.py writes it
//---------------------------------------------------------------------------

struct Row {
    int      rst = 0, valid = 0, visit_valid = 0;
    uint64_t bits = 0;
    uint64_t pins = 0;
};

struct Run {
    std::string      name;
    uint32_t         N = 0, C = 0, cycles = 0;
    std::vector<Row> rows;
};

// A malformed line, a C row before any H row, and a run whose row count
// disagrees with its own header are all failures: a corpus that cannot be read
// is never something to skip past, and a short read is exactly how "the test
// infrastructure did nothing" would look.
std::vector<Run> load_outeng(const std::string& path) {
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
            Row r;
            int rst = 0, valid = 0, vv = 0;
            if (!(ss >> std::dec >> rst >> valid >> vv)) {
                throw std::runtime_error("line " + std::to_string(lineno) +
                                         ": malformed C line");
            }
            if (!(ss >> std::hex >> r.bits >> r.pins)) {
                throw std::runtime_error("line " + std::to_string(lineno) +
                                         ": malformed C line (bits or pins)");
            }
            r.rst         = rst;
            r.valid       = valid;
            r.visit_valid = vv;
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

std::string inputs_of(const Row& r, const Run& run) {
    char buf[192];
    std::snprintf(buf, sizeof buf,
                  "rst=%d valid=%d visit_valid=%d column_bits=%llx C=%u",
                  r.rst, r.valid, r.visit_valid,
                  static_cast<unsigned long long>(r.bits), run.C);
    return buf;
}

// One run: reset, then drive each recorded cycle. The order per cycle is the
// corpus's: drive the inputs, let the combination settle, compare, and only then
// take the edge that consumes them.
void replay(Dut& dut, const Run& run) {
    dut.reset();

    for (size_t i = 0; i < run.rows.size(); ++i) {
        const Row& row = run.rows[i];

        dut.t.rst           = row.rst;
        dut.t.valid_i       = row.valid;
        dut.t.visit_valid_i = row.visit_valid;
        dut.t.column_bits_i = row.bits;
        dut.t.C_i           = run.C;
        dut.settle();

        const uint64_t got = static_cast<uint64_t>(dut.t.pins_o);
        if (got != row.pins) {
            fail("%s cycle %zu: pins_o is %llx, expected %llx\n"
                 "      inputs: %s\n",
                 run.name.c_str(), i,
                 static_cast<unsigned long long>(got),
                 static_cast<unsigned long long>(row.pins),
                 inputs_of(row, run).c_str());
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
                     "usage: %s [--limit K] [--vcd PATH] <outeng_vectors.txt>...\n",
                     argv[0]);
        return 2;
    }

    Dut dut;
    if (!vcd.empty()) dut.open_trace(vcd);

    size_t runs = 0;
    for (const std::string& file : files) {
        std::vector<Run> loaded;
        try {
            loaded = load_outeng(file);
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

    // A corpus that yielded no runs is a failure, not a pass: an empty replay is
    // how a wiring mistake becomes a false green.
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

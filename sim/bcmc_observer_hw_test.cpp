//===========================================================================
// bcmc_observer_hw_test.cpp -- the observer engine, every cycle
//
// This harness replays sim/vectors/obshw_*.txt against rtl/bcmc_observer.v,
// comparing EVERY cycle rather than only the emitted visits: running, column,
// visit_valid, done and aborted, on each clock, against the values
// validation/observer_hw.py produced. The model is the referee; the file is
// the record of what it said.
//
// The name is bcmc_observer_hw_test.cpp because sim/bcmc_observer_test.cpp is
// already the *software* observer's conformance test, and two different claims
// should not share a name.
//
// What is compared, and what cannot be
// ------------------------------------
// Six of the seven recorded fields are module outputs and are compared every
// cycle: running, column, visit_valid, done, aborted (and the duplicated
// `running`, which the vector writes twice so that a harness confusing the
// state bit with the output bit is caught rather than flattered).
//
// The seventh, `scheduled` (the registered strobe visit_q), is NOT a port.
// The document deliberately does not export it, and rather than add a port to
// a frozen interface this harness checks it exactly where it is observable:
// when `valid` is high, visit_q and visit_valid are the same signal by
// construction, so `visit_valid == scheduled` is asserted there. When `valid`
// is low the strobe is unobservable from outside -- which is precisely the
// point of section 3.7, and the suppression it causes is checked through its
// consequences instead: no visit is presented, the pass aborts, and no
// subsequent visit occurs without a new start.
//
// Geometry
// --------
// The engine holds no geometry-dependent state: it counts to N and drives a
// column. The one geometry-dependent part is the internal bcmc_column
// instance, and its own harness (sim/bcmc_column_test.cpp) already holds that
// module to reference.py exhaustively. Here it is exercised at the RTL's
// default parameters, and the wiring is checked by requiring column_bits to
// reproduce the R lines of matrix_*.txt for a real context.
//
// Usage:
//     bcmc_observer_hw_test [--limit K] [--vcd PATH] <obshw_vectors.txt>...
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

#include "Vbcmc_observer.h"
#include "vectors.h"
#include "verilated_vcd_c.h"

namespace {

// The geometry comes from sim/CMakeLists.txt, which passes the same numbers to
// Verilator (-G) and to this translation unit (BCMC_OBS_*), so the two cannot
// drift. The defaults match rtl/bcmc_observer.v for a hand build.
#ifndef BCMC_OBS_VAL_W
#define BCMC_OBS_VAL_W 16
#endif
#ifndef BCMC_OBS_MAX_C
#define BCMC_OBS_MAX_C 32
#endif

constexpr int kValW = BCMC_OBS_VAL_W;
constexpr int kMaxC = BCMC_OBS_MAX_C;
constexpr int kWords = (kMaxC * kValW + 31) / 32;

using FlatVec = VlWide<kWords>;

int failures = 0;
long long cycles_checked = 0;

void fail(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::fputs("FAIL  ", stdout);
    std::vfprintf(stdout, fmt, ap);
    va_end(ap);
    ++failures;
}

//---------------------------------------------------------------------------
// The observation sideband, as the wrapper will drive it. For the engine runs
// the matrix is irrelevant -- a zero context is a legal one, and every visit
// then asks a question whose answer is zero -- so it is left at zero.
//---------------------------------------------------------------------------

void clear_flat(FlatVec& v) {
    for (int i = 0; i < kWords; ++i) v[i] = 0;
}

void put_lane(FlatVec& v, int lane, uint32_t value) {
    const int bit = lane * kValW;
    const int word = bit / 32;
    const int shift = bit % 32;
    v[word] |= static_cast<uint32_t>(value) << shift;
    if (shift + kValW > 32) {
        v[word + 1] |= static_cast<uint32_t>(value) >> (32 - shift);
    }
}

struct Dut {
    Vbcmc_observer t;

    VerilatedVcdC* tfp = nullptr;

    void open_trace(const std::string& path) {
        Verilated::traceEverOn(true);
        tfp = new VerilatedVcdC;
        t.trace(tfp, 99);
        tfp->open(path.c_str());
    }

    Dut() {
        t.clk = 0;
        t.rst = 0;
        t.start = 0;
        t.trigger = 0;
        t.oneshot = 0;
        t.valid = 0;
        t.N = 0;
        t.C = 0;
        for (int i = 0; i < kWords; ++i) {
            t.weights_flat[i] = 0;
            t.offsets_flat[i] = 0;
        }
        t.eval();
    }

    // The clock is held low while a cycle is observed: the registers are
    // stable, the combinational outputs have settled, and the rising edge that
    // consumes the inputs has not happened yet.
    // v2.0c: the engine no longer computes the traversal -- it asks a source
    // through the seam, `ts_t` out and `ts_pi` in. This harness drives the engine
    // as its top, and a C++ harness cannot instantiate a second Verilog module,
    // so the identity is supplied here, one line before every evaluation. The
    // Icarus bench instantiates the real `bcmc_src_identity` module instead,
    // which is the stronger of the two; this is the mechanical half.
    void seam() { t.ts_pi = t.ts_t; }

    void settle() { seam(); t.eval(); }

    void edge() {
        seam();
        t.clk = 1;
        t.eval();
        seam();
        t.clk = 0;
        t.eval();
    }

    void reset() {
        t.rst = 1;
        seam();
        t.eval();
        edge();
        edge();
        t.rst = 0;
        seam();
        t.eval();
    }
};

//---------------------------------------------------------------------------
// The vector format, as gen_observer_hw_vectors.py writes it
//---------------------------------------------------------------------------

struct Cyc {
    int      rst = 0, start = 0, trigger = 0, valid = 0;
    int      running = 0, running_dup = 0;
    uint32_t column = 0;
    int      scheduled = 0, visit_valid = 0, done = 0, aborted = 0;
};

struct Run {
    std::string      name;
    uint32_t         N = 0, oneshot = 0, cycles = 0;
    std::vector<Cyc> cyc;
};

std::vector<Run> load_obshw(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open " + path);

    std::vector<Run> runs;
    std::string      line;
    std::string      pending;
    int              lineno = 0;

    while (std::getline(in, line)) {
        ++lineno;
        const size_t hash = line.find('#');
        std::string  text = (hash == std::string::npos) ? line : line.substr(0, hash);

        std::istringstream ss(text);
        std::string        tag;
        if (!(ss >> tag)) {
            if (hash != std::string::npos) {
                std::string name = line.substr(hash + 1);
                while (!name.empty() && name.front() == ' ') name.erase(0, 1);
                if (!name.empty()) pending = name;
            }
            continue;
        }
        if (tag == "---") continue;

        if (tag == "H") {
            Run r;
            r.name = pending;
            pending.clear();
            if (!(ss >> r.N >> r.oneshot >> r.cycles)) {
                throw std::runtime_error(path + ":" + std::to_string(lineno) +
                                         ": malformed H line");
            }
            runs.push_back(r);
        } else if (tag == "C") {
            if (runs.empty()) {
                throw std::runtime_error(path + ":" + std::to_string(lineno) +
                                         ": C line before any H line");
            }
            Cyc c;
            if (!(ss >> c.rst >> c.start >> c.trigger >> c.valid >> c.running >>
                  c.running_dup >> c.column >> c.scheduled >> c.visit_valid >>
                  c.done >> c.aborted)) {
                throw std::runtime_error(path + ":" + std::to_string(lineno) +
                                         ": malformed C line");
            }
            runs.back().cyc.push_back(c);
        } else {
            throw std::runtime_error(path + ":" + std::to_string(lineno) +
                                     ": unknown tag '" + tag + "'");
        }
    }
    return runs;
}

//---------------------------------------------------------------------------
// Replaying one run, every cycle
//---------------------------------------------------------------------------

void replay(Dut& dut, const Run& run) {
    if (run.cyc.size() != run.cycles) {
        fail("%s: H says %u cycles but the file has %zu", run.name.c_str(),
             run.cycles, run.cyc.size());
        return;
    }

    dut.reset();
    clear_flat(dut.t.weights_flat);
    clear_flat(dut.t.offsets_flat);
    dut.t.C = 0;

    for (size_t k = 0; k < run.cyc.size(); ++k) {
        const Cyc& c = run.cyc[k];

        dut.t.rst     = c.rst != 0;
        dut.t.start   = c.start != 0;
        dut.t.trigger = c.trigger != 0;
        dut.t.valid   = c.valid != 0;
        dut.t.oneshot = run.oneshot != 0;
        dut.t.N       = run.N;
        dut.settle();
        ++cycles_checked;

        const uint32_t running     = dut.t.running;
        const uint32_t column      = dut.t.column;
        const uint32_t visit_valid = dut.t.visit_valid;
        const uint32_t done        = dut.t.done;
        const uint32_t aborted     = dut.t.aborted;

        if (running != static_cast<uint32_t>(c.running) ||
            running != static_cast<uint32_t>(c.running_dup) ||
            column != c.column ||
            visit_valid != static_cast<uint32_t>(c.visit_valid) ||
            done != static_cast<uint32_t>(c.done) ||
            aborted != static_cast<uint32_t>(c.aborted)) {
            fail("%s cycle %zu: RTL {run=%u col=%u visit=%u done=%u abort=%u}"
                 "  model {run=%d col=%u visit=%d done=%d abort=%d}",
                 run.name.c_str(), k, running, column, visit_valid, done, aborted,
                 c.running, c.column, c.visit_valid, c.done, c.aborted);
            return;   // one line per run is enough to see the disagreement
        }

        // visit_q is not a port. It is observable exactly when `valid` is high,
        // and there it must equal visit_valid; see the header.
        if (c.valid && visit_valid != static_cast<uint32_t>(c.scheduled)) {
            fail("%s cycle %zu: visit_valid %u != scheduled %d while valid",
                 run.name.c_str(), k, visit_valid, c.scheduled);
            return;
        }

        dut.edge();
    }
}

//---------------------------------------------------------------------------
// The internal bcmc_column instance, on a real context
//
// The engine does not know the matrix, but the block contains one projection.
// Loading a matrix_*.txt case and requiring column_bits to reproduce its R
// lines checks that the observer drives the projection correctly -- the wiring
// claim, not the projection's mathematics, which bcmc_column_test.cpp owns.
//---------------------------------------------------------------------------

void check_projection(Dut& dut, const bcmc::MatrixCase& m) {
    if (m.N < 1 || m.N > 0xFFFFu || m.C > static_cast<uint32_t>(kMaxC)) return;

    dut.reset();
    dut.t.N = m.N;
    dut.t.C = m.C;
    dut.t.oneshot = 0;
    dut.t.valid = 1;
    clear_flat(dut.t.weights_flat);
    clear_flat(dut.t.offsets_flat);
    for (uint32_t i = 0; i < static_cast<uint32_t>(kMaxC); ++i) {
        const uint32_t w = (i < m.C) ? m.weights[i] : 0;
        const uint32_t o = (i < m.C) ? m.offsets[i] : 0;
        put_lane(dut.t.weights_flat, static_cast<int>(i), w);
        put_lane(dut.t.offsets_flat, static_cast<int>(i), o);
    }

    dut.t.start = 1;
    dut.t.trigger = 0;
    dut.settle();
    dut.edge();

    for (uint32_t step = 0; step <= m.N; ++step) {
        dut.settle();
        if (dut.t.visit_valid) {
            const uint32_t col = dut.t.column;
            for (uint32_t i = 0; i < m.C; ++i) {
                const uint32_t bit = (dut.t.column_bits >> i) & 1u;
                const uint32_t want = m.rows[i][col];
                if (bit != want) {
                    fail("projection N=%u C=%u col %u row %u: RTL %u, file %u",
                         m.N, m.C, col, i, bit, want);
                    return;
                }
                ++cycles_checked;
            }
        }
        dut.t.start = 0;
        dut.t.trigger = 1;
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
                     "usage: %s [--limit K] [--vcd PATH]"
                     " <obshw_vectors.txt | matrix_vectors.txt>...\n",
                     argv[0]);
        return 2;
    }

    Dut dut;
    if (!vcd.empty()) dut.open_trace(vcd);

    size_t total_runs = 0;
    size_t total_cases = 0;

    for (const std::string& file : files) {
        // A suite whose first tag is `N` is a matrix file; one whose first tag
        // is `H` is an observer run. They are read by different readers and
        // checked differently, so the kind is decided before either is tried.
        bool is_matrix = false;
        {
            std::ifstream probe(file);
            std::string   line;
            while (std::getline(probe, line)) {
                const size_t hash = line.find('#');
                std::string  text = (hash == std::string::npos)
                                        ? line : line.substr(0, hash);
                std::istringstream ss(text);
                std::string        tag;
                if (ss >> tag) {
                    is_matrix = (tag == "N");
                    break;
                }
            }
        }

        if (is_matrix) {
            std::vector<bcmc::MatrixCase> cases;
            try {
                cases = bcmc::load_matrix_vectors(file);
            } catch (const std::exception& e) {
                std::printf("FAIL  %s\n", e.what());
                return 1;
            }
            if (limit != 0 && cases.size() > limit) cases.resize(limit);
            for (const bcmc::MatrixCase& m : cases) check_projection(dut, m);
            total_cases += cases.size();
        } else {
            std::vector<Run> runs;
            try {
                runs = load_obshw(file);
            } catch (const std::exception& e) {
                std::printf("FAIL  %s\n", e.what());
                return 1;
            }
            if (limit != 0 && runs.size() > limit) runs.resize(limit);
            for (const Run& r : runs) {
                replay(dut, r);
                ++total_runs;
            }
        }
    }

    if (failures == 0) {
        std::printf("PASS  %zu runs, %zu matrices, %lld cycles/rows checked\n",
                    total_runs, total_cases, cycles_checked);
        return 0;
    }
    std::printf("FAIL  %d failures\n", failures);
    return 1;
}

//===========================================================================
// bcmc_core_test.cpp -- RTL == Python for the BCMC Core
//
// This testbench never invents an expected answer. Every case is checked two
// independent ways:
//
//   1. Against Python. The offset stream must equal, element for element, the
//      output of validation/reference.py's bcmc_core(weights, N), as recorded
//      in the vector files by validation/gen_vectors.py.
//
//   2. Against the defining recurrence, recomputed here with a real `%`
//      operator and no reference to Python at all:
//
//          o[0]   = 0
//          o[i+1] = (o[i] + w[i]) mod N
//
//      If the Python model, the C++ recurrence and the RTL ever disagree, the
//      disagreement is reported rather than papered over.
//
// It also checks the BCMC Prefix Stream Interface protocol itself (see
// docs/Hardware_Architecture.md):
//
//   * exactly C offsets are emitted, never more, never fewer;
//   * offset_valid is never asserted outside a transform;
//   * done is a single-cycle pulse that occurs strictly after the last offset;
//   * busy and done are never asserted together;
//   * C = 0 completes with no offsets at all;
//   * gap invariance -- the sequence of accepted weights alone determines the
//     output sequence. Every case is replayed with back-to-back weights and
//     with 1, 3 and randomised idle cycles between them; all four runs must
//     produce byte-identical offset streams.
//
// Usage:
//     bcmc_core_test [--limit K] [--vcd PATH] vectors.txt [vectors.txt ...]
//===========================================================================

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <random>
#include <string>
#include <vector>

#include "Vbcmc_core.h"
#include "verilated.h"
#include "vectors.h"

#if VM_TRACE
#include "verilated_vcd_c.h"
#endif

namespace {

//---------------------------------------------------------------------------
// The device under test, plus a clock
//---------------------------------------------------------------------------

class Dut {
  public:
    Dut() : dut_(new Vbcmc_core) {}

    ~Dut() {
#if VM_TRACE
        if (trace_) {
            trace_->close();
            delete trace_;
        }
#endif
        dut_->final();
        delete dut_;
    }

    Vbcmc_core* operator->() { return dut_; }

    void open_trace(const std::string& path) {
#if VM_TRACE
        Verilated::traceEverOn(true);
        trace_ = new VerilatedVcdC;
        dut_->trace(trace_, 99);
        trace_->open(path.c_str());
#else
        (void)path;
        std::fprintf(stderr,
                     "note: built without tracing; --vcd ignored\n");
#endif
    }

    // One full clock period. Outputs read after tick() are the values the DUT
    // presents during the following cycle.
    void tick() {
        dut_->clk = 0;
        dut_->eval();
        dump();
        ++time_;

        dut_->clk = 1;
        dut_->eval();
        dump();
        ++time_;
    }

    uint64_t time() const { return time_; }

  private:
    void dump() {
#if VM_TRACE
        if (trace_) trace_->dump(static_cast<vluint64_t>(time_));
#endif
    }

    Vbcmc_core* dut_ = nullptr;
    uint64_t    time_ = 0;
#if VM_TRACE
    VerilatedVcdC* trace_ = nullptr;
#endif
};

//---------------------------------------------------------------------------
// Result of driving one case through the DUT
//---------------------------------------------------------------------------

struct RunResult {
    std::vector<uint32_t> offsets;
    std::string           error;      // empty == protocol respected
    bool ok() const { return error.empty(); }
};

// How idle cycles are inserted between accepted weights. The values must not
// matter -- that is the property under test.
enum class Gaps { None, One, Three, Random };

const char* gaps_name(Gaps g) {
    switch (g) {
        case Gaps::None:   return "gap=0";
        case Gaps::One:    return "gap=1";
        case Gaps::Three:  return "gap=3";
        case Gaps::Random: return "gap=random";
    }
    return "gap=?";
}

//---------------------------------------------------------------------------
// Drive one case
//---------------------------------------------------------------------------

RunResult run_case(Dut& dut, const bcmc::Case& c, Gaps gaps, std::mt19937& rng) {
    RunResult r;

    std::uniform_int_distribution<int> gap_dist(0, 4);
    const auto next_gap = [&]() -> int {
        switch (gaps) {
            case Gaps::None:   return 0;
            case Gaps::One:    return 1;
            case Gaps::Three:  return 3;
            case Gaps::Random: return gap_dist(rng);
        }
        return 0;
    };

    // Reset. N and C are held stable for the whole transform; the DUT samples
    // them at start.
    dut->rst          = 1;
    dut->start        = 0;
    dut->weight_valid = 0;
    dut->weight_in    = 0;
    dut->N            = c.N;
    dut->C            = c.C;
    dut.tick();
    dut.tick();
    dut->rst = 0;
    dut.tick();  // one idle cycle in ST_IDLE

    if (dut->busy || dut->done || dut->offset_valid) {
        r.error = "not quiescent after reset";
        return r;
    }

    // Start. After this tick the DUT is in its first busy cycle.
    dut->start = 1;
    dut.tick();
    dut->start = 0;

    std::size_t wi        = 0;
    int         wait_cyc  = next_gap();
    bool        done_seen = false;

    // Generous bound: every weight needs at most one cycle plus its gap, and
    // framing costs a handful more.
    const uint64_t guard =
        static_cast<uint64_t>(c.C) * 8 + 64;

    for (uint64_t k = 0; k < guard; ++k) {
        // --- sample the current cycle -------------------------------------
        const bool     busy  = dut->busy != 0;
        const bool     done  = dut->done != 0;
        const bool     ovld  = dut->offset_valid != 0;
        const uint32_t oval  = dut->offset_out;

        if (busy && done) {
            r.error = "busy and done asserted in the same cycle";
            return r;
        }

        if (ovld) {
            if (!busy) {
                r.error = "offset_valid asserted while not busy";
                return r;
            }
            if (r.offsets.size() >= c.C) {
                r.error = "more than C offsets emitted";
                return r;
            }
            r.offsets.push_back(oval);
        }

        if (done) {
            if (r.offsets.size() != c.C) {
                char buf[128];
                std::snprintf(buf, sizeof(buf),
                              "done asserted after %zu of %u offsets",
                              r.offsets.size(), c.C);
                r.error = buf;
                return r;
            }
            done_seen = true;
        }

        // --- drive the current cycle ---------------------------------------
        bool send = false;
        if (busy && wi < c.C) {
            if (wait_cyc > 0) {
                --wait_cyc;
            } else {
                send = true;
            }
        }
        dut->weight_valid = send ? 1 : 0;
        dut->weight_in    = send ? c.weights[wi] : 0;

        dut.tick();

        if (send) {
            ++wi;
            wait_cyc = next_gap();
        }

        if (done_seen) break;
    }

    dut->weight_valid = 0;
    dut->weight_in    = 0;

    if (!done_seen) {
        r.error = "done never asserted";
        return r;
    }
    if (wi != c.C) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "only %zu of %u weights were accepted",
                      wi, c.C);
        r.error = buf;
        return r;
    }

    // The transform must be quiet once it is over: no trailing offsets, no
    // second done pulse.
    for (int k = 0; k < 8; ++k) {
        if (dut->offset_valid) {
            r.error = "offset_valid asserted after done";
            return r;
        }
        if (dut->done) {
            r.error = "done asserted for more than one cycle";
            return r;
        }
        if (dut->busy) {
            r.error = "busy still asserted after done";
            return r;
        }
        dut.tick();
    }

    return r;
}

//---------------------------------------------------------------------------
// The defining recurrence, computed here and nowhere else
//---------------------------------------------------------------------------

std::vector<uint32_t> recurrence(const bcmc::Case& c) {
    std::vector<uint32_t> o;
    o.reserve(c.C);
    uint32_t x = 0;
    for (uint32_t w : c.weights) {
        o.push_back(x);
        x = (x + w) % c.N;   // a real modulo, not the RTL's conditional subtract
    }
    return o;
}

//---------------------------------------------------------------------------
// Reporting
//---------------------------------------------------------------------------

std::string join(const std::vector<uint32_t>& v, std::size_t limit = 16) {
    std::string s = "[";
    for (std::size_t i = 0; i < v.size() && i < limit; ++i) {
        if (i) s += ", ";
        s += std::to_string(v[i]);
    }
    if (v.size() > limit) s += ", ...";
    s += "]";
    return s;
}

void describe(const std::string& path, const bcmc::Case& c) {
    std::fprintf(stderr, "  at %s:%d   N = %u, C = %u\n", path.c_str(), c.line,
                 c.N, c.C);
    std::fprintf(stderr, "  weights  %s\n", join(c.weights).c_str());
}

}  // namespace

//---------------------------------------------------------------------------

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);

    std::vector<std::string> files;
    std::string              vcd;
    std::size_t              limit = 0;  // 0 == no limit

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--vcd" && i + 1 < argc) {
            vcd = argv[++i];
        } else if (arg == "--limit" && i + 1 < argc) {
            limit = static_cast<std::size_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (arg.rfind("--", 0) == 0 || arg.rfind("+", 0) == 0) {
            // Verilator's own arguments (+verilator+...) and unknown flags.
            continue;
        } else {
            files.push_back(arg);
        }
    }

    if (files.empty()) {
        std::fprintf(stderr,
                     "usage: bcmc_core_test [--limit K] [--vcd PATH] "
                     "vectors.txt [vectors.txt ...]\n");
        return 2;
    }

    Dut dut;
    if (!vcd.empty()) dut.open_trace(vcd);

    const Gaps modes[] = {Gaps::None, Gaps::One, Gaps::Three, Gaps::Random};

    std::size_t total_cases = 0;
    std::size_t total_runs  = 0;
    std::size_t total_rows  = 0;
    std::size_t failures    = 0;

    for (const std::string& path : files) {
        std::vector<bcmc::Case> cases;
        try {
            cases = bcmc::load_vectors(path);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "FAIL  %s\n", e.what());
            return 1;
        }

        std::size_t n_here = 0;

        for (const bcmc::Case& c : cases) {
            if (limit && n_here >= limit) break;
            ++n_here;
            ++total_cases;
            total_rows += c.C;

            // The recurrence, computed independently of Python and of the RTL.
            const std::vector<uint32_t> recur = recurrence(c);

            if (recur != c.offsets) {
                std::fprintf(stderr,
                             "FAIL  vector file disagrees with the recurrence\n");
                describe(path, c);
                std::fprintf(stderr, "  file       %s\n", join(c.offsets).c_str());
                std::fprintf(stderr, "  recurrence %s\n", join(recur).c_str());
                ++failures;
                continue;
            }
            if (c.C > 0 && recur[0] != 0) {
                std::fprintf(stderr, "FAIL  offset[0] != 0\n");
                describe(path, c);
                ++failures;
                continue;
            }

            // Deterministic per-case seed so a failure can be reproduced.
            std::mt19937          rng(0x42434D43u ^ static_cast<uint32_t>(c.line));
            std::vector<uint32_t> reference;
            bool                  first = true;
            bool                  bad   = false;

            for (Gaps g : modes) {
                const RunResult r = run_case(dut, c, g, rng);
                ++total_runs;

                if (!r.ok()) {
                    std::fprintf(stderr, "FAIL  protocol: %s (%s)\n",
                                 r.error.c_str(), gaps_name(g));
                    describe(path, c);
                    bad = true;
                    break;
                }
                if (r.offsets.size() != c.C) {
                    std::fprintf(stderr,
                                 "FAIL  %zu offsets, expected %u (%s)\n",
                                 r.offsets.size(), c.C, gaps_name(g));
                    describe(path, c);
                    bad = true;
                    break;
                }
                // 1. RTL == Python.
                if (r.offsets != c.offsets) {
                    std::fprintf(stderr, "FAIL  RTL disagrees with Python (%s)\n",
                                 gaps_name(g));
                    describe(path, c);
                    std::fprintf(stderr, "  python %s\n", join(c.offsets).c_str());
                    std::fprintf(stderr, "  rtl    %s\n", join(r.offsets).c_str());
                    bad = true;
                    break;
                }
                // 2. RTL == the recurrence, asserted independently.
                if (r.offsets != recur) {
                    std::fprintf(stderr,
                                 "FAIL  RTL disagrees with the recurrence (%s)\n",
                                 gaps_name(g));
                    describe(path, c);
                    bad = true;
                    break;
                }
                // Offsets must be residues.
                for (std::size_t i = 0; i < r.offsets.size(); ++i) {
                    if (r.offsets[i] >= c.N) {
                        std::fprintf(stderr,
                                     "FAIL  offset[%zu] = %u is not less than N = %u\n",
                                     i, r.offsets[i], c.N);
                        describe(path, c);
                        bad = true;
                        break;
                    }
                }
                if (bad) break;

                // 3. Gap invariance.
                if (first) {
                    reference = r.offsets;
                    first     = false;
                } else if (r.offsets != reference) {
                    std::fprintf(stderr,
                                 "FAIL  idle cycles changed the output (%s)\n",
                                 gaps_name(g));
                    describe(path, c);
                    bad = true;
                    break;
                }
            }

            if (bad) ++failures;
        }

        std::printf("  %-24s %6zu cases\n", path.c_str(), n_here);
    }

    std::printf("\n%s  %zu cases, %zu runs, %zu rows, %zu failures\n",
                failures ? "FAIL" : "PASS", total_cases, total_runs, total_rows,
                failures);

    return failures ? 1 : 0;
}

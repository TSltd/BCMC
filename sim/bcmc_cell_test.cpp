//===========================================================================
// bcmc_cell_test.cpp -- RTL == Python, for the BCMC characteristic function
//
// The claim this program establishes:
//
//     rtl/bcmc_cell.v computes exactly validation/reference.py's bcmc_cell()
//
// and for N <= 12 it establishes that claim EXHAUSTIVELY -- every legal
// (N, weight, offset, column) tuple, not a sample of them. The cell's input
// space is finite and small, so it can be enumerated rather than explored.
// That is a stronger statement than is available for any other module here.
//
// Five independent checks are applied:
//
//   1. Preconditions.   The testbench owns them, because bcmc_cell.v
//                       deliberately contains no assertions: a combinational
//                       module has no clock edge on which to check safely.
//   2. RTL == Python.   The bit in the vector file came from reference.py.
//   3. RTL == oracle.   An independent formula written here in C++, using a
//                       real `%` operator rather than the RTL's conditional
//                       add. If both the RTL and this oracle are wrong they
//                       must be wrong in the same way, having been derived
//                       differently.
//   4. bit is boolean.  Exactly 0 or 1.
//   5. Statelessness.   Every case is evaluated in three different orders --
//                       forwards, backwards and shuffled -- and must give the
//                       same answer every time. For a combinational module
//                       this is the analogue of the Core's gap invariance: it
//                       is how "no history" is tested rather than asserted.
//
// Usage:
//     bcmc_cell_test [--limit K] [--sweep K] [--vcd PATH] <vectors.txt>...
//
//     --sweep K   additionally enumerate every legal tuple for N <= K and
//                 check the RTL against the oracle. No vector file is needed
//                 for this, so K can be far larger than what is worth
//                 committing to git.
//===========================================================================

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <random>
#include <string>
#include <vector>

#include "Vbcmc_cell.h"
#include "verilated.h"
#include "vectors.h"

#if VM_TRACE
#include "verilated_vcd_c.h"
#endif

namespace {

//---------------------------------------------------------------------------
// The device under test
//---------------------------------------------------------------------------

class Dut {
  public:
    Dut() : top_(new Vbcmc_cell) {}

    ~Dut() {
#if VM_TRACE
        if (trace_) {
            trace_->close();
            delete trace_;
        }
#endif
        top_->final();
        delete top_;
    }

    Vbcmc_cell* operator->() { return top_; }

    void open_trace(const std::string& path) {
#if VM_TRACE
        Verilated::traceEverOn(true);
        trace_ = new VerilatedVcdC;
        top_->trace(trace_, 99);
        trace_->open(path.c_str());
#else
        (void)path;
        std::fprintf(stderr, "warning: built without tracing; --vcd ignored\n");
#endif
    }

    // The whole of "running" a combinational module.
    uint32_t evaluate(uint32_t N, uint32_t weight, uint32_t offset,
                      uint32_t column) {
        top_->N      = N;
        top_->weight = weight;
        top_->offset = offset;
        top_->column = column;
        top_->eval();
#if VM_TRACE
        if (trace_) trace_->dump(time_);
#endif
        time_ += 10;
        return top_->bit_out;
    }

  private:
    Vbcmc_cell* top_ = nullptr;
#if VM_TRACE
    VerilatedVcdC* trace_ = nullptr;
#endif
    uint64_t time_ = 0;
};

//---------------------------------------------------------------------------
// The independent oracle
//
// Deliberately NOT the RTL's algorithm. The RTL conditionally adds N; this
// uses a real modulo on a signed difference. Two derivations, one answer.
//---------------------------------------------------------------------------

uint32_t oracle(uint32_t N, uint32_t weight, uint32_t offset, uint32_t column) {
    const int64_t n     = static_cast<int64_t>(N);
    const int64_t diff  = static_cast<int64_t>(column) - static_cast<int64_t>(offset);
    const int64_t delta = ((diff % n) + n) % n;   // a real modulo, both signs
    return (delta < static_cast<int64_t>(weight)) ? 1u : 0u;
}

//---------------------------------------------------------------------------
// Preconditions. The testbench owns these; see the header of bcmc_cell.v.
//---------------------------------------------------------------------------

std::string precondition_error(uint32_t N, uint32_t weight, uint32_t offset,
                               uint32_t column) {
    char buf[192];
    if (N < 1) {
        std::snprintf(buf, sizeof buf, "precondition N >= 1 violated (N = %u)", N);
        return buf;
    }
    if (weight > N) {
        std::snprintf(buf, sizeof buf,
                      "precondition weight <= N violated (weight = %u, N = %u)",
                      weight, N);
        return buf;
    }
    if (offset >= N) {
        std::snprintf(buf, sizeof buf,
                      "precondition offset < N violated (offset = %u, N = %u)",
                      offset, N);
        return buf;
    }
    if (column >= N) {
        std::snprintf(buf, sizeof buf,
                      "precondition column < N violated (column = %u, N = %u)",
                      column, N);
        return buf;
    }
    return std::string();
}

//---------------------------------------------------------------------------
// Reporting
//---------------------------------------------------------------------------

int failures = 0;

void fail(const std::string& file, const bcmc::CellCase& c, const char* what,
          uint32_t got, uint32_t want) {
    if (failures < 20) {
        std::printf("FAIL %s:%d  %s\n"
                    "     N=%u weight=%u offset=%u column=%u   got %u, expected %u\n",
                    file.c_str(), c.line, what, c.N, c.weight, c.offset, c.column,
                    got, want);
    } else if (failures == 20) {
        std::printf("... further failures suppressed\n");
    }
    ++failures;
}

//---------------------------------------------------------------------------
// One pass over a suite, in a given order
//---------------------------------------------------------------------------

const char* order_name(int pass) {
    switch (pass) {
        case 0:  return "forwards";
        case 1:  return "backwards";
        default: return "shuffled";
    }
}

void run_pass(Dut& dut, const std::string& file,
              const std::vector<bcmc::CellCase>& cases,
              const std::vector<size_t>& order, int pass) {
    for (const size_t idx : order) {
        const bcmc::CellCase& c = cases[idx];

        // 1. Preconditions.
        const std::string bad = precondition_error(c.N, c.weight, c.offset, c.column);
        if (!bad.empty()) {
            std::printf("FAIL %s:%d  %s\n", file.c_str(), c.line, bad.c_str());
            ++failures;
            continue;
        }

        const uint32_t got = dut.evaluate(c.N, c.weight, c.offset, c.column);

        // 4. The output is a single bit.
        if (got > 1) {
            fail(file, c, "output is not 0 or 1", got, c.bit);
            continue;
        }

        // 2. RTL == Python. This also discharges check 5: a stateful cell
        //    would disagree with Python in at least one of the three orders.
        if (got != c.bit) {
            char what[96];
            std::snprintf(what, sizeof what,
                          "RTL disagrees with reference.py (pass: %s)",
                          order_name(pass));
            fail(file, c, what, got, c.bit);
            continue;
        }

        // 3. RTL == independent oracle.
        const uint32_t want = oracle(c.N, c.weight, c.offset, c.column);
        if (got != want) {
            fail(file, c, "RTL disagrees with the independent oracle", got, want);
        }
    }
}

//---------------------------------------------------------------------------
// The file-free exhaustive sweep
//---------------------------------------------------------------------------

uint64_t sweep(Dut& dut, uint32_t max_N) {
    uint64_t evaluated = 0;

    for (uint32_t N = 1; N <= max_N; ++N) {
        for (uint32_t weight = 0; weight <= N; ++weight) {
            for (uint32_t offset = 0; offset < N; ++offset) {
                for (uint32_t column = 0; column < N; ++column) {
                    const uint32_t got  = dut.evaluate(N, weight, offset, column);
                    const uint32_t want = oracle(N, weight, offset, column);
                    ++evaluated;
                    if (got != want) {
                        if (failures < 20) {
                            std::printf("FAIL sweep  N=%u weight=%u offset=%u "
                                        "column=%u   got %u, expected %u\n",
                                        N, weight, offset, column, got, want);
                        }
                        ++failures;
                    }
                }
            }
        }
    }
    return evaluated;
}

}  // namespace

//---------------------------------------------------------------------------

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);

    std::vector<std::string> files;
    size_t                   limit    = 0;   // 0 = no limit
    uint32_t                 sweep_to = 0;   // 0 = no sweep
    std::string              vcd;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--limit" && i + 1 < argc) {
            limit = static_cast<size_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (arg == "--sweep" && i + 1 < argc) {
            sweep_to = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (arg == "--vcd" && i + 1 < argc) {
            vcd = argv[++i];
        } else if (arg.rfind("--", 0) == 0 || arg.rfind("+", 0) == 0) {
            continue;   // Verilator's own arguments
        } else {
            files.push_back(arg);
        }
    }

    if (files.empty() && sweep_to == 0) {
        std::fprintf(stderr,
                     "usage: %s [--limit K] [--sweep K] [--vcd PATH] "
                     "<vectors.txt>...\n",
                     argv[0]);
        return 2;
    }

    Dut dut;
    if (!vcd.empty()) dut.open_trace(vcd);

    size_t   total_cases = 0;
    uint64_t total_evals = 0;

    for (const std::string& file : files) {
        std::vector<bcmc::CellCase> cases;
        try {
            cases = bcmc::load_cell_vectors(file);
        } catch (const std::exception& e) {
            std::printf("FAIL  %s\n", e.what());
            return 1;
        }

        if (limit != 0 && cases.size() > limit) cases.resize(limit);

        std::vector<size_t> order(cases.size());
        std::iota(order.begin(), order.end(), size_t{0});

        // Pass 0: forwards. Pass 1: backwards. Pass 2: shuffled.
        // A cell that remembered anything about its previous evaluation would
        // have to agree with reference.py in all three orders to escape.
        for (int pass = 0; pass < 3; ++pass) {
            if (pass == 1) {
                std::reverse(order.begin(), order.end());
            } else if (pass == 2) {
                std::mt19937 rng(0x42434D43u ^ static_cast<uint32_t>(cases.size()));
                std::shuffle(order.begin(), order.end(), rng);
            }
            run_pass(dut, file, cases, order, pass);
        }

        total_cases += cases.size();
        total_evals += static_cast<uint64_t>(cases.size()) * 3;
    }

    if (sweep_to != 0) {
        const uint64_t swept = sweep(dut, sweep_to);
        std::printf("  sweep: every legal tuple for N <= %u  (%llu evaluations)\n",
                    sweep_to, static_cast<unsigned long long>(swept));
        total_evals += swept;
    }

    std::printf("%s  %zu cases, %llu evaluations, %d failures\n",
                failures == 0 ? "PASS" : "FAIL", total_cases,
                static_cast<unsigned long long>(total_evals), failures);

    return failures == 0 ? 0 : 1;
}

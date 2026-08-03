//===========================================================================
// bcmc_row_test.cpp -- the row projection is nothing but replicated cells
//
// Two claims are established here, and they are different in kind.
//
//   A. rtl/bcmc_row.v agrees with validation/reference.py's bcmc_row().
//      This is the same claim as for every other module: RTL == Python.
//
//   B. rtl/bcmc_row.v is STRUCTURALLY nothing but copies of rtl/bcmc_cell.v.
//      This program instantiates a second, separate bcmc_cell alongside the
//      row and compares them bit for bit, for every column of every case. The
//      cell is the primitive; the row is proven to be replication, not
//      re-derivation. Nothing about the definition is restated here.
//
// Because a whole matrix arrives in each vector case, the two matrix-level
// statements are also checked, using the rows the RTL itself produced:
//
//   * Row conservation, popcount(row i) == weights[i]. Lemma 1: the
//     construction places exactly the weight it was asked for.
//   * The Balance Theorem, popcount(column j) == q + 1 for j < r else q, with
//     W = qN + r recomputed here from the weights alone. The theorem depends on
//     nothing but (N, W), and this program checks it against a matrix that was
//     assembled entirely out of RTL bits.
//
// Six checks in total:
//
//   1. Preconditions.       The row has none of its own; it inherits the
//                           cell's, and no RTL module in this project asserts
//                           them. The testbench owns them.
//   2. RTL == Python.       Every bit, against the R lines of the vector file.
//   3. RTL == cell.         Claim B above, one separate cell per column.
//   4. Lanes above N.       Columns that do not exist must read zero.
//   5. Row conservation.    Lemma 1, on RTL bits.
//   6. Balance Theorem.     On RTL bits, against an independent q, r.
//
// Plus, wrapping all of it, order invariance: every case is evaluated three
// times -- forwards, backwards, shuffled -- because a combinational module that
// remembered anything would have to agree in all three orders to escape.
//
// Usage:
//     bcmc_row_test [--limit K] [--vcd PATH] <matrix_vectors.txt>...
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

#include "Vbcmc_cell.h"
#include "Vbcmc_row.h"
#include "verilated.h"
#include "vectors.h"

#if VM_TRACE
#include "verilated_vcd_c.h"
#endif

// MAX_N of the elaborated bcmc_row. Set by sim/CMakeLists.txt, which passes the
// same number to Verilator as -GMAX_N, so the two can never drift apart.
#ifndef BCMC_ROW_MAX_N
#define BCMC_ROW_MAX_N 32
#endif

namespace {

constexpr uint32_t kMaxN = BCMC_ROW_MAX_N;

//---------------------------------------------------------------------------
// The device under test, and the lone cell it is compared against
//---------------------------------------------------------------------------

class Dut {
  public:
    Dut() : row_(new Vbcmc_row), cell_(new Vbcmc_cell) {}

    ~Dut() {
#if VM_TRACE
        if (trace_) {
            trace_->close();
            delete trace_;
        }
#endif
        row_->final();
        cell_->final();
        delete row_;
        delete cell_;
    }

    void open_trace(const std::string& path) {
#if VM_TRACE
        Verilated::traceEverOn(true);
        trace_ = new VerilatedVcdC;
        row_->trace(trace_, 99);
        trace_->open(path.c_str());
#else
        (void)path;
        std::fprintf(stderr, "warning: built without tracing; --vcd ignored\n");
#endif
    }

    // The whole of "running" a combinational module: apply, evaluate, read.
    uint32_t evaluate_row(uint32_t N, uint32_t weight, uint32_t offset) {
        row_->N      = N;
        row_->weight = weight;
        row_->offset = offset;
        row_->eval();
#if VM_TRACE
        if (trace_) trace_->dump(time_);
#endif
        time_ += 10;
        return static_cast<uint32_t>(row_->row_bits);
    }

    // The separate cell. This is the whole of claim B's right-hand side.
    uint32_t evaluate_cell(uint32_t N, uint32_t weight, uint32_t offset,
                           uint32_t column) {
        cell_->N      = N;
        cell_->weight = weight;
        cell_->offset = offset;
        cell_->column = column;
        cell_->eval();
        return cell_->bit_out;
    }

  private:
    Vbcmc_row*  row_  = nullptr;
    Vbcmc_cell* cell_ = nullptr;
#if VM_TRACE
    VerilatedVcdC* trace_ = nullptr;
#endif
    uint64_t time_ = 0;
};

//---------------------------------------------------------------------------
// Reporting
//---------------------------------------------------------------------------

int      failures    = 0;
uint64_t bits_checked = 0;

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
// Preconditions, owned by the testbench
//---------------------------------------------------------------------------

bool preconditions_ok(const std::string& file, const bcmc::MatrixCase& c) {
    if (c.N < 1) {
        fail("FAIL %s:%d  precondition N >= 1 violated (N = %u)\n",
             file.c_str(), c.line, c.N);
        return false;
    }
    if (c.N > kMaxN) {
        fail("FAIL %s:%d  N = %u exceeds the elaborated MAX_N = %u\n",
             file.c_str(), c.line, c.N, kMaxN);
        return false;
    }
    if (c.weights.size() != c.C || c.offsets.size() != c.C ||
        c.rows.size() != c.C || c.occupancy.size() != c.N) {
        fail("FAIL %s:%d  malformed case: C = %u but %zu weights, %zu offsets, "
             "%zu rows, %zu occupancies\n",
             file.c_str(), c.line, c.C, c.weights.size(), c.offsets.size(),
             c.rows.size(), c.occupancy.size());
        return false;
    }
    for (uint32_t i = 0; i < c.C; ++i) {
        if (c.weights[i] > c.N) {
            fail("FAIL %s:%d  precondition weight <= N violated in row %u "
                 "(weight = %u, N = %u)\n",
                 file.c_str(), c.line, i, c.weights[i], c.N);
            return false;
        }
        if (c.offsets[i] >= c.N) {
            fail("FAIL %s:%d  precondition offset < N violated in row %u "
                 "(offset = %u, N = %u)\n",
                 file.c_str(), c.line, i, c.offsets[i], c.N);
            return false;
        }
    }
    return true;
}

//---------------------------------------------------------------------------
// One case: assemble the whole matrix out of RTL rows, then interrogate it
//---------------------------------------------------------------------------

void run_case(Dut& dut, const std::string& file, const bcmc::MatrixCase& c,
              int pass) {
    // The matrix as the RTL built it. Everything below is computed from this,
    // never from the vector file's R lines.
    std::vector<uint32_t> rtl_rows(c.C, 0);

    for (uint32_t i = 0; i < c.C; ++i) {
        const uint32_t bits =
            dut.evaluate_row(c.N, c.weights[i], c.offsets[i]);
        rtl_rows[i] = bits;

        for (uint32_t j = 0; j < c.N; ++j) {
            const uint32_t got = (bits >> j) & 1u;
            ++bits_checked;

            // 2. RTL == Python.
            const uint32_t want = c.rows[i][j];
            if (got != want) {
                fail("FAIL %s:%d  row %u column %u disagrees with reference.py "
                     "(pass: %s)\n"
                     "     N=%u weight=%u offset=%u   got %u, expected %u\n",
                     file.c_str(), c.line, i, j, order_name(pass), c.N,
                     c.weights[i], c.offsets[i], got, want);
            }

            // 3. RTL == a separately instantiated cell. The row claims to be
            //    replication; this is that claim, tested.
            const uint32_t from_cell =
                dut.evaluate_cell(c.N, c.weights[i], c.offsets[i], j);
            if (got != from_cell) {
                fail("FAIL %s:%d  row %u column %u is not what bcmc_cell says\n"
                     "     N=%u weight=%u offset=%u   row %u, cell %u\n",
                     file.c_str(), c.line, i, j, c.N, c.weights[i],
                     c.offsets[i], got, from_cell);
            }
        }

        // 4. Columns that do not exist must read zero.
        for (uint32_t j = c.N; j < kMaxN; ++j) {
            if ((bits >> j) & 1u) {
                fail("FAIL %s:%d  row %u: lane %u is above N = %u but reads 1\n",
                     file.c_str(), c.line, i, j, c.N);
            }
        }

        // 5. Row conservation (Lemma 1), on RTL bits.
        uint32_t popcount = 0;
        for (uint32_t j = 0; j < c.N; ++j) popcount += (bits >> j) & 1u;
        if (popcount != c.weights[i]) {
            fail("FAIL %s:%d  row conservation: row %u has popcount %u, "
                 "weight %u\n",
                 file.c_str(), c.line, i, popcount, c.weights[i]);
        }
    }

    // 6. The Balance Theorem, on the matrix the RTL assembled.
    //
    // W and (q, r) are recomputed here from the weights alone. The theorem's
    // whole content is that the occupancies depend on nothing else.
    const uint32_t W = c.total_weight();
    const uint32_t q = W / c.N;
    const uint32_t r = W % c.N;

    for (uint32_t j = 0; j < c.N; ++j) {
        uint32_t load = 0;
        for (uint32_t i = 0; i < c.C; ++i) load += (rtl_rows[i] >> j) & 1u;

        const uint32_t predicted = (j < r) ? q + 1 : q;
        if (load != predicted) {
            fail("FAIL %s:%d  Balance Theorem: column %u carries %u, "
                 "W = %u = %u*%u + %u predicts %u\n",
                 file.c_str(), c.line, j, load, W, q, c.N, r, predicted);
        }
        if (load != c.occupancy[j]) {
            fail("FAIL %s:%d  column %u occupancy %u disagrees with "
                 "reference.py's %u\n",
                 file.c_str(), c.line, j, load, c.occupancy[j]);
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
                     "usage: %s [--limit K] [--vcd PATH] "
                     "<matrix_vectors.txt>...\n",
                     argv[0]);
        return 2;
    }

    Dut dut;
    if (!vcd.empty()) dut.open_trace(vcd);

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

        // Preconditions once per case, before any evaluation: a case that is
        // outside the specification is a broken vector file, not a broken RTL.
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
            for (const size_t idx : order) run_case(dut, file, cases[idx], pass);
        }

        total_cases += cases.size();
    }

    std::printf("%s  %zu matrices, %llu bits compared against bcmc_cell, "
                "%d failures\n",
                failures == 0 ? "PASS" : "FAIL", total_cases,
                static_cast<unsigned long long>(bits_checked), failures);

    return failures == 0 ? 0 : 1;
}

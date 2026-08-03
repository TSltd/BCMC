//===========================================================================
// bcmc_column_test.cpp -- the column projection is nothing but replicated cells
//
// The mirror image of bcmc_row_test.cpp, and deliberately its equal: neither
// projection is privileged. The same two claims, in the same two kinds:
//
//   A. rtl/bcmc_column.v agrees with validation/reference.py's bcmc_column().
//   B. rtl/bcmc_column.v is STRUCTURALLY nothing but copies of
//      rtl/bcmc_cell.v. A second, separate bcmc_cell is instantiated alongside
//      the column and compared bit for bit, for every row of every case.
//
// The column is the projection an allocator usually wants -- one column is one
// scheduling slot -- so the Balance Theorem is a statement about the popcount of
// this module's output directly, with no assembly step in between.
//
// Six checks:
//
//   1. Preconditions.       Inherited from the cell; the testbench owns them.
//   2. RTL == Python.       Every bit, against column j of the R lines.
//   3. RTL == cell.         Claim B above, one separate cell per row.
//   4. Lanes above C.       Rows that do not exist must read zero.
//   5. Balance Theorem.     popcount(column_bits) == q + 1 for j < r else q,
//                           with W = qN + r recomputed here from the weights.
//   6. Row conservation.    Accumulated across the columns of a case: a row's
//                           bits, gathered one column at a time, must still sum
//                           to its weight. Lemma 1 seen sideways.
//
// Plus order invariance, at both levels: the cases of a suite and the columns
// of a case are both visited forwards, backwards and shuffled.
//
// Usage:
//     bcmc_column_test [--limit K] [--vcd PATH] <matrix_vectors.txt>...
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
#include "Vbcmc_column.h"
#include "verilated.h"
#include "vectors.h"

#if VM_TRACE
#include "verilated_vcd_c.h"
#endif

// MAX_C and VAL_W of the elaborated bcmc_column. Set by sim/CMakeLists.txt,
// which passes the same numbers to Verilator as -GMAX_C and -GVAL_W, so the two
// can never drift apart.
#ifndef BCMC_COLUMN_MAX_C
#define BCMC_COLUMN_MAX_C 32
#endif
#ifndef BCMC_COLUMN_VAL_W
#define BCMC_COLUMN_VAL_W 16
#endif

namespace {

constexpr uint32_t kMaxC = BCMC_COLUMN_MAX_C;
constexpr uint32_t kValW = BCMC_COLUMN_VAL_W;

//---------------------------------------------------------------------------
// The device under test, and the lone cell it is compared against
//---------------------------------------------------------------------------

class Dut {
  public:
    Dut() : col_(new Vbcmc_column), cell_(new Vbcmc_cell) {}

    ~Dut() {
#if VM_TRACE
        if (trace_) {
            trace_->close();
            delete trace_;
        }
#endif
        col_->final();
        cell_->final();
        delete col_;
        delete cell_;
    }

    void open_trace(const std::string& path) {
#if VM_TRACE
        Verilated::traceEverOn(true);
        trace_ = new VerilatedVcdC;
        col_->trace(trace_, 99);
        trace_->open(path.c_str());
#else
        (void)path;
        std::fprintf(stderr, "warning: built without tracing; --vcd ignored\n");
#endif
    }

    // Verilog-2005 has no array ports, so the weights and offsets arrive as
    // flat vectors with row i in bits [VAL_W*i +: VAL_W]. Verilator presents
    // anything wider than 64 bits as an array of 32-bit words, so packing is
    // done here rather than in the RTL.
    void set_rows(const std::vector<uint32_t>& weights,
                  const std::vector<uint32_t>& offsets) {
        pack(col_->weights_flat, weights);
        pack(col_->offsets_flat, offsets);
    }

    uint32_t evaluate_column(uint32_t N, uint32_t C, uint32_t column) {
        col_->N      = N;
        col_->C      = C;
        col_->column = column;
        col_->eval();
#if VM_TRACE
        if (trace_) trace_->dump(time_);
#endif
        time_ += 10;
        return static_cast<uint32_t>(col_->column_bits);
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
    // Works for whatever type Verilator chose for the flat port: an array of
    // words for wide vectors, or a plain integer for narrow ones.
    template <typename Port>
    static void pack(Port& port, const std::vector<uint32_t>& values) {
        constexpr uint32_t kWords = (kMaxC * kValW + 31) / 32;
        uint32_t           words[kWords];
        for (uint32_t w = 0; w < kWords; ++w) words[w] = 0;

        for (uint32_t i = 0; i < kMaxC; ++i) {
            const uint64_t value = (i < values.size()) ? values[i] : 0u;
            const uint32_t lsb   = kValW * i;
            for (uint32_t b = 0; b < kValW; ++b) {
                if ((value >> b) & 1u) {
                    words[(lsb + b) / 32] |= 1u << ((lsb + b) % 32);
                }
            }
        }
        store(port, words, kWords);
    }

    // Wide port: an indexable array of 32-bit words.
    template <typename Port>
    static auto store(Port& port, const uint32_t* words, uint32_t n)
        -> decltype(port[0], void()) {
        for (uint32_t w = 0; w < n; ++w) port[w] = words[w];
    }

    // Narrow port: a single integer, 64 bits or fewer.
    static void store(uint32_t& port, const uint32_t* words, uint32_t) {
        port = words[0];
    }
    static void store(uint64_t& port, const uint32_t* words, uint32_t n) {
        port = words[0];
        if (n > 1) port |= static_cast<uint64_t>(words[1]) << 32;
    }

    Vbcmc_column* col_  = nullptr;
    Vbcmc_cell*   cell_ = nullptr;
#if VM_TRACE
    VerilatedVcdC* trace_ = nullptr;
#endif
    uint64_t time_ = 0;
};

//---------------------------------------------------------------------------
// Reporting
//---------------------------------------------------------------------------

int      failures     = 0;
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
    if (c.C > kMaxC) {
        fail("FAIL %s:%d  C = %u exceeds the elaborated MAX_C = %u\n",
             file.c_str(), c.line, c.C, kMaxC);
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
// One case: visit every column, in a given order
//---------------------------------------------------------------------------

void run_case(Dut& dut, const std::string& file, const bcmc::MatrixCase& c,
              int pass) {
    dut.set_rows(c.weights, c.offsets);

    // The order in which the columns of this case are visited. A stateless
    // module cannot care, which is exactly what is being tested.
    std::vector<uint32_t> columns(c.N);
    std::iota(columns.begin(), columns.end(), 0u);
    if (pass == 1) {
        std::reverse(columns.begin(), columns.end());
    } else if (pass == 2) {
        std::mt19937 rng(0x434F4C55u ^ c.N ^ (c.C << 8));
        std::shuffle(columns.begin(), columns.end(), rng);
    }

    // W and (q, r) come from the weights alone: the Balance Theorem's content
    // is that the occupancies depend on nothing else.
    const uint32_t W = c.total_weight();
    const uint32_t q = W / c.N;
    const uint32_t r = W % c.N;

    // Row conservation, gathered one column at a time.
    std::vector<uint32_t> row_popcount(c.C, 0);

    for (const uint32_t j : columns) {
        const uint32_t bits = dut.evaluate_column(c.N, c.C, j);

        uint32_t load = 0;

        for (uint32_t i = 0; i < c.C; ++i) {
            const uint32_t got = (bits >> i) & 1u;
            ++bits_checked;
            load += got;
            row_popcount[i] += got;

            // 2. RTL == Python.
            const uint32_t want = c.rows[i][j];
            if (got != want) {
                fail("FAIL %s:%d  column %u row %u disagrees with reference.py "
                     "(pass: %s)\n"
                     "     N=%u weight=%u offset=%u   got %u, expected %u\n",
                     file.c_str(), c.line, j, i, order_name(pass), c.N,
                     c.weights[i], c.offsets[i], got, want);
            }

            // 3. RTL == a separately instantiated cell.
            const uint32_t from_cell =
                dut.evaluate_cell(c.N, c.weights[i], c.offsets[i], j);
            if (got != from_cell) {
                fail("FAIL %s:%d  column %u row %u is not what bcmc_cell says\n"
                     "     N=%u weight=%u offset=%u   column %u, cell %u\n",
                     file.c_str(), c.line, j, i, c.N, c.weights[i],
                     c.offsets[i], got, from_cell);
            }
        }

        // 4. Rows that do not exist must read zero.
        for (uint32_t i = c.C; i < kMaxC; ++i) {
            if ((bits >> i) & 1u) {
                fail("FAIL %s:%d  column %u: lane %u is above C = %u but "
                     "reads 1\n",
                     file.c_str(), c.line, j, i, c.C);
            }
        }

        // 5. The Balance Theorem, directly on this module's popcount.
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

    // 6. Row conservation (Lemma 1), seen sideways: every column of the case
    //    has now been visited, so the per-row totals are complete.
    for (uint32_t i = 0; i < c.C; ++i) {
        if (row_popcount[i] != c.weights[i]) {
            fail("FAIL %s:%d  row conservation: row %u summed to %u across the "
                 "columns, weight %u\n",
                 file.c_str(), c.line, i, row_popcount[i], c.weights[i]);
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

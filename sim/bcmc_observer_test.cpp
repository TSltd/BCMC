//===========================================================================
// bcmc_observer_test.cpp -- sw/bcmc_observer.c against Python, and against RTL
//
//     bcmc_observer_test [--limit K] [--vcd PATH]
//                        --prng observer_prng.txt --order observer_order.txt
//                        <matrix_vectors.txt>...
//
// TWO OBLIGATIONS, FROM docs/Observers.md
//
//     | The C observers match the Python permutation | here |
//     | The C observers, driven against real RTL     | here |
//
// The first is a table lookup and the second is a simulation, and they are in
// one file because they are one claim: that sw/bcmc_observer.c is the same
// observer validation/observers.py already proved conforming, and that it
// remains that observer when the matrix underneath it is produced by
// rtl/bcmc_wb.v rather than by Python.
//
// WHY THE PERMUTATION IS A TABLE
//
// "A seeded shuffle" is not a specification. Two correct implementations of
// Fisher-Yates disagree unless the generator, the direction of the loop and
// the inclusivity of the draw are all pinned, so docs/Observers.md pins all
// three and validation/gen_observer_vectors.py writes down what they produce.
// This harness then holds the C to that table index for index. Nothing here
// recomputes a permutation: a second implementation of the shuffle in C++
// would only prove that two things this author wrote agree.
//
// WHAT AN OBSERVER PASS IS CHECKED AGAINST
//
// The same matrices as everything else -- validation/reference.py by way of
// matrix_*.txt -- read through the real driver over the real peripheral. The
// properties checked are the ones docs/Observers.md names, and not one of them
// is a new theorem:
//
//     O1  every order used is a bijection of 0 .. N-1
//     O2  visiting j yields R(j): the rows reference.py recorded, ascending
//     P1  a pass emits W events, the support of M, once each
//     P2  row conservation: row i is emitted weight[i] times, any order
//     P3  the multiset of occupancies is the same, any order
//     P4  two observers, same events, different sequence
//
// P3 is the Balance Theorem surviving being read out of order, which is the
// entire point of v0.5: balance is a property of the matrix, smoothness is a
// property of the observer.
//
// ACCESS COUNTING IS HOW "ADDS NO TRAFFIC" IS STATED
//
// sw/bcmc_observer.h claims a visit costs whatever bcmc_read_column() costs
// and not one access more, and that building an order costs nothing at all.
// Both are claims about traffic, so the traffic is counted: an order builder
// that peeked at N, or a cursor that re-read STATUS between visits, would show
// up here as an access that should not exist.
//
//===========================================================================

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "Vbcmc_wb.h"
#include "vectors.h"
#include "verilated.h"
#include "wb_bfm.h"

#if VM_TRACE
#include "verilated_vcd_c.h"
#endif

extern "C" {
#include "bcmc.h"
#include "bcmc_observer.h"
}

namespace {

uint64_t g_checks = 0;

void fail(const char* fmt, ...) {
    std::va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stdout, fmt, ap);
    va_end(ap);
    std::fflush(stdout);
    std::exit(1);
}

void check(bool ok, const char* fmt, ...) {
    g_checks++;
    if (ok) return;
    std::va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stdout, fmt, ap);
    va_end(ap);
    std::fflush(stdout);
    std::exit(1);
}

//---------------------------------------------------------------------------
// The observer vector formats
//
// These live here rather than in sim/common/vectors.h on purpose. That reader
// exists so the Verilator harnesses and the Verilog testbenches can consume the
// same files; no Verilog reads a permutation, because a permutation is not an
// instance of BCMC mathematics. Nothing generic is being avoided -- there is
// simply no second consumer to share with.
//
//     observer_prng.txt   G <seed> <z0> .. <z15>            all hexadecimal
//     observer_order.txt  S <N> <pi(0)> .. <pi(N-1)>         pi decimal
//                         P <N> <seed> <pi(0)> .. <pi(N-1)>  seed hexadecimal
//
// Blank lines are skipped and '#' comments to end of line, as everywhere else
// in sim/vectors/.
//---------------------------------------------------------------------------

struct PrngCase {
    uint32_t              seed = 0;
    std::vector<uint32_t> draws;
    int                   line = 0;
};

struct OrderCase {
    bool                  permuted = false;
    uint32_t              n        = 0;
    uint32_t              seed     = 0;  // permuted only
    std::vector<uint32_t> pi;
    int                   line = 0;
};

std::vector<std::string> tokens_of(const std::string& raw) {
    std::string text = raw;
    const std::size_t hash = text.find('#');
    if (hash != std::string::npos) text.erase(hash);
    std::istringstream        in(text);
    std::vector<std::string>  out;
    std::string               tok;
    while (in >> tok) out.push_back(tok);
    return out;
}

uint32_t parse_u32(const std::string& tok, int base, const char* path, int line) {
    char*                    end = nullptr;
    const unsigned long long v   = std::strtoull(tok.c_str(), &end, base);
    if (end == tok.c_str() || *end != '\0' || v > 0xFFFFFFFFull) {
        fail("FAIL: %s:%d: '%s' is not a 32-bit value\n", path, line, tok.c_str());
    }
    return static_cast<uint32_t>(v);
}

std::vector<PrngCase> load_prng_vectors(const std::string& path) {
    std::ifstream in(path.c_str());
    if (!in) fail("FAIL: cannot open %s\n", path.c_str());

    std::vector<PrngCase> out;
    std::string           raw;
    int                   line = 0;
    while (std::getline(in, raw)) {
        line++;
        const std::vector<std::string> t = tokens_of(raw);
        if (t.empty()) continue;
        if (t[0] != "G") {
            fail("FAIL: %s:%d: expected a G line, got '%s'\n", path.c_str(), line,
                 t[0].c_str());
        }
        if (t.size() < 3) {
            fail("FAIL: %s:%d: a G line needs a seed and at least one draw\n",
                 path.c_str(), line);
        }
        PrngCase c;
        c.line = line;
        c.seed = parse_u32(t[1], 16, path.c_str(), line);
        for (std::size_t k = 2; k < t.size(); k++) {
            c.draws.push_back(parse_u32(t[k], 16, path.c_str(), line));
        }
        out.push_back(c);
    }
    if (out.empty()) fail("FAIL: %s: no PRNG cases\n", path.c_str());
    return out;
}

std::vector<OrderCase> load_order_vectors(const std::string& path) {
    std::ifstream in(path.c_str());
    if (!in) fail("FAIL: cannot open %s\n", path.c_str());

    std::vector<OrderCase> out;
    std::string            raw;
    int                    line = 0;
    while (std::getline(in, raw)) {
        line++;
        const std::vector<std::string> t = tokens_of(raw);
        if (t.empty()) continue;

        OrderCase c;
        c.line = line;
        std::size_t at = 0;
        if (t[0] == "S") {
            c.permuted = false;
            at         = 2;
        } else if (t[0] == "P") {
            c.permuted = true;
            at         = 3;
        } else {
            fail("FAIL: %s:%d: expected S or P, got '%s'\n", path.c_str(), line,
                 t[0].c_str());
        }
        if (t.size() < at) {
            fail("FAIL: %s:%d: truncated %s line\n", path.c_str(), line, t[0].c_str());
        }
        c.n = parse_u32(t[1], 10, path.c_str(), line);
        if (c.permuted) c.seed = parse_u32(t[2], 16, path.c_str(), line);

        for (std::size_t k = at; k < t.size(); k++) {
            c.pi.push_back(parse_u32(t[k], 10, path.c_str(), line));
        }
        if (c.pi.size() != c.n) {
            fail("FAIL: %s:%d: N = %u but %u indices follow\n", path.c_str(), line,
                 c.n, static_cast<unsigned>(c.pi.size()));
        }
        out.push_back(c);
    }
    if (out.empty()) fail("FAIL: %s: no traversal cases\n", path.c_str());
    return out;
}

//---------------------------------------------------------------------------
// The platform, and the meter
//
// Both lifted unchanged in spirit from sim/bcmc_driver_test.cpp: two accessors
// over bcmc::WbMaster, and a counter that turns every documented cost into an
// assertion. The driver below is the real sw/bcmc.c, the observer above it is
// the real sw/bcmc_observer.c, and the peripheral under both is the verilated
// rtl/bcmc_wb.v. Nothing in this file models any of the three.
//---------------------------------------------------------------------------

struct Platform {
    bcmc::WbMaster* bus        = nullptr;
    uint64_t        timeouts   = 0;
    uint64_t        duplicates = 0;
    uint64_t        errs       = 0;
};

void note(Platform* p, const bcmc::WbResponse& r) {
    if (r.timeout) p->timeouts++;
    if (r.duplicate) p->duplicates++;
    if (r.err) p->errs++;
}

int plat_read(void* ctx, uint32_t addr, uint32_t* data) {
    Platform*        p = static_cast<Platform*>(ctx);
    bcmc::WbResponse r = p->bus->read(addr);
    note(p, r);
    if (!r.ack()) return -1;
    *data = r.data;
    return 0;
}

int plat_write(void* ctx, uint32_t addr, uint32_t data) {
    Platform*        p = static_cast<Platform*>(ctx);
    bcmc::WbResponse r = p->bus->write(addr, data);
    note(p, r);
    return r.ack() ? 0 : -1;
}

class Meter {
public:
    Meter(bcmc::WbMaster* bus, Platform* plat) : bus_(bus), plat_(plat) {}

    void     begin() { mark_ = bus_->accesses(); }
    uint64_t cost() const { return bus_->accesses() - mark_; }

    void expect(bcmc_status_t got, bcmc_status_t want, long long cost,
                const char* where) {
        const uint64_t spent = this->cost();
        check(got == want, "FAIL: %s: got %s, expected %s\n", where,
              bcmc_strstatus(got), bcmc_strstatus(want));
        if (cost >= 0) {
            check(spent == static_cast<uint64_t>(cost),
                  "FAIL: %s: cost %llu bus accesses, expected %lld\n", where,
                  static_cast<unsigned long long>(spent), cost);
        }
        check(plat_->timeouts == 0, "FAIL: %s: the slave failed to answer\n", where);
        check(plat_->duplicates == 0,
              "FAIL: %s: the slave answered twice; the response must be one "
              "cycle wide\n",
              where);
    }

private:
    bcmc::WbMaster* bus_;
    Platform*       plat_;
    uint64_t        mark_ = 0;
};

#define CALL(meter, want, cost, expr)              \
    do {                                           \
        (meter).begin();                           \
        bcmc_status_t s_ = (expr);                 \
        (meter).expect(s_, (want), (cost), #expr); \
    } while (0)

//---------------------------------------------------------------------------
// Suite 1: the generator, against Python
//
// Index for index, from the seed. This is the least interesting suite in the
// file and the one everything else depends on: if the generator drifts by a
// single constant, every permuted observer in every deployment disagrees with
// every other, and the failure shows up as a scheduling anomaly nobody would
// think to blame on a multiply.
//---------------------------------------------------------------------------

void suite_prng(const std::vector<PrngCase>& cases, const char* path) {
    for (const PrngCase& c : cases) {
        bcmc_rng_t rng;
        bcmc_rng_seed(&rng, c.seed);
        for (std::size_t k = 0; k < c.draws.size(); k++) {
            const uint32_t got = bcmc_rng_next(&rng);
            check(got == c.draws[k],
                  "FAIL: %s:%d: seed %08X draw %u = %08X, observers.py says "
                  "%08X\n",
                  path, c.line, c.seed, static_cast<unsigned>(k), got, c.draws[k]);
        }
    }

    // Seeding is total and idempotent: docs/Observers.md forbids a forbidden
    // seed, so zero is a seed like any other and re-seeding replays exactly.
    bcmc_rng_t a;
    bcmc_rng_t b;
    bcmc_rng_seed(&a, 0u);
    bcmc_rng_seed(&b, 0u);
    for (int k = 0; k < 64; k++) {
        check(bcmc_rng_next(&a) == bcmc_rng_next(&b),
              "FAIL: prng: the same seed gave two different streams\n");
    }

    // uniform(0) is the one draw whose answer is fixed by arithmetic rather
    // than by chance, and the one a mask built with an off-by-one would get
    // wrong first.
    bcmc_rng_t u;
    bcmc_rng_seed(&u, 12345u);
    for (int k = 0; k < 32; k++) {
        check(bcmc_rng_uniform(&u, 0u) == 0u,
              "FAIL: prng: uniform(0) must be 0; there is nothing else to "
              "return\n");
    }

    // The draw is inclusive of m -- j == i is a legal Fisher-Yates outcome --
    // so the range is 0 .. m and never m + 1.
    for (uint32_t m = 1u; m <= 40u; m++) {
        bcmc_rng_t r;
        bcmc_rng_seed(&r, m);
        for (int k = 0; k < 200; k++) {
            const uint32_t x = bcmc_rng_uniform(&r, m);
            check(x <= m, "FAIL: prng: uniform(%u) returned %u\n", m, x);
        }
    }
}

//---------------------------------------------------------------------------
// Suite 2: the two order builders, against Python
//
// Also index for index. Note what is NOT here: no C++ shuffle to compare
// against, and no statistical test. observers.py already showed the shuffle is
// uniform, and showed a deliberately wrong shuffle failing that test; repeating
// it here would test the same mathematics with a worse random source. What is
// unproven at this point is only whether the C reproduces it, and a table
// settles that outright.
//---------------------------------------------------------------------------

void suite_orders(const std::vector<OrderCase>& cases, const char* path) {
    std::vector<uint32_t> order;
    std::vector<uint32_t> scratch;

    for (const OrderCase& c : cases) {
        order.assign(c.n, 0xFFFFFFFFu);
        const bcmc_status_t s =
            c.permuted ? bcmc_order_permuted(order.data(), c.n, c.seed)
                       : bcmc_order_sequential(order.data(), c.n);
        check(s == BCMC_OK, "FAIL: %s:%d: order builder returned %s\n", path, c.line,
              bcmc_strstatus(s));

        for (uint32_t t = 0; t < c.n; t++) {
            check(order[t] == c.pi[t],
                  "FAIL: %s:%d: pi(%u) = %u, observers.py says %u\n", path, c.line,
                  t, order[t], c.pi[t]);
        }

        // O1, on every recorded traversal, decided by the C rather than
        // inherited from the file.
        scratch.assign((c.n + 31u) / 32u, 0u);
        check(bcmc_order_is_bijection(order.data(), c.n,
                                      scratch.data(),
                                      static_cast<uint32_t>(scratch.size())),
              "FAIL: %s:%d: the order built for N = %u is not a bijection\n", path,
              c.line, c.n);
    }

    // Refusals. Geometry the builder can settle by itself, settled by itself.
    uint32_t one = 0;
    check(bcmc_order_sequential(NULL, 4) == BCMC_EINVAL,
          "FAIL: orders: a null buffer was accepted\n");
    check(bcmc_order_permuted(NULL, 4, 1) == BCMC_EINVAL,
          "FAIL: orders: a null buffer was accepted by the shuffle\n");
    check(bcmc_order_sequential(&one, 0) == BCMC_ERANGE,
          "FAIL: orders: a pass of length zero was accepted\n");
    check(bcmc_order_permuted(&one, 0, 1) == BCMC_ERANGE,
          "FAIL: orders: a shuffle of length zero was accepted\n");
}

//---------------------------------------------------------------------------
// Suite 3: the bijection test can fail
//
// A predicate that never returns false proves nothing, so it is shown three
// distinct non-bijections and one buffer too small to decide the question. The
// last case matters most: bcmc_order_is_bijection() must refuse rather than
// read past the scratch it was given, because sw/ has no allocator and the
// scratch is the caller's.
//---------------------------------------------------------------------------

void suite_bijection(void) {
    uint32_t scratch[8] = {0};

    const uint32_t good[8]      = {3, 1, 0, 7, 5, 2, 6, 4};
    const uint32_t repeated[8]  = {3, 1, 0, 7, 5, 2, 6, 3};  // 4 missing, 3 twice
    const uint32_t out_of_range[8] = {3, 1, 0, 8, 5, 2, 6, 4};  // 8 is not a column
    const uint32_t identity_off[4] = {1, 2, 3, 4};              // off by one

    check(bcmc_order_is_bijection(good, 8, scratch, 8),
          "FAIL: bijection: a permutation of 0..7 was rejected\n");
    check(!bcmc_order_is_bijection(repeated, 8, scratch, 8),
          "FAIL: bijection: a repeated column was accepted; that pass visits "
          "one column twice and one never\n");
    check(!bcmc_order_is_bijection(out_of_range, 8, scratch, 8),
          "FAIL: bijection: an index outside 0..N-1 was accepted\n");
    check(!bcmc_order_is_bijection(identity_off, 4, scratch, 8),
          "FAIL: bijection: an off-by-one traversal was accepted\n");
    check(!bcmc_order_is_bijection(good, 8, scratch, 0),
          "FAIL: bijection: decided the question with no scratch at all\n");
    check(!bcmc_order_is_bijection(NULL, 8, scratch, 8),
          "FAIL: bijection: a null order was accepted\n");

    // Scratch arrives dirty, because a caller reuses one buffer for every
    // traversal it checks. Clearing it is the callee's job: an implementation
    // that skipped it would accept the first order and reject every one after.
    for (uint32_t k = 0; k < 8u; k++) {
        scratch[k] = 0xFFFFFFFFu;
    }
    check(bcmc_order_is_bijection(good, 8, scratch, 8),
          "FAIL: bijection: a bitmap left dirty by an earlier call was not "
          "cleared, so a good order was rejected\n");
}

//---------------------------------------------------------------------------
// Suite 4: decoding, with no bus and no matrix
//
// bcmc_column_load() and bcmc_column_rows() are pure functions of a bitmap, so
// they are checked on bitmaps chosen to be awkward rather than on matrices:
// empty, full, word boundaries, and a column too heavy for the buffer offered.
// Their agreement with reference.py's matrices is established in suite 5,
// where the bitmaps come from the hardware.
//---------------------------------------------------------------------------

void suite_decode(void) {
    uint32_t words[3] = {0u, 0u, 0u};
    uint32_t rows[96];
    uint32_t nrows = 0xDEADBEEFu;

    check(bcmc_column_load(words, 3) == 0u,
          "FAIL: decode: an empty column has load 0\n");
    check(bcmc_column_rows(words, 3, rows, 96, &nrows) == BCMC_OK &&
              nrows == 0u,
          "FAIL: decode: an empty column decodes to no rows\n");

    // Row 0, row 31, row 32 and row 95: the bits either side of every word
    // boundary, which is where a shift-by-32 or a k*32 would go wrong.
    words[0] = 0x80000001u;
    words[1] = 0x00000001u;
    words[2] = 0x80000000u;
    check(bcmc_column_load(words, 3) == 4u, "FAIL: decode: load of a 4-bit column\n");
    check(bcmc_column_rows(words, 3, rows, 96, &nrows) == BCMC_OK,
          "FAIL: decode: a 4-bit column was refused\n");
    check(nrows == 4u, "FAIL: decode: %u rows, expected 4\n", nrows);
    check(rows[0] == 0u && rows[1] == 31u && rows[2] == 32u && rows[3] == 95u,
          "FAIL: decode: rows %u %u %u %u, expected 0 31 32 95 ascending\n", rows[0],
          rows[1], rows[2], rows[3]);

    // A column that does not fit leaves the buffer alone and does not write
    // *nrows: a truncated R(j) is indistinguishable from a lighter column.
    nrows = 0xDEADBEEFu;
    check(bcmc_column_rows(words, 3, rows, 3, &nrows) == BCMC_ERANGE,
          "FAIL: decode: a column of 4 rows fitted into 3\n");
    check(nrows == 0xDEADBEEFu,
          "FAIL: decode: *nrows was written on a refused decode\n");

    // Full: every row of a 96-row column active.
    words[0] = 0xFFFFFFFFu;
    words[1] = 0xFFFFFFFFu;
    words[2] = 0xFFFFFFFFu;
    check(bcmc_column_load(words, 3) == 96u, "FAIL: decode: load of a full column\n");
    check(bcmc_column_rows(words, 3, rows, 96, &nrows) == BCMC_OK && nrows == 96u,
          "FAIL: decode: a full column decodes to 96 rows\n");
    for (uint32_t r = 0; r < 96u; r++) {
        check(rows[r] == r, "FAIL: decode: full column row %u decoded as %u\n", r,
              rows[r]);
    }

    check(bcmc_column_rows(NULL, 3, rows, 96, &nrows) == BCMC_EINVAL,
          "FAIL: decode: a null bitmap was accepted\n");
    check(bcmc_column_rows(words, 3, rows, 96, NULL) == BCMC_EINVAL,
          "FAIL: decode: a null count was accepted\n");
    check(bcmc_column_load(NULL, 3) == 0u,
          "FAIL: decode: a null bitmap has no load to report\n");
}

//---------------------------------------------------------------------------
// Suite 5: a pass, over the RTL
//
// One visit, recorded. This is the only structure in the file that holds
// anything about a matrix, and it belongs to the harness rather than to the
// observer: sw/bcmc_observer.c keeps no column, by rule.
//---------------------------------------------------------------------------

struct Visit {
    uint32_t              col  = 0;
    uint32_t              load = 0;
    std::vector<uint32_t> rows;  // ascending
};

// Walks a cursor to the end, recording every visit and pinning the cost of
// each. Returns the visits in the order they happened, which is the only place
// the identity of the observer survives.
std::vector<Visit> run_pass(bcmc_observer_t* ob, Meter* m, uint32_t words,
                            uint32_t max_c, const char* who) {
    std::vector<uint32_t> bits(words, 0u);
    std::vector<uint32_t> rows(max_c, 0u);
    std::vector<Visit>    out;

    const long long visit_cost = 1 + static_cast<long long>(words);

    while (!bcmc_observer_at_end(ob)) {
        uint32_t col = 0xFFFFFFFFu;
        // The whole claim of sw/bcmc_observer.h in one line: a visit is a
        // bcmc_read_column() and the cursor adds nothing.
        CALL(*m, BCMC_OK, visit_cost,
             bcmc_observer_next(ob, &col, bits.data(), words));

        Visit v;
        v.col  = col;
        v.load = bcmc_column_load(bits.data(), words);

        uint32_t nrows = 0;
        const bcmc_status_t s =
            bcmc_column_rows(bits.data(), words, rows.data(), max_c, &nrows);
        check(s == BCMC_OK, "FAIL: %s: decoding column %u returned %s\n", who, col,
              bcmc_strstatus(s));
        check(nrows == v.load,
              "FAIL: %s: column %u decoded %u rows but has load %u\n", who, col,
              nrows, v.load);
        v.rows.assign(rows.begin(), rows.begin() + nrows);

        for (uint32_t k = 1; k < nrows; k++) {
            check(v.rows[k] > v.rows[k - 1],
                  "FAIL: %s: column %u gave rows out of order: %u then %u\n", who,
                  col, v.rows[k - 1], v.rows[k]);
        }
        out.push_back(v);
    }

    // Past the end a cursor refuses rather than wrapping, and refuses without
    // touching the bus: there is no column to read, so there is no reason to
    // ask for one.
    uint32_t col = 0xFFFFFFFFu;
    CALL(*m, BCMC_ENOTREADY, 0, bcmc_observer_next(ob, &col, bits.data(), words));

    return out;
}

// The events of a pass, as (row, column) pairs, sorted. Sorting is what makes
// the comparison a statement about the support of M rather than about order --
// which is exactly the distinction P4 draws.
std::vector<std::pair<uint32_t, uint32_t> > events_of(const std::vector<Visit>& pass) {
    std::vector<std::pair<uint32_t, uint32_t> > out;
    for (const Visit& v : pass) {
        for (uint32_t r : v.rows) out.push_back(std::make_pair(r, v.col));
    }
    std::sort(out.begin(), out.end());
    return out;
}

void suite_matrix(bcmc_dev_t* dev, Meter* m, const bcmc::MatrixCase& c,
                  const char* path) {
    const uint32_t max_c = dev->max_c;
    const uint32_t words = bcmc_column_words(dev);

    // The matrix comes from the peripheral, programmed by the real driver. The
    // observer never sees a weight or an offset.
    m->begin();
    const bcmc_status_t s = bcmc_load(dev, c.weights.data(), c.N, c.C);
    check(s == BCMC_OK, "FAIL: %s:%d: bcmc_load returned %s\n", path, c.line,
          bcmc_strstatus(s));

    // Two orders. The permuted one is seeded from the case so that a failure
    // is reproducible from the vector file alone.
    std::vector<uint32_t> order(c.N, 0u);
    std::vector<uint32_t> scratch((c.N + 31u) / 32u, 0u);
    const uint32_t        seed = 0x9E3779B9u ^ static_cast<uint32_t>(c.line);

    // Building an order is arithmetic, not communication.
    m->begin();
    check(bcmc_order_permuted(order.data(), c.N, seed) == BCMC_OK,
          "FAIL: %s:%d: the shuffle refused N = %u\n", path, c.line, c.N);
    check(m->cost() == 0,
          "FAIL: %s:%d: building an order cost %llu bus accesses; it must cost "
          "none\n",
          path, c.line, (unsigned long long)m->cost());
    check(bcmc_order_is_bijection(order.data(), c.N, scratch.data(),
                                  static_cast<uint32_t>(scratch.size())),
          "FAIL: %s:%d: the shuffled order is not a bijection of 0..%u\n", path,
          c.line, c.N - 1u);

    bcmc_observer_t seq;
    bcmc_observer_t perm;
    m->begin();
    check(bcmc_observer_init_sequential(&seq, dev, c.N) == BCMC_OK,
          "FAIL: %s:%d: the sequential observer refused to start\n", path, c.line);
    check(bcmc_observer_init(&perm, dev, order.data(), c.N) == BCMC_OK,
          "FAIL: %s:%d: the permuted observer refused to start\n", path, c.line);
    check(m->cost() == 0,
          "FAIL: %s:%d: starting a pass cost %llu bus accesses; a cursor is not "
          "a transaction\n",
          path, c.line, (unsigned long long)m->cost());

    // peek() is the order without the traversal: no bus, no advance.
    for (uint32_t t = 0; t < c.N; t++) {
        uint32_t want = 0;
        m->begin();
        check(bcmc_observer_peek(&perm, t, &want) == BCMC_OK,
              "FAIL: %s:%d: peek(%u) refused\n", path, c.line, t);
        check(m->cost() == 0, "FAIL: %s:%d: peek touched the bus\n", path, c.line);
        check(want == order[t], "FAIL: %s:%d: peek(%u) = %u, order says %u\n", path,
              c.line, t, want, order[t]);
        uint32_t ignored = 0;
        check(bcmc_observer_peek(&seq, t, &ignored) == BCMC_OK && ignored == t,
              "FAIL: %s:%d: the sequential observer peeked %u at step %u\n", path,
              c.line, ignored, t);
    }
    uint32_t past = 0;
    check(bcmc_observer_peek(&perm, c.N, &past) == BCMC_ERANGE,
          "FAIL: %s:%d: peek past the end of a pass was answered\n", path, c.line);

    m->begin();
    const std::vector<Visit> pass_seq = run_pass(&seq, m, words, max_c, "sequential");
    m->begin();
    const std::vector<Visit> pass_perm = run_pass(&perm, m, words, max_c, "permuted");

    // O1, from outside: a pass is exactly N visits and covers every column.
    check(pass_seq.size() == c.N,
          "FAIL: %s:%d: the sequential pass made %u visits, N = %u\n", path, c.line,
          static_cast<unsigned>(pass_seq.size()), c.N);
    check(pass_perm.size() == c.N,
          "FAIL: %s:%d: the permuted pass made %u visits, N = %u\n", path, c.line,
          static_cast<unsigned>(pass_perm.size()), c.N);

    // O3, and the promise peek() made: step t visits pi(t), exactly. Without
    // this the two passes below could agree simply because both of them ran in
    // column order, and P4 would be a tautology rather than a claim.
    for (uint32_t t = 0; t < c.N; t++) {
        check(pass_perm[t].col == order[t],
              "FAIL: %s:%d: step %u visited column %u, pi(%u) = %u\n", path, c.line,
              t, pass_perm[t].col, t, order[t]);
    }

    // O2: visiting j yields R(j) -- reference.py's rows, ascending -- and the
    // sequential observer visits j at step j, so the two indexings coincide.
    for (uint32_t t = 0; t < c.N; t++) {
        const Visit& v = pass_seq[t];
        check(v.col == t, "FAIL: %s:%d: the sequential observer visited %u at step "
                          "%u\n",
              path, c.line, v.col, t);

        std::vector<uint32_t> want;
        for (uint32_t r = 0; r < c.C; r++) {
            if (c.rows[r][t]) want.push_back(r);
        }
        check(v.rows == want,
              "FAIL: %s:%d: column %u yielded %u rows, reference.py says %u\n", path,
              c.line, t, static_cast<unsigned>(v.rows.size()),
              static_cast<unsigned>(want.size()));
        for (std::size_t k = 0; k < want.size(); k++) {
            check(v.rows[k] == want[k],
                  "FAIL: %s:%d: column %u row %u = %u, reference.py says %u\n", path,
                  c.line, t, static_cast<unsigned>(k), v.rows[k], want[k]);
        }

        // L(j), through the observer's popcount rather than through the
        // harness's loop. The Balance Theorem constrains this number; the
        // observer only counts it.
        check(v.load == c.occupancy[t],
              "FAIL: %s:%d: L(%u) = %u, reference.py says %u\n", path, c.line, t,
              v.load, c.occupancy[t]);
    }

    // P1: W events, the support of M, once each.
    const std::vector<std::pair<uint32_t, uint32_t> > ev_seq  = events_of(pass_seq);
    const std::vector<std::pair<uint32_t, uint32_t> > ev_perm = events_of(pass_perm);
    check(ev_seq.size() == c.total_weight(),
          "FAIL: %s:%d: a pass emitted %u events, W = %u\n", path, c.line,
          static_cast<unsigned>(ev_seq.size()), c.total_weight());
    for (std::size_t k = 1; k < ev_seq.size(); k++) {
        check(ev_seq[k] != ev_seq[k - 1],
              "FAIL: %s:%d: event (row %u, column %u) was emitted twice\n", path,
              c.line, ev_seq[k].first, ev_seq[k].second);
    }

    // P4: the same events, in a different sequence. This is the sentence at the
    // top of docs/Observers.md made checkable -- an observer contributes order
    // and nothing else.
    check(ev_seq == ev_perm,
          "FAIL: %s:%d: the two observers disagreed about which events occur\n",
          path, c.line);

    // P2: row conservation. Row i is emitted weight[i] times whatever the
    // order, which is the statement that traversal moves work in time without
    // creating or destroying any.
    std::vector<uint32_t> per_row(c.C, 0u);
    for (const std::pair<uint32_t, uint32_t>& e : ev_perm) {
        check(e.first < c.C, "FAIL: %s:%d: an event named row %u, but C = %u\n", path,
              c.line, e.first, c.C);
        per_row[e.first]++;
    }
    for (uint32_t i = 0; i < c.C; i++) {
        check(per_row[i] == c.weights[i],
              "FAIL: %s:%d: row %u was emitted %u times, its weight is %u\n", path,
              c.line, i, per_row[i], c.weights[i]);
    }

    // P3: the multiset of occupancies is observer-invariant. Sorted, because a
    // permuted observer meets the heavy columns at different moments -- which
    // is the entire point -- and the claim is about the collection, not the
    // sequence.
    std::vector<uint32_t> load_seq;
    std::vector<uint32_t> load_perm;
    std::vector<uint32_t> load_ref(c.occupancy);
    for (const Visit& v : pass_seq) load_seq.push_back(v.load);
    for (const Visit& v : pass_perm) load_perm.push_back(v.load);
    std::sort(load_seq.begin(), load_seq.end());
    std::sort(load_perm.begin(), load_perm.end());
    std::sort(load_ref.begin(), load_ref.end());
    check(load_seq == load_ref,
          "FAIL: %s:%d: the sequential pass saw a different balance profile than "
          "reference.py recorded\n",
          path, c.line);
    check(load_perm == load_ref,
          "FAIL: %s:%d: reading out of order changed the balance profile; the "
          "Balance Theorem is a property of the matrix\n",
          path, c.line);

    // And a rewind is the same pass again, which is what makes a permuted
    // observer reproducible without re-shuffling.
    bcmc_observer_rewind(&perm);
    check(!bcmc_observer_at_end(&perm),
          "FAIL: %s:%d: a rewound cursor was still at the end\n", path, c.line);
    m->begin();
    const std::vector<Visit> again = run_pass(&perm, m, words, max_c, "rewound");
    check(again.size() == pass_perm.size(),
          "FAIL: %s:%d: the rewound pass had a different length\n", path, c.line);
    for (std::size_t t = 0; t < again.size(); t++) {
        check(again[t].col == pass_perm[t].col && again[t].rows == pass_perm[t].rows,
              "FAIL: %s:%d: the rewound pass diverged at step %u\n", path, c.line,
              static_cast<unsigned>(t));
    }
}

//---------------------------------------------------------------------------
// Suite 6: what a cursor refuses
//
// Geometry, refused locally and for free; and a device that has not been
// probed, refused because a visit needs a column width that only CAPS knows.
// Both are the rule sw/bcmc.h already lives by, inherited rather than restated.
//---------------------------------------------------------------------------

void suite_cursor_refusals(bcmc_dev_t* dev, Meter* m) {
    const uint32_t        words = bcmc_column_words(dev);
    std::vector<uint32_t> bits(words ? words : 1u, 0u);
    const uint32_t        order[4] = {0, 1, 2, 3};
    bcmc_observer_t       ob;
    uint32_t              col = 0;

    CALL(*m, BCMC_EINVAL, 0, bcmc_observer_init(NULL, dev, order, 4));
    CALL(*m, BCMC_EINVAL, 0, bcmc_observer_init(&ob, NULL, order, 4));
    CALL(*m, BCMC_EINVAL, 0, bcmc_observer_init(&ob, dev, NULL, 4));
    CALL(*m, BCMC_ERANGE, 0, bcmc_observer_init(&ob, dev, order, 0));
    CALL(*m, BCMC_ERANGE, 0, bcmc_observer_init_sequential(&ob, dev, 0));

    // An unprobed device: attached, so the accessors work, but with no
    // geometry, so a visit cannot be sized. It must be refused before any
    // access, not after a read whose width was guessed.
    bcmc_dev_t blind;
    check(bcmc_attach(&blind, 0x0, dev->read, dev->write, dev->ctx) == BCMC_OK,
          "FAIL: refusals: bcmc_attach refused a well-formed device\n");
    CALL(*m, BCMC_EINVAL, 0, bcmc_observer_init_sequential(&ob, &blind, 4));
    CALL(*m, BCMC_EINVAL, 0, bcmc_observer_init(&ob, &blind, order, 4));

    CALL(*m, BCMC_OK, 0, bcmc_observer_init(&ob, dev, order, 4));
    CALL(*m, BCMC_EINVAL, 0, bcmc_observer_next(&ob, NULL, bits.data(), words));
    CALL(*m, BCMC_EINVAL, 0, bcmc_observer_next(&ob, &col, NULL, words));
    check(bcmc_observer_at_end(NULL),
          "FAIL: refusals: a null cursor is not in the middle of a pass\n");
}

}  // namespace

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);

    uint64_t                 limit = 0;
    std::string              vcd;
    std::string              prng_path;
    std::string              order_path;
    std::vector<std::string> files;

    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        if (arg == "--limit" && i + 1 < argc) {
            limit = std::strtoull(argv[++i], nullptr, 10);
        } else if (arg == "--vcd" && i + 1 < argc) {
            vcd = argv[++i];
        } else if (arg == "--prng" && i + 1 < argc) {
            prng_path = argv[++i];
        } else if (arg == "--order" && i + 1 < argc) {
            order_path = argv[++i];
        } else if (arg.rfind("--", 0) == 0) {
            continue;  // Verilated::commandArgs took it
        } else {
            files.push_back(arg);
        }
    }

    if (prng_path.empty() || order_path.empty() || files.empty()) {
        std::printf("usage: bcmc_observer_test [--limit K] [--vcd PATH] "
                    "--prng observer_prng.txt --order observer_order.txt "
                    "<matrix_vectors.txt>...\n");
        return 2;
    }

    // The two table suites first, and deliberately before any hardware exists:
    // if the C generator has drifted from Python there is no point simulating
    // anything, and the failure message should say so plainly.
    const std::vector<PrngCase>  prng  = load_prng_vectors(prng_path);
    const std::vector<OrderCase> table = load_order_vectors(order_path);
    suite_prng(prng, prng_path.c_str());
    suite_orders(table, order_path.c_str());
    suite_bijection();
    suite_decode();

#if VM_TRACE
    if (!vcd.empty()) Verilated::traceEverOn(true);
#endif

    Vbcmc_wb dut;

#if VM_TRACE
    VerilatedVcdC* trace = nullptr;
    if (!vcd.empty()) {
        trace = new VerilatedVcdC;
        dut.trace(trace, 99);
        trace->open(vcd.c_str());
    }
#endif

    Platform plat;
    uint64_t sample = 0;

    bcmc::WbMaster bus(bcmc::wb_signals(&dut, [&]() {
        sample++;
#if VM_TRACE
        if (trace) trace->dump(sample);
#else
        (void)sample;
#endif
    }));
    plat.bus = &bus;

    bus.reset();

    bcmc_dev_t dev;
    if (bcmc_attach(&dev, 0x0, plat_read, plat_write, &plat) != BCMC_OK) {
        fail("FAIL: bcmc_attach refused a well-formed device\n");
    }
    if (bcmc_probe(&dev) != BCMC_OK) {
        fail("FAIL: bcmc_probe could not identify the peripheral\n");
    }

    Meter meter(&bus, &plat);
    suite_cursor_refusals(&dev, &meter);

    uint64_t cases  = 0;
    uint64_t visits = 0;
    for (const std::string& path : files) {
        std::vector<bcmc::MatrixCase> vec;
        try {
            vec = bcmc::load_matrix_vectors(path);
        } catch (const std::exception& e) {
            fail("FAIL: %s: %s\n", path.c_str(), e.what());
        }

        for (const bcmc::MatrixCase& c : vec) {
            if (limit != 0 && cases >= limit) break;
            // A recording made for a wider instance is not this instance's to
            // check; the same rule sim/bcmc_driver_test.cpp follows.
            if (c.C > dev.max_c) continue;
            suite_matrix(&dev, &meter, c, path.c_str());
            cases++;
            visits += 3u * c.N;  // sequential, permuted, rewound
        }
        if (limit != 0 && cases >= limit) break;
    }

    if (cases == 0) fail("FAIL: no usable matrix cases were observed\n");

    if (plat.timeouts != 0 || plat.duplicates != 0) {
        fail("FAIL: bus protocol: %llu timeouts, %llu duplicate responses\n",
             (unsigned long long)plat.timeouts,
             (unsigned long long)plat.duplicates);
    }

    dut.final();

#if VM_TRACE
    if (trace) {
        trace->close();
        delete trace;
    }
#endif

    std::printf("bcmc_observer_test: PASS  %llu seeds, %llu traversals, "
                "%llu matrices, %llu visits, %llu checks, %llu accesses\n",
                (unsigned long long)prng.size(), (unsigned long long)table.size(),
                (unsigned long long)cases, (unsigned long long)visits,
                (unsigned long long)g_checks, (unsigned long long)bus.accesses());
    return 0;
}

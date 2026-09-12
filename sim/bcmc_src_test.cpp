//===========================================================================
// sim/bcmc_src_test.cpp -- the traversal sources under Verilator
//
// The second simulator, over the SAME corpora the Icarus benches consume.
// It shares no code with them: the corpora are re-read here in C++, and each
// source's expected values are derived independently -- for the affine from
// pi(t) = (a t + b) mod N computed with a real multiply and modulus, and for
// the shuffled source from the bank permutations the corpus already carries
// (which come from the pinned software family, not from this RTL).
//
//   bcmc_src_test affine   vectors/srcaff_edge.txt
//   bcmc_src_test shuffled vectors/srcshuf_edge.txt
//   bcmc_src_test identity
//
// What is compared is the declared contract, nothing else:
//
//   affine    ready, a_out, b_out (while ready), and ts_pi on the walk
//   shuffled  ready, ts_pi, and the underrun flag's agreement with the handoff
//   identity  ts_pi == ts_t, which is a framework check rather than a proof
//
// Nothing here reaches into derivation internals -- the mask register, the
// generator state, the Euclid pair, the countdown -- because none of them is a
// port. The Icarus benches are kept as they are: an independent path, not a
// common implementation behind two front ends.
//
// Preparation latency is NOT contractual (`docs/Traversal_Sources_Specification`
// section 4.4), so `ready` is polled to a bound rather than counted to.
//===========================================================================

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "verilated.h"

#include "Vbcmc_src_affine.h"
#include "Vbcmc_src_identity.h"
#include "Vbcmc_src_shuffled.h"

namespace {

int g_checks = 0;
int g_errors = 0;

void fail(const std::string& where, const std::string& what) {
    if (g_errors < 24) {
        std::printf("  FAIL %s: %s\n", where.c_str(), what.c_str());
    }
    g_errors++;
}

void check_eq(const std::string& where, const char* what, long long got,
              long long want) {
    g_checks++;
    if (got != want) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "%s: got %lld (%llx), want %lld (%llx)",
                      what, got, static_cast<unsigned long long>(got), want,
                      static_cast<unsigned long long>(want));
        fail(where, buf);
    }
}

//--- reading -----------------------------------------------------------------

struct Record {
    std::string tag;
    std::vector<std::string> words;   // every field, tag excluded
};

// Fields are read POSITIONALLY, because the bases differ within a line: the
// benches read the seed with `%h` and everything else with `%d`. A numeric scan
// would mis-read a bare-hex seed like `BEEF` -- and worse, would silently drop
// it, misaligning every later field.
long long dec(const Record& r, std::size_t i) {
    return std::strtoll(r.words.at(i).c_str(), nullptr, 10);
}
long long hex(const Record& r, std::size_t i) {
    return static_cast<long long>(
        std::strtoull(r.words.at(i).c_str(), nullptr, 16));
}

// The corpora are scripts, not tables: the file is read once into records and
// each mode interprets the tags it knows. Comments and separators are skipped.
bool read_records(const std::string& path, std::vector<Record>& out) {
    std::ifstream fh(path);
    if (!fh) {
        std::printf("FAIL  cannot open %s\n", path.c_str());
        return false;
    }
    std::string line;
    while (std::getline(fh, line)) {
        std::istringstream is(line);
        std::string tok;
        if (!(is >> tok)) continue;
        if (tok[0] == '#' || tok == "---") continue;
        Record r;
        r.tag = tok;
        // A field is a word. `---` separators trail a line only when the corpus
        // writes them there, and they are skipped for the same reason.
        while (is >> tok) {
            if (tok == "---") break;
            r.words.push_back(tok);
        }
        out.push_back(r);
    }
    return true;
}

// `R` names and `B` bank tags carry words; everything else is numeric.

//--- the affine source -------------------------------------------------------

// pi(t) = (a t + b) mod N, computed the expensive way on purpose: the point is
// that this shares no arithmetic with the accumulator under test.
long long affine_pi(long long t, long long a, long long b, long long n) {
    return (a * t + b) % n;
}

struct Affine {
    Vbcmc_src_affine* dut;
    long long N = 1, A = 0, B = 0;

    void clock() {
        dut->clk = 1; dut->eval();
        dut->clk = 0; dut->eval();
    }
    // Section 4.4: the derivation takes however many cycles it takes, so the
    // harness waits for the handshake instead of counting to a schedule.
    void poll_ready(const std::string& where, long long bound = 400000) {
        for (long long i = 0; i < bound; i++) {
            dut->eval();
            if (dut->ready) return;
            clock();
        }
        fail(where, "ready never asserted");
    }
};

int run_affine(const std::string& path) {
    std::vector<Record> recs;
    if (!read_records(path, recs)) return 1;

    Vbcmc_src_affine dut;
    Affine m;
    m.dut = &dut;
    dut.clk = 0; dut.rst = 1; dut.N = 1; dut.seed = 0;
    dut.load = 0; dut.ts_t = 0;
    dut.eval();
    // A reset of the whole file first, so every run starts from the same place.
    for (int i = 0; i < 4; i++) m.clock();
    dut.rst = 0;

    int runs = 0, walks = 0, walk_cycles = 0;
    for (const Record& r : recs) {
        if (r.tag == "R") {
            const std::string name = r.words.empty() ? "?" : r.words[0];
            m.N = dec(r, 1);
            dut.N = static_cast<IData>(m.N & 0xFFFF);
            dut.seed = static_cast<IData>(hex(r, 2) & 0xFFFFFFFFLL);
            m.A = dec(r, 4); m.B = dec(r, 5);
            const long long rst_cycles = dec(r, 3);

            dut.rst = 1;
            for (long long i = 0; i < rst_cycles; i++) m.clock();
            dut.rst = 0;
            dut.ts_t = 0;
            runs++;

            m.poll_ready(name);
            check_eq(name, "a_out", dut.a_out & 0xFFFF, m.A);
            check_eq(name, "b_out", dut.b_out & 0xFFFF, m.B);

        } else if (r.tag == "C") {
            const long long load = dec(r, 0);
            m.N = dec(r, 1);
            dut.N = static_cast<IData>(m.N & 0xFFFF);
            dut.seed = static_cast<IData>(hex(r, 2) & 0xFFFFFFFFLL);
            m.A = dec(r, 3); m.B = dec(r, 4);
            const std::string where = "C(N=" + std::to_string(m.N) + ")";

            // The change must void readiness BEFORE another pass can be admitted,
            // which is section 4.4's rule for N as much as for the seed.
            if (load) { dut.load = 1; m.clock(); dut.load = 0; }
            else      { m.clock(); }
            dut.eval();
            if (dut.ready) fail(where, "ready stayed high across the change");

            m.poll_ready(where);
            check_eq(where, "a_out", dut.a_out & 0xFFFF, m.A);
            check_eq(where, "b_out", dut.b_out & 0xFFFF, m.B);

        } else if (r.tag == "W") {
            const std::string where = "W(N=" + std::to_string(m.N) + ")";
            walks++;
            for (std::size_t i = 0; i < r.words.size(); i++) {
                const long long t = std::strtoll(r.words[i].c_str(), nullptr, 10);
                dut.ts_t = static_cast<IData>(t & 0xFFFF);
                dut.eval();
                walk_cycles++;
                check_eq(where, "ts_pi", dut.ts_pi & 0xFFFF,
                         affine_pi(t, m.A, m.B, m.N));
                m.clock();
            }
        } else if (r.tag == "P") {
            // A reset pulse away from the derivation: `at` idle cycles, then
            // `hold` cycles of rst, then the handshake again.
            const std::string where = "P";
            for (long long i = 0; i < dec(r, 0); i++) m.clock();
            dut.rst = 1;
            for (long long i = 0; i < dec(r, 1); i++) m.clock();
            dut.rst = 0;
            dut.ts_t = 0;
            m.poll_ready(where);
        }
    }

    std::printf("src_affine: %d runs, %d walks, %d walk cycles, %d checks\n",
                runs, walks, walk_cycles, g_checks);
    return g_errors == 0 ? 0 : 1;
}

//--- the shuffled source -----------------------------------------------------

int run_shuffled(const std::string& path) {
    std::vector<Record> recs;
    if (!read_records(path, recs)) return 1;

    Vbcmc_src_shuffled dut;
    dut.clk = 0; dut.rst = 1; dut.N = 1; dut.seed = 0; dut.load = 0; dut.ts_t = 0;
    dut.eval();

    auto clk = [&]() { dut.clk = 1; dut.eval(); dut.clk = 0; dut.eval(); };
    // There is no promised fill cycle count (section 5.4), so the only honest
    // thing a harness can do with `ready` is wait for it, or wait to see it stay
    // low.
    auto wait_ready = [&](const std::string& where, bool want) {
        for (long long i = 0; i < 40000000; i++) {
            dut.eval();
            if ((dut.ready != 0) == want) return;
            clk();
        }
        fail(where, want ? "ready never asserted" : "ready never cleared");
    };

    long long N = 1;
    int kind = 0;                       // 0 = q, 1 = u, 2 = n
    long long cur = -1;                 // the bank the previous pass took
    std::vector<std::vector<long long>> banks;
    int runs = 0, passes = 0;
    long long asks = 0;

    for (const Record& r : recs) {
        if (r.tag == "R") {
            const std::string name = r.words.empty() ? "?" : r.words[0];
            const char k = (r.words.size() > 1) ? r.words[1][0] : 'q';
            kind = (k == 'u') ? 1 : (k == 'n') ? 2 : 0;
            N = dec(r, 2);
            dut.N = static_cast<IData>(N & 0xFFFF);
            dut.seed = static_cast<IData>(hex(r, 3) & 0xFFFFFFFFLL);
            cur = -1;
            banks.clear();
            runs++;
            dut.ts_t = 0;
            dut.rst = 1;
            for (long long i = 0; i < dec(r, 4); i++) clk();
            dut.rst = 0;
            if (kind != 2) wait_ready(name, true);

        } else if (r.tag == "B") {
            // The expected banks come from the corpus, which took them from the
            // pinned software family -- not from this RTL's bank state.
            const long long k = dec(r, 0);
            if (static_cast<long long>(banks.size()) <= k) banks.resize(k + 1);
            banks[k].clear();
            for (std::size_t i = 1; i < r.words.size(); i++) {
                banks[k].push_back(
                    std::strtoll(r.words[i].c_str(), nullptr, 10));
            }

        } else if (r.tag == "P") {
            const std::string where = "pass " + std::to_string(dec(r, 0));
            passes++;
            dut.ts_t = 0;
            for (long long i = 0; i < dec(r, 1); i++) clk();

            const long long gap = dec(r, 2);
            bool saw_new = false, saw_rep = false;
            bool indist = (cur >= 0) &&
                          ((cur + 1) < static_cast<long long>(banks.size()));
            for (long long t = 0; t < N; t++) {
                dut.ts_t = static_cast<IData>(t & 0xFFFF);
                for (long long g = 0; g < gap; g++) {
                    dut.eval();
                    asks++;
                    const long long got = dut.ts_pi & 0xFFFF;
                    if (!dut.ready) fail(where, "ready low during a pass");
                    if (cur < 0) {
                        check_eq(where, "the first pass is not bank 0", got,
                                 banks[0][t]);
                    } else {
                        const bool new_ok =
                            ((cur + 1) < static_cast<long long>(banks.size())) &&
                            (got == banks[cur + 1][t]);
                        const bool rep_ok = (got == banks[cur][t]);
                        if (new_ok)      saw_new = true;
                        else if (rep_ok) saw_rep = true;
                        else fail(where, "ts_pi is neither the new bank nor the"
                                         " repeat");
                        // N = 1 makes every bank [0], so "new" and "repeat" are
                        // one observation there and the verdict cannot demand
                        // exactly one. Checked against the corpus: only that run
                        // is indistinguishable.
                        if (((cur + 1) < static_cast<long long>(banks.size())) &&
                            (banks[cur + 1][t] != banks[cur][t])) indist = false;
                    }
                    clk();
                }
            }

            // The verdict: which bank, and does the flag agree with it.
            if (cur < 0) {
                if (kind == 0 && dut.underrun)
                    fail(where, "the first pass reported an underrun");
                cur = 0;
            } else if (saw_new && !saw_rep) {
                if (dut.underrun)
                    fail(where, "a new bank was taken but underrun says"
                                " otherwise");
                cur++;
            } else if (saw_rep && !saw_new) {
                if (kind == 0) fail(where, "a repeat in a qualified run");
                if (!dut.underrun)
                    fail(where, "a repeat without underrun -- the flag must say"
                                " so");
            } else if (indist) {
                // Indistinguishable, and its asks were checked above.
            } else {
                fail(where, "the pass matched neither bank, or both");
            }

        } else if (r.tag == "D") {
            const std::string where = "D";
            dut.ts_t = 0;
            for (long long i = 0; i < dec(r, 0); i++) {
                dut.eval();
                if (kind == 2) {
                    if (dut.ready)
                        fail(where, "ready asserted for an unservable N");
                } else if (!dut.ready) {
                    fail(where, "ready low while idle");
                }
                clk();
            }

        } else if (r.tag == "X") {
            const std::string where = "X";
            // Section 8.2: IDLE drives ts_t = 0 and a reset does not suspend
            // that. Leaving ts_t at its last ask would make the next pass's ask
            // for step 0 look like a wrap and hand in the wrong bank.
            dut.ts_t = 0;
            dut.rst = 1;
            for (long long i = 0; i < dec(r, 0); i++) clk();
            dut.rst = 0;
            cur = -1;                       // a reset restarts the stream
            wait_ready(where, true);
            if (dut.underrun) fail(where, "a reset did not clear underrun");

        } else if (r.tag == "L") {
            dut.ts_t = 0;
            dut.seed = static_cast<IData>(hex(r, 0) & 0xFFFFFFFFLL);
            dut.load = 1; clk(); dut.load = 0;

        } else if (r.tag == "E") {
            dut.eval();
            if (dut.ready) fail("E", "ready asserted when it must not be");
        }
    }

    std::printf("src_shuffled: %d runs, %d passes, %lld asks, %d checks\n",
                runs, passes, asks, g_checks);
    return g_errors == 0 ? 0 : 1;
}

//--- the identity source -----------------------------------------------------

// The identity is one `assign`, and a bench that drives ts_t and checks
// ts_pi == ts_t is testing it against itself. The specification says as much and
// gives its real verification as the seam regression. It is exercised here for a
// different reason: to establish that this framework drives a source with no
// clk, no rst and no handshake -- which is the shape of the seam itself.
int run_identity() {
    Vbcmc_src_identity dut;
    const long long widths[] = {1, 2, 3, 5, 16, 255, 256, 4095, 65535};
    int sweeps = 0;
    for (long long n : widths) {
        for (long long t = 0; t < n; t++) {
            dut.ts_t = static_cast<IData>(t & 0xFFFF);
            dut.eval();
            check_eq("identity", "ts_pi", dut.ts_pi & 0xFFFF, t);
        }
        sweeps++;
    }
    std::printf("src_identity: %d sweeps, %d checks (framework control)\n",
                sweeps, g_checks);
    return g_errors == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    if (argc < 2) {
        std::printf("usage: bcmc_src_test {affine|shuffled|identity} [corpus]\n");
        return 2;
    }
    const std::string mode = argv[1];
    int rc = 0;
    if (mode == "affine" || mode == "shuffled") {
        if (argc < 3) {
            std::printf("FAIL  %s needs a corpus\n", mode.c_str());
            return 2;
        }
        rc = (mode == "affine") ? run_affine(argv[2]) : run_shuffled(argv[2]);
    } else if (mode == "identity") {
        rc = run_identity();
    } else {
        std::printf("FAIL  unknown mode %s\n", mode.c_str());
        return 2;
    }
    if (rc == 0) {
        std::printf("bcmc_src_test: PASS  %s, %d checks\n", mode.c_str(), g_checks);
    } else {
        std::printf("bcmc_src_test: FAIL  %s, %d checks, %d errors\n",
                    mode.c_str(), g_checks, g_errors);
    }
    return rc;
}

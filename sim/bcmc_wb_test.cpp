//===========================================================================
// bcmc_wb_test.cpp -- replay recorded bus conversations against rtl/bcmc_wb.v
//
// Every other test in this directory compares a combinational or streamed
// result against a value that validation/reference.py computed. This one does
// the same thing to a protocol: validation/gen_wb_vectors.py drove
// validation/bcmc_periph.py through the sequences of
// docs/Transaction_Sequences.md and wrote down what happened, and this program
// replays that conversation on real wires.
//
//     docs/Register_Map.md -> bcmc_periph.py -> wb_*.txt -> THIS -> bcmc_wb.v
//
// Nothing here decides what the right answer is. There is not a single
// expected register value in this file; the only judgements it makes on its
// own are protocol invariants that the model cannot express because the model
// has no wires:
//
//   * ack and err are never asserted together
//   * every access is answered exactly once -- neither a timeout nor a second
//     response to the same request
//   * the interrupt pin equals STATUS.IRQ & CTRL.IRQ_EN, which is checked by
//     reading both registers back rather than by remembering what was written
//
// THE ERR CASES ARE THE POINT
//
// One access in seven of wb_sequences.txt expects ERR -- 25 of them against
// 154 that expect ACK -- and they are recorded inline among the successes
// rather than in a file of their own. That is deliberate: an address decoder
// that accepts too much is a far more common defect than one that accepts too
// little, and "the access was refused" is the only place the hardware can say
// so. docs/Register_Map.md chose ERR over ACK-with-zero for exactly this
// reason -- silent success hides bugs.
//
//     bcmc_wb_test [--limit K] [--vcd PATH] <wb_vectors.txt>...
//
// The geometry is not a parameter of the replay. A recording contains
// addresses that only exist for one MAX_C, so the first thing every file does
// is read CAPS; if this program is ever built against a differently
// parameterised bcmc_wb, that read fails and says so.
//===========================================================================

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "Vbcmc_wb.h"
#include "vectors.h"
#include "verilated.h"
#include "wb_bfm.h"

#if VM_TRACE
#include "verilated_vcd_c.h"
#endif

namespace {

// Addresses the harness reads for its own purposes. They are not expectations:
// no value read from them is compared against a constant, only against another
// value read from the device.
constexpr uint32_t kRegCtrl    = 0x00C;
constexpr uint32_t kRegStatus  = 0x010;
constexpr uint32_t kCtrlIrqEn  = 1u << 1;
constexpr uint32_t kStatusIrq  = 1u << 2;

// A poll that never finishes is a hung transform, and a hung transform must be
// reported rather than waited on for ever.
constexpr int kPollLimit = 4096;

int      failures      = 0;
uint64_t values_checked = 0;

void fail(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

void fail(const char* fmt, ...) {
    ++failures;
    if (failures > 20) {
        if (failures == 21) std::printf("  ... further failures suppressed\n");
        return;
    }
    std::va_list args;
    va_start(args, fmt);
    std::vprintf(fmt, args);
    va_end(args);
}

const char* op_name(bcmc::BusOp::Kind kind) {
    switch (kind) {
        case bcmc::BusOp::kRead:  return "R";
        case bcmc::BusOp::kWrite: return "W";
        case bcmc::BusOp::kPoll:  return "P";
        case bcmc::BusOp::kReset: return "Z";
        case bcmc::BusOp::kLabel: return "L";
    }
    return "?";
}

// Describes an outcome the way the vector files do, so that a failure message
// can be read against the file it came from.
std::string outcome(const bcmc::WbResponse& r) {
    if (r.timeout) return "no response";
    if (r.err) return "ERR";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "ACK %08X", r.data);
    return buf;
}

// The interrupt pin carries no information of its own: docs/Register_Map.md
// defines it as the AND of a status bit and a control bit, both of which are
// readable. So the pin can be checked without believing anything -- read the
// two registers and compare. STATUS and CTRL are accessible in every state,
// which is what makes this safe to do at any point in a replay.
void audit_irq(bcmc::WbMaster& bus, const Vbcmc_wb& dut, const char* where) {
    const bcmc::WbResponse status = bus.read(kRegStatus);
    const bcmc::WbResponse ctrl   = bus.read(kRegCtrl);
    if (!status.ack() || !ctrl.ack()) {
        fail("FAIL: %s: STATUS/CTRL must always be readable (%s, %s)\n", where,
             outcome(status).c_str(), outcome(ctrl).c_str());
        return;
    }
    const bool expected = (status.data & kStatusIrq) && (ctrl.data & kCtrlIrqEn);
    const bool actual   = dut.irq_o != 0;
    ++values_checked;
    if (expected != actual) {
        fail("FAIL: %s: irq_o = %d, but STATUS.IRQ = %d and CTRL.IRQ_EN = %d\n",
             where, actual ? 1 : 0, (status.data & kStatusIrq) ? 1 : 0,
             (ctrl.data & kCtrlIrqEn) ? 1 : 0);
    }
}

// Replays one file. Returns the number of ops executed.
uint64_t replay(const std::string& path, bcmc::WbMaster& bus, const Vbcmc_wb& dut,
                uint64_t limit) {
    const std::vector<bcmc::BusOp> ops = bcmc::load_bus_vectors(path);

    std::string label = "(start)";
    uint64_t    done  = 0;

    for (const bcmc::BusOp& op : ops) {
        if (done >= limit) break;

        char where[256];
        std::snprintf(where, sizeof(where), "%s:%d [%s] %s %03X", path.c_str(),
                      op.line, label.c_str(), op_name(op.kind), op.adr);

        switch (op.kind) {
            case bcmc::BusOp::kLabel:
                audit_irq(bus, dut, label.c_str());
                label = op.label;
                continue;

            case bcmc::BusOp::kReset:
                bus.reset();
                ++done;
                continue;

            case bcmc::BusOp::kRead:
            case bcmc::BusOp::kWrite: {
                const bcmc::WbResponse r =
                    bus.access(op.adr, op.kind == bcmc::BusOp::kWrite, op.data,
                               static_cast<uint8_t>(op.sel));
                ++values_checked;
                ++done;

                if (r.timeout) {
                    fail("FAIL: %s: no ack or err within %d clocks\n", where,
                         bus.timeout_ticks());
                    continue;
                }
                if (r.duplicate) {
                    fail("FAIL: %s: answered twice; the response must be one "
                         "cycle wide\n", where);
                }
                if (r.err != op.err) {
                    fail("FAIL: %s sel %X: expected %s, got %s\n", where, op.sel,
                         op.err ? "ERR" : "ACK", outcome(r).c_str());
                    continue;
                }
                if (op.kind == bcmc::BusOp::kRead && !op.err && r.data != op.rdata) {
                    fail("FAIL: %s: read %08X, expected %08X\n", where, r.data,
                         op.rdata);
                }
                continue;
            }

            case bcmc::BusOp::kPoll: {
                bool matched = false;
                for (int i = 0; i < kPollLimit && !matched; ++i) {
                    const bcmc::WbResponse r = bus.read(op.adr);
                    ++values_checked;
                    if (r.timeout) {
                        fail("FAIL: %s: no ack or err while polling\n", where);
                        break;
                    }
                    if (r.duplicate) {
                        fail("FAIL: %s: answered twice while polling\n", where);
                    }
                    if (r.err) {
                        fail("FAIL: %s: polling a register that errs (%s)\n", where,
                             outcome(r).c_str());
                        break;
                    }
                    matched = (r.data & op.data) == op.rdata;
                }
                ++done;
                if (!matched) {
                    fail("FAIL: %s: (read & %08X) never became %08X in %d reads\n",
                         where, op.data, op.rdata, kPollLimit);
                }
                continue;
            }
        }
    }

    audit_irq(bus, dut, label.c_str());
    return done;
}

}  // namespace

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);

    uint64_t                 limit = UINT64_MAX;
    std::string              vcd;
    std::vector<std::string> files;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--limit" && i + 1 < argc) {
            limit = std::strtoull(argv[++i], nullptr, 10);
        } else if (arg == "--vcd" && i + 1 < argc) {
            vcd = argv[++i];
        } else if (!arg.empty() && arg[0] != '+') {
            files.push_back(arg);
        }
    }

    if (files.empty()) {
        std::printf("usage: bcmc_wb_test [--limit K] [--vcd PATH] "
                    "<wb_vectors.txt>...\n");
        return 2;
    }

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

    uint64_t sample = 0;

    // The one protocol rule that has to be watched continuously rather than
    // sampled at the end of an access: ack and err are alternatives, and a
    // slave that raises both has no defined meaning at all.
    auto after_eval = [&]() {
        if (dut.wb_ack_o && dut.wb_err_o) {
            fail("FAIL: wb_ack_o and wb_err_o asserted together\n");
        }
#if VM_TRACE
        if (trace) trace->dump(sample);
#endif
        ++sample;
    };

    bcmc::WbMaster bus(bcmc::wb_signals(&dut, after_eval));
    bus.reset();

    uint64_t ops = 0;
    try {
        for (const std::string& path : files) {
            ops += replay(path, bus, dut, limit);
        }
    } catch (const std::exception& e) {
        std::printf("FAIL: %s\n", e.what());
        ++failures;
    }

#if VM_TRACE
    if (trace) {
        trace->close();
        delete trace;
    }
#endif

    dut.final();

    std::printf("bcmc_wb_test: %s  %llu ops, %llu checks, %llu clocks\n",
                failures ? "FAIL" : "PASS",
                static_cast<unsigned long long>(ops),
                static_cast<unsigned long long>(values_checked),
                static_cast<unsigned long long>(bus.ticks()));
    return failures ? 1 : 0;
}

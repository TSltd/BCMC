//===========================================================================
// bcmc_driver_test.cpp -- sw/bcmc.c against rtl/bcmc_wb.v
//
//     bcmc_driver_test [--limit K] [--vcd PATH] <matrix_vectors.txt>...
//
// THE DRIVER UNDER TEST IS THE DRIVER
//
// This harness compiles sw/bcmc.c -- the same translation unit a bare-metal
// target would link -- and points its two accessors at bcmc::WbMaster, which
// drives the verilated rtl/bcmc_wb.v. Nothing is mocked and nothing is
// reimplemented. When bcmc_load() writes a weight here, a Wishbone cycle
// happens and a flip-flop in bcmc_context changes state.
//
// That matters because a driver tested against a model of its peripheral only
// proves the two agree, and the interesting bugs live exactly where they
// disagree: the width of a field, the address of a window, which state refuses
// which access. Here the peripheral is the peripheral.
//
// WHERE THE EXPECTED ANSWERS COME FROM
//
// The same place they come from everywhere else in this project:
// validation/reference.py, by way of the matrix_*.txt vector files. This
// harness knows the recursion
//
//     offset[0] = 0,  offset[i+1] = (offset[i] + weight[i]) mod N
//
// not at all, and it must not: it compares what the driver reads back with
// what reference.py recorded. The chain is
//
//     docs/Proof.md -> reference.py -> matrix_*.txt -> bcmc_wb.v
//                                                  -> sw/bcmc.c -> here
//
// so a disagreement anywhere along it fails, and the mathematics is never
// asserted twice.
//
// The bus conversations in wb_*.txt are deliberately NOT reused. Those record
// what validation/bcmc_periph.py does, access by access, and replaying them
// would test the recording, not the driver. What is checked here is that the
// driver, left to choose its own accesses, arrives at reference.py's matrix.
//
// ACCESS COUNTING IS HOW THE COMPOSITION CLAIM IS CHECKED
//
// sw/bcmc.h claims that a primitive is one bus access and that a composition
// adds nothing but sequence. That is a claim about traffic, so the harness
// counts traffic: WbMaster::accesses() before and after every call. It is the
// difference between a documented design rule and an enforced one.
//
// The counts also separate the two kinds of refusal, which is the distinction
// sw/bcmc.h rests on:
//
//     a GEOMETRY refusal (BCMC_ERANGE) costs ZERO accesses -- the driver knew
//     the answer from CAPS
//
//     a STATE refusal (BCMC_EREFUSED) costs EXACTLY ONE -- the driver asked,
//     because only the peripheral knows
//
// A driver that pre-checked BUSY would show up as an extra access. A driver
// that assumed MAX_C would show up as an access that should not exist.
//
//===========================================================================

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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

extern "C" {
#include "bcmc.h"
}

// The geometry this build was elaborated with. It reaches Verilator as -G and
// the C++ as -D from one variable in sim/CMakeLists.txt, so the two cannot
// drift. bcmc_probe() discovers the same numbers from CAPS at run time, and the
// first check below is that discovery and elaboration agree.
#ifndef BCMC_DRIVER_MAX_C
#define BCMC_DRIVER_MAX_C 64
#endif
#ifndef BCMC_DRIVER_VAL_W
#define BCMC_DRIVER_VAL_W 16
#endif
#ifndef BCMC_DRIVER_IDX_W
#define BCMC_DRIVER_IDX_W 16
#endif

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
// The platform, such as it is
//
// Two accessors over WbMaster. They translate a Wishbone response into the
// driver's convention -- zero for ack, non-zero for err -- and nothing else.
//
// A timeout or a duplicate response is NOT reported to the driver, because
// neither is a specified outcome the driver could sensibly handle: they are
// bugs in the slave, and they are recorded here so the run fails with a
// message about the bus rather than a puzzling message about the driver.
//---------------------------------------------------------------------------

struct Platform {
    bcmc::WbMaster* bus       = nullptr;
    uint64_t        timeouts  = 0;
    uint64_t        duplicates = 0;
    uint64_t        errs      = 0;
};

void note(Platform* p, const bcmc::WbResponse& r) {
    if (r.timeout) p->timeouts++;
    if (r.duplicate) p->duplicates++;
    if (r.err) p->errs++;
}

int plat_read(void* ctx, uint32_t addr, uint32_t* data) {
    Platform*         p = static_cast<Platform*>(ctx);
    bcmc::WbResponse  r = p->bus->read(addr);
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

//---------------------------------------------------------------------------
// A bus that is not a BCMC
//
// The one place in this file where the driver is not talking to the RTL, and
// the reason is that the RTL cannot answer the question. bcmc_probe() decides
// whether a peripheral is a BCMC at all, and rtl/bcmc_wb.v is one -- it has no
// setting in which it reports the wrong ID, an unsupported major version, or a
// MAX_C of zero. Refusing a foreign device is therefore only observable
// against a foreign device.
//
// This is three constants and a counter, not a model. It serves ID, VERSION
// and CAPS and errs on everything else, which is all bcmc_probe() looks at, so
// there is no behaviour here to drift out of step with the register map.
//---------------------------------------------------------------------------

struct Foreign {
    uint32_t id       = 0;
    uint32_t version  = 0;
    uint32_t caps     = 0;
    uint64_t accesses = 0;
};

int foreign_read(void* ctx, uint32_t addr, uint32_t* data) {
    Foreign* f = static_cast<Foreign*>(ctx);
    f->accesses++;
    switch (addr) {
        case BCMC_REG_ID: *data = f->id; return 0;
        case BCMC_REG_VERSION: *data = f->version; return 0;
        case BCMC_REG_CAPS: *data = f->caps; return 0;
        default: return -1;
    }
}

int foreign_write(void* ctx, uint32_t addr, uint32_t data) {
    Foreign* f = static_cast<Foreign*>(ctx);
    f->accesses++;
    (void)addr;
    (void)data;
    return -1;
}

//---------------------------------------------------------------------------
// A call, and what it cost
//
// Every judgement below goes through one of these two so that the access count
// is never forgotten. `expect_cost` of -1 means "do not care", which is used
// only where the cost is genuinely data-dependent (a poll loop).
//---------------------------------------------------------------------------

class Meter {
public:
    Meter(bcmc::WbMaster* bus, Platform* plat) : bus_(bus), plat_(plat) {}

    void begin() { mark_ = bus_->accesses(); }
    uint64_t cost() const { return bus_->accesses() - mark_; }

    // Runs `s`, then checks both the status and the traffic it generated.
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

#define CALL(meter, want, cost, expr)                     \
    do {                                                  \
        (meter).begin();                                   \
        bcmc_status_t s_ = (expr);                         \
        (meter).expect(s_, (want), (cost), #expr);         \
    } while (0)

//---------------------------------------------------------------------------
// Suite 1: discovery
//---------------------------------------------------------------------------

void suite_probe(bcmc_dev_t* dev, Meter* m) {
    // Before probing, everything that needs geometry must refuse, and must do
    // it without touching the bus. This is the "never assumes" rule of
    // docs/Register_Map.md, made observable.
    uint32_t v = 0;
    uint32_t words[2] = {0, 0};
    CALL(*m, BCMC_EINVAL, 0, bcmc_set_c(dev, 1));
    CALL(*m, BCMC_EINVAL, 0, bcmc_write_weight(dev, 0, 1));
    CALL(*m, BCMC_EINVAL, 0, bcmc_read_weight(dev, 0, &v));
    CALL(*m, BCMC_EINVAL, 0, bcmc_read_offset(dev, 0, &v));
    CALL(*m, BCMC_EINVAL, 0, bcmc_read_column(dev, 0, words, 2));
    check(bcmc_column_words(dev) == 0,
          "FAIL: probe: an unprobed device claimed to know its column width\n");

    // ID, VERSION, CAPS: three reads, no more.
    CALL(*m, BCMC_OK, 3, bcmc_probe(dev));

    check(dev->max_c == BCMC_DRIVER_MAX_C,
          "FAIL: probe: CAPS reported MAX_C = %u, elaborated with %u\n",
          dev->max_c, (unsigned)BCMC_DRIVER_MAX_C);
    check(dev->val_w == BCMC_DRIVER_VAL_W,
          "FAIL: probe: CAPS reported VAL_W = %u, elaborated with %u\n",
          dev->val_w, (unsigned)BCMC_DRIVER_VAL_W);
    check(dev->idx_w == BCMC_DRIVER_IDX_W,
          "FAIL: probe: CAPS reported IDX_W = %u, elaborated with %u\n",
          dev->idx_w, (unsigned)BCMC_DRIVER_IDX_W);
    check(BCMC_VERSION_MAJOR(dev->version) == BCMC_DRIVER_MAJOR,
          "FAIL: probe: VERSION major %u, driver written for %u\n",
          BCMC_VERSION_MAJOR(dev->version), (unsigned)BCMC_DRIVER_MAJOR);

    const uint32_t want_words = (BCMC_DRIVER_MAX_C + 31u) / 32u;
    check(bcmc_column_words(dev) == want_words,
          "FAIL: probe: column is %u words, expected %u\n",
          bcmc_column_words(dev), want_words);
}

//---------------------------------------------------------------------------
// Suite 1b: probing something that is not a BCMC
//
// Against the Foreign stub, because bcmc_wb.v is a BCMC and cannot pretend
// otherwise. Three refusals, and each must stop at the register that settled
// it -- the access count is how "stopped there" is stated. A probe that read
// CAPS after seeing a foreign ID would be reading a register it has no reason
// to believe exists.
//
// After every refusal the device must still be unprobed, and the last thing
// this suite does is re-probe a device that was already good. That is the
// property which makes a failed probe safe: the geometry of some earlier,
// different peripheral must not survive to be trusted by a range check later.
//---------------------------------------------------------------------------

void suite_foreign(uint32_t good_version, uint32_t good_caps) {
    bcmc_dev_t dev;
    Foreign    f;

    struct Case {
        const char*   what;
        uint32_t      id;
        uint32_t      version;
        uint32_t      caps;
        bcmc_status_t want;
        uint64_t      reads;
    };

    const Case cases[] = {
        // A different peripheral entirely: settled by ID, one read.
        {"a foreign ID", 0xDEADBEEFu, 0, 0, BCMC_ENODEV, 1},
        // Zero is what an absent or held-in-reset slave often reads as.
        {"an ID of zero", 0u, 0, 0, BCMC_ENODEV, 1},
        // A BCMC, but one whose register map this driver was not written for:
        // settled by VERSION, two reads.
        {"a future major version", BCMC_ID_VALUE,
         (BCMC_DRIVER_MAJOR + 1u) << 16, 0, BCMC_EVERSION, 2},
        // A BCMC with no addressable weight window at all: settled by CAPS,
        // three reads.
        {"MAX_C = 0", BCMC_ID_VALUE, 0, 0x10100000u, BCMC_ENODEV, 3},
        // And the same stub, answering correctly, must be accepted -- or the
        // three refusals above would prove nothing but that the stub is broken.
        {"a well-formed BCMC", BCMC_ID_VALUE, 0, 0, BCMC_OK, 3},
    };

    for (const Case& c : cases) {
        f.id      = c.id;
        f.version = (c.want == BCMC_EVERSION) ? c.version : good_version;
        f.caps    = (c.caps != 0) ? c.caps : good_caps;
        f.accesses = 0;

        if (bcmc_attach(&dev, 0x0, foreign_read, foreign_write, &f) != BCMC_OK) {
            fail("FAIL: foreign: bcmc_attach refused the stub\n");
        }
        check(!dev.probed, "FAIL: foreign: bcmc_attach left the device probed\n");

        const bcmc_status_t s = bcmc_probe(&dev);
        check(s == c.want, "FAIL: foreign: %s gave %s, expected %s\n", c.what,
              bcmc_strstatus(s), bcmc_strstatus(c.want));
        check(f.accesses == c.reads,
              "FAIL: foreign: %s took %llu reads, expected %llu\n", c.what,
              (unsigned long long)f.accesses, (unsigned long long)c.reads);

        if (c.want == BCMC_OK) {
            check(dev.probed, "FAIL: foreign: a good probe left the device unprobed\n");
        } else {
            check(!dev.probed,
                  "FAIL: foreign: %s was refused but the device is marked "
                  "probed\n",
                  c.what);
            check(bcmc_column_words(&dev) == 0,
                  "FAIL: foreign: %s was refused but the geometry survived\n",
                  c.what);
        }
    }

    // Re-probing a device that was already good. Every case above started from
    // a fresh bcmc_attach(), which clears `probed` itself, so none of them can
    // tell whether bcmc_probe() also clears it on entry. This can: the loop
    // left `dev` probed, with a real MAX_C, and the same handle is now pointed
    // at a peripheral that is no longer a BCMC.
    //
    // A driver that kept the old geometry would go on range-checking new
    // indices against a width it read from a part that is not there any more --
    // and would do it silently, since a stale MAX_C is a plausible number.
    check(dev.probed && bcmc_column_words(&dev) != 0,
          "FAIL: foreign: the well-formed case did not leave a probed device\n");

    f.id       = 0xDEADBEEFu;
    f.accesses = 0;

    const bcmc_status_t again = bcmc_probe(&dev);
    check(again == BCMC_ENODEV,
          "FAIL: foreign: re-probing a foreign ID gave %s, expected %s\n",
          bcmc_strstatus(again), bcmc_strstatus(BCMC_ENODEV));
    check(f.accesses == 1,
          "FAIL: foreign: re-probe took %llu reads, expected 1\n",
          (unsigned long long)f.accesses);
    check(!dev.probed,
          "FAIL: foreign: a failed re-probe left the device marked probed\n");
    check(bcmc_column_words(&dev) == 0,
          "FAIL: foreign: a failed re-probe left the old column width behind\n");

    // And the geometry really is gone, not merely unreported: the primitives
    // that need it refuse locally again, exactly as they did before any probe.
    f.accesses = 0;
    const bcmc_status_t after = bcmc_set_c(&dev, 1);
    check(after == BCMC_EINVAL,
          "FAIL: foreign: after a failed re-probe bcmc_set_c gave %s, expected "
          "%s\n",
          bcmc_strstatus(after), bcmc_strstatus(BCMC_EINVAL));
    check(f.accesses == 0,
          "FAIL: foreign: after a failed re-probe bcmc_set_c touched the bus "
          "%llu times\n",
          (unsigned long long)f.accesses);
}

//---------------------------------------------------------------------------
// Suite 2: the two kinds of refusal
//
// The costs are the point. A geometry refusal is free; a state refusal is one
// access. If those ever swap, the driver has either started guessing state or
// stopped trusting CAPS.
//---------------------------------------------------------------------------

void suite_refusals(bcmc_dev_t* dev, Meter* m) {
    uint32_t v = 0;
    bool     b = false;
    const uint32_t max_c = dev->max_c;
    const uint32_t words = bcmc_column_words(dev);
    std::vector<uint32_t> buf(words + 1, 0);

    // Geometry: known from CAPS, so refused for free.
    CALL(*m, BCMC_ERANGE, 0, bcmc_set_c(dev, max_c + 1));
    CALL(*m, BCMC_ERANGE, 0, bcmc_write_weight(dev, max_c, 1));
    CALL(*m, BCMC_ERANGE, 0, bcmc_read_weight(dev, max_c, &v));
    CALL(*m, BCMC_ERANGE, 0, bcmc_read_offset(dev, max_c, &v));
    CALL(*m, BCMC_ERANGE, 0, bcmc_read_column(dev, 0, buf.data(), words - 1));

    // C == MAX_C is the boundary and is legal: one access, not a refusal.
    CALL(*m, BCMC_OK, 1, bcmc_set_c(dev, max_c));

    // Null arguments: also free, and also not the peripheral's business.
    CALL(*m, BCMC_EINVAL, 0, bcmc_read_offset(dev, 0, NULL));
    CALL(*m, BCMC_EINVAL, 0, bcmc_read_cell(dev, 0, 0, NULL));
    CALL(*m, BCMC_EINVAL, 0, bcmc_read_column(dev, 0, NULL, words));
    CALL(*m, BCMC_EINVAL, 0, bcmc_busy(dev, NULL));
    CALL(*m, BCMC_EINVAL, 0, bcmc_done(dev, NULL));
    CALL(*m, BCMC_EINVAL, 0, bcmc_load(dev, NULL, 8, 3));

    // State: only the peripheral knows, so the driver asks and is refused.
    // Writing C above cleared VALID, so no matrix exists right now.
    CALL(*m, BCMC_OK, 1, bcmc_done(dev, &b));
    check(!b, "FAIL: refusals: VALID set although the context was just written\n");

    CALL(*m, BCMC_EREFUSED, 1, bcmc_read_offset(dev, 0, &v));
    CALL(*m, BCMC_EREFUSED, 3, bcmc_read_cell(dev, 0, 0, &b));
    CALL(*m, BCMC_EREFUSED, 2, bcmc_read_column(dev, 0, buf.data(), words));

    // BUSY is not knowable either, and bcmc_busy() is one access like the rest.
    CALL(*m, BCMC_OK, 1, bcmc_busy(dev, &b));
    check(!b, "FAIL: refusals: BUSY set with no transform running\n");
}

//---------------------------------------------------------------------------
// Suite 3: what each call costs
//
// sw/bcmc.h's primitive/composition split, written down as numbers. Nothing
// here inspects a value; this suite is entirely about traffic.
//---------------------------------------------------------------------------

void suite_costs(bcmc_dev_t* dev, Meter* m) {
    uint32_t v = 0;
    bool     b = false;
    const uint32_t words = bcmc_column_words(dev);
    std::vector<uint32_t> buf(words, 0);

    // Primitives: one access each.
    CALL(*m, BCMC_OK, 1, bcmc_set_n(dev, 8));
    CALL(*m, BCMC_OK, 1, bcmc_set_c(dev, 3));
    CALL(*m, BCMC_OK, 1, bcmc_write_weight(dev, 0, 6));
    CALL(*m, BCMC_OK, 1, bcmc_read_weight(dev, 0, &v));
    CALL(*m, BCMC_OK, 1, bcmc_set_cell_row(dev, 1));
    CALL(*m, BCMC_OK, 1, bcmc_set_cell_col(dev, 2));
    CALL(*m, BCMC_OK, 1, bcmc_read_status(dev, &v));
    CALL(*m, BCMC_OK, 1, bcmc_irq_enable(dev, false));
    CALL(*m, BCMC_OK, 1, bcmc_irq_clear(dev));
    CALL(*m, BCMC_OK, 1, bcmc_read_reg(dev, BCMC_REG_ID, &v));
    CALL(*m, BCMC_OK, 1, bcmc_write_reg(dev, BCMC_REG_CELL_COL, 0));

    check(v == BCMC_ID_VALUE,
          "FAIL: costs: the escape hatch read ID as %08X\n", v);

    // The stated exception: START cannot be written without deciding IRQ_EN,
    // so bcmc_start() reads CTRL first. Two accesses, and the header says so.
    CALL(*m, BCMC_OK, 2, bcmc_start(dev));

    // One access each, and each is one bcmc_read_status().
    CALL(*m, BCMC_OK, 1, bcmc_busy(dev, &b));
    CALL(*m, BCMC_OK, 1, bcmc_done(dev, &b));
    CALL(*m, BCMC_OK, 1, bcmc_irq_pending(dev, &b));

    // The transform above is still running or has just finished; either way
    // waiting is legal and ends in VALID.
    m->begin();
    bcmc_status_t s = bcmc_wait(dev, 3u + 4u + BCMC_POLL_MARGIN);
    m->expect(s, BCMC_OK, -1, "bcmc_wait after bcmc_start");

    // Compositions: exactly their parts, and nothing hidden.
    CALL(*m, BCMC_OK, 3, bcmc_read_cell(dev, 0, 0, &b));
    CALL(*m, BCMC_OK, 1 + (long long)words, bcmc_read_column(dev, 0, buf.data(), words));

    // A buffer sized for a wider peripheral than this one. The words past the
    // column must come back cleared, not left as they were, or a caller who
    // sized for MAX_C = 128 and attached to a MAX_C = 64 part would read the
    // column plus whatever happened to be in memory -- and every one of those
    // stale bits would look like an occupied row. The cost is unchanged: the
    // clearing is memory, not traffic.
    std::vector<uint32_t> over(words + 2, 0xFFFFFFFFu);
    CALL(*m, BCMC_OK, 1 + (long long)words,
         bcmc_read_column(dev, 0, over.data(), words + 2));
    for (uint32_t k = words; k < words + 2; k++) {
        check(over[k] == 0,
              "FAIL: costs: word %u past the column read %08X; it must be "
              "cleared\n",
              k, over[k]);
    }

    // A read-only window really is read-only, and the driver reports it as a
    // refusal rather than pretending the write happened.
    CALL(*m, BCMC_EREFUSED, 1, bcmc_write_reg(dev, BCMC_OFFSET_BASE, 1));
    CALL(*m, BCMC_EREFUSED, 1, bcmc_write_reg(dev, BCMC_REG_ID, 0));

    // The poll bound is honoured. With C at its maximum the transform needs
    // MAX_C + 4 clocks and a single poll is three, so one poll cannot see
    // VALID -- which is exactly the case bcmc_wait() must not loop forever on.
    CALL(*m, BCMC_OK, 1, bcmc_set_c(dev, dev->max_c));
    CALL(*m, BCMC_OK, 1, bcmc_set_n(dev, 64));
    CALL(*m, BCMC_OK, 2, bcmc_start(dev));

    // The only moment in this file when a transform is definitely still
    // running, and so the only moment BUSY can be seen set. Worth taking:
    // without it, a bcmc_busy() that reported VALID instead would pass every
    // other check here, since outside this window the two agree on zero.
    CALL(*m, BCMC_OK, 1, bcmc_busy(dev, &b));
    check(b, "FAIL: costs: BUSY clear during a transform of C = MAX_C rows\n");
    CALL(*m, BCMC_OK, 1, bcmc_done(dev, &b));
    check(!b, "FAIL: costs: VALID set while the transform is still running\n");

    CALL(*m, BCMC_ENOTREADY, 1, bcmc_wait(dev, 1));
    // ... and the transform is still perfectly capable of finishing.
    m->begin();
    s = bcmc_wait(dev, dev->max_c + 4u + BCMC_POLL_MARGIN);
    m->expect(s, BCMC_OK, -1, "bcmc_wait after giving up once");
}

//---------------------------------------------------------------------------
// Suite 4: the interrupt
//
// Two properties from docs/Register_Map.md, neither of which the driver may
// get wrong quietly:
//
//   the pin is IRQ & IRQ_EN, and IRQ latches whether or not IRQ_EN is set
//   starting a transform does not disturb IRQ_EN
//
// The second is why bcmc_start() costs two accesses, so it is checked here by
// reading CTRL back rather than by trusting the comment.
//---------------------------------------------------------------------------

void suite_irq(bcmc_dev_t* dev, Meter* m, Vbcmc_wb* dut) {
    uint32_t ctrl = 0;
    bool     pending = false;
    const uint32_t weights[2] = {3, 2};

    CALL(*m, BCMC_OK, 1, bcmc_irq_clear(dev));
    CALL(*m, BCMC_OK, 1, bcmc_irq_enable(dev, true));

    m->begin();
    bcmc_status_t s = bcmc_load(dev, weights, 8, 2);
    m->expect(s, BCMC_OK, -1, "bcmc_load with interrupts enabled");

    CALL(*m, BCMC_OK, 1, bcmc_irq_pending(dev, &pending));
    check(pending, "FAIL: irq: no interrupt latched after a transform\n");
    check(dut->irq_o != 0,
          "FAIL: irq: IRQ and IRQ_EN are both set but the pin is low\n");

    // The claim bcmc_start() exists to make true.
    CALL(*m, BCMC_OK, 1, bcmc_read_reg(dev, BCMC_REG_CTRL, &ctrl));
    check((ctrl & BCMC_CTRL_IRQ_EN) != 0,
          "FAIL: irq: bcmc_start() cleared IRQ_EN behind the caller's back\n");
    check((ctrl & BCMC_CTRL_START) == 0,
          "FAIL: irq: START read back as 1; it is a command, not a state\n");

    CALL(*m, BCMC_OK, 1, bcmc_irq_clear(dev));
    CALL(*m, BCMC_OK, 1, bcmc_irq_pending(dev, &pending));
    check(!pending, "FAIL: irq: write-1-to-clear did not clear IRQ\n");
    check(dut->irq_o == 0, "FAIL: irq: the pin stayed high after IRQ was cleared\n");

    // Disabled: the pin is gated, the latch is not.
    CALL(*m, BCMC_OK, 1, bcmc_irq_enable(dev, false));
    m->begin();
    s = bcmc_load(dev, weights, 8, 2);
    m->expect(s, BCMC_OK, -1, "bcmc_load with interrupts disabled");

    CALL(*m, BCMC_OK, 1, bcmc_irq_pending(dev, &pending));
    check(pending, "FAIL: irq: IRQ_EN gated the latch as well as the pin\n");
    check(dut->irq_o == 0, "FAIL: irq: the pin rose although IRQ_EN was clear\n");
    CALL(*m, BCMC_OK, 1, bcmc_irq_clear(dev));
}

//---------------------------------------------------------------------------
// Suite 5: the matrix, as reference.py computed it
//
// One MatrixCase per call. Everything compared here came out of
// validation/reference.py; nothing is computed locally except the popcount of
// a column, which is arithmetic on what was read rather than a prediction of
// what should have been read.
//---------------------------------------------------------------------------

void suite_matrix(bcmc_dev_t* dev, Meter* m, const bcmc::MatrixCase& c,
                  const char* path) {
    const uint32_t max_c = dev->max_c;
    const uint32_t words = bcmc_column_words(dev);
    std::vector<uint32_t> col(words, 0);

    // bcmc_load() is the programming sequence: C, N, C weights, START, and at
    // least one poll. The exact poll count is data-dependent, so the cost of
    // everything before the poll is what gets pinned down.
    m->begin();
    bcmc_status_t s = bcmc_load(dev, c.weights.data(), c.N, c.C);
    const uint64_t spent = m->cost();
    check(s == BCMC_OK, "FAIL: %s:%d: bcmc_load returned %s\n", path, c.line,
          bcmc_strstatus(s));
    const uint64_t least = 2u + static_cast<uint64_t>(c.C) + 2u + 1u;
    check(spent >= least,
          "FAIL: %s:%d: bcmc_load spent %llu accesses, fewer than the "
          "C + 5 the sequence requires\n",
          path, c.line, (unsigned long long)spent);

    // The offsets: the Core's output, compared with reference.py's.
    for (uint32_t i = 0; i < c.C; i++) {
        uint32_t got = 0;
        CALL(*m, BCMC_OK, 1, bcmc_read_offset(dev, i, &got));
        check(got == c.offsets[i],
              "FAIL: %s:%d: offset[%u] = %u, reference.py says %u\n", path,
              c.line, i, got, c.offsets[i]);
    }

    // Above C the window is defined to be zero, so a stale offset from an
    // earlier, larger C cannot hide there.
    for (uint32_t i = c.C; i < max_c; i++) {
        uint32_t got = 0;
        CALL(*m, BCMC_OK, 1, bcmc_read_offset(dev, i, &got));
        check(got == 0,
              "FAIL: %s:%d: offset[%u] = %u above C = %u; it must read 0\n",
              path, c.line, i, got, c.C);
    }

    // The weights read back as written: the register map offers this so a
    // driver can verify itself, so the harness verifies that it can.
    for (uint32_t i = 0; i < c.C; i++) {
        uint32_t got = 0;
        CALL(*m, BCMC_OK, 1, bcmc_read_weight(dev, i, &got));
        check(got == c.weights[i],
              "FAIL: %s:%d: weight[%u] read back as %u, wrote %u\n", path,
              c.line, i, got, c.weights[i]);
    }

    // Every bit of the matrix, one column at a time. A column read costs one
    // index write plus one word per 32 rows, so this covers all C*N bits in
    // N * (1 + words) accesses rather than 3*C*N.
    for (uint32_t j = 0; j < c.N; j++) {
        CALL(*m, BCMC_OK, 1 + (long long)words,
             bcmc_read_column(dev, j, col.data(), words));

        uint32_t seen = 0;
        for (uint32_t r = 0; r < max_c; r++) {
            const uint32_t bit = (col[r / 32] >> (r % 32)) & 1u;
            if (r < c.C) {
                const uint32_t want = c.rows[r][j];
                check(bit == want,
                      "FAIL: %s:%d: COLUMN %u bit %u = %u, reference.py says "
                      "%u\n",
                      path, c.line, j, r, bit, want);
                seen += bit;
            } else {
                check(bit == 0,
                      "FAIL: %s:%d: COLUMN %u bit %u = 1 above C = %u; rows "
                      "that do not exist answer 0\n",
                      path, c.line, j, r, c.C);
            }
        }

        // The occupancy the Balance Theorem constrains, seen through the
        // driver. reference.py recorded it; this is the same number counted
        // from what the hardware just returned.
        check(seen == c.occupancy[j],
              "FAIL: %s:%d: column %u holds %u ones, reference.py says %u\n",
              path, c.line, j, seen, c.occupancy[j]);
    }

    // Cells. The projection must agree with the column it is a slice of, and
    // a row at or above C must be an inactive lane rather than an error. Rows
    // are sampled because a cell read is three accesses; columns above already
    // covered every bit.
    uint32_t probe_rows[4];
    uint32_t nrows = 0;
    if (c.C > 0) {
        probe_rows[nrows++] = 0;
        probe_rows[nrows++] = c.C / 2;
        probe_rows[nrows++] = c.C - 1;
    }
    if (c.C < max_c) probe_rows[nrows++] = c.C;

    for (uint32_t j = 0; j < c.N; j++) {
        for (uint32_t k = 0; k < nrows; k++) {
            const uint32_t i = probe_rows[k];
            bool           bit = false;
            CALL(*m, BCMC_OK, 3, bcmc_read_cell(dev, i, j, &bit));
            const uint32_t want = (i < c.C) ? c.rows[i][j] : 0u;
            check(bit == (want != 0),
                  "FAIL: %s:%d: CELL(%u,%u) = %u, expected %u%s\n", path,
                  c.line, i, j, bit ? 1u : 0u, want,
                  (i >= c.C) ? " (an inactive lane answers 0)" : "");
        }
    }
}

//---------------------------------------------------------------------------
// Suite 6: C = 0
//
// Legal, and the one transform that consumes no weight at all. It is checked
// separately because no vector file contains it: reference.py's matrices all
// have at least one row.
//---------------------------------------------------------------------------

void suite_empty(bcmc_dev_t* dev, Meter* m) {
    const uint32_t words = bcmc_column_words(dev);
    std::vector<uint32_t> col(words, 0);
    bool bit = false;

    // No weights, so a null vector is legal: C, N, START, at least one poll.
    m->begin();
    bcmc_status_t s = bcmc_load(dev, NULL, 8, 0);
    const uint64_t spent = m->cost();
    m->expect(s, BCMC_OK, -1, "bcmc_load with C = 0");
    check(spent >= 4, "FAIL: empty: bcmc_load(C=0) spent only %llu accesses\n",
          (unsigned long long)spent);

    // A matrix exists -- it simply has no rows -- so reads are answered, and
    // every answer is zero.
    for (uint32_t j = 0; j < 8; j++) {
        CALL(*m, BCMC_OK, 1 + (long long)words,
             bcmc_read_column(dev, j, col.data(), words));
        for (uint32_t k = 0; k < words; k++) {
            check(col[k] == 0, "FAIL: empty: column %u word %u = %08X\n", j, k,
                  col[k]);
        }
        CALL(*m, BCMC_OK, 3, bcmc_read_cell(dev, 0, j, &bit));
        check(!bit, "FAIL: empty: CELL(0,%u) = 1 with C = 0\n", j);
    }

    // And every offset in the window was cleared.
    for (uint32_t i = 0; i < dev->max_c; i++) {
        uint32_t got = 0;
        CALL(*m, BCMC_OK, 1, bcmc_read_offset(dev, i, &got));
        check(got == 0, "FAIL: empty: offset[%u] = %u with C = 0\n", i, got);
    }
}

}  // namespace

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);

    uint64_t                 limit = 0;
    std::string              vcd;
    std::vector<std::string> files;

    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        if (arg == "--limit" && i + 1 < argc) {
            limit = std::strtoull(argv[++i], nullptr, 10);
        } else if (arg == "--vcd" && i + 1 < argc) {
            vcd = argv[++i];
        } else if (arg.rfind("--", 0) == 0) {
            continue;  // Verilated::commandArgs took it
        } else {
            files.push_back(arg);
        }
    }

    if (files.empty()) {
        std::printf("usage: bcmc_driver_test [--limit K] [--vcd PATH] "
                    "<matrix_vectors.txt>...\n");
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

    // Base zero, because there is no bridge here: bcmc_wb decodes a 4 KiB
    // region and the harness is wired straight to it. Where the region sits in
    // a system's address space is the bridge's business, and the only thing the
    // driver does with `base` is add it.
    Meter meter(&bus, &plat);

    suite_probe(&dev, &meter);
    // Uses the geometry the real peripheral just reported, so that the stub
    // differs from it in exactly one register at a time.
    suite_foreign(dev.version,
                  ((uint32_t)BCMC_DRIVER_IDX_W << 24) |
                      ((uint32_t)BCMC_DRIVER_VAL_W << 16) |
                      (uint32_t)BCMC_DRIVER_MAX_C);
    suite_refusals(&dev, &meter);
    suite_costs(&dev, &meter);
    suite_irq(&dev, &meter, &dut);
    suite_empty(&dev, &meter);

    uint64_t cases = 0;
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
            // check, and skipping it silently is better than failing on a
            // geometry the vector file never claimed to match.
            if (c.C > dev.max_c) continue;
            suite_matrix(&dev, &meter, c, path.c_str());
            cases++;
        }
        if (limit != 0 && cases >= limit) break;
    }

    if (cases == 0) fail("FAIL: no usable matrix cases were replayed\n");

    // Nothing above tolerated one of these, but say so explicitly: a run that
    // never provoked a protocol violation is a stronger statement than a run
    // that merely did not check for one.
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

    std::printf("bcmc_driver_test: PASS  %llu cases, %llu checks, "
                "%llu accesses (%llu refused), %llu clocks\n",
                (unsigned long long)cases, (unsigned long long)g_checks,
                (unsigned long long)bus.accesses(),
                (unsigned long long)plat.errs, (unsigned long long)bus.ticks());
    return 0;
}

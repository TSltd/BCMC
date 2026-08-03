//===========================================================================
// bcmc.h -- the BCMC peripheral, as seen from C
//
// docs/Register_Map.md says what every address means. This header says how a
// program reaches those addresses, and it adds nothing else. There is no
// protocol here, no queue, no state machine and no cached copy of the
// peripheral's state, because the register map has none of those things and a
// driver that invented one would be describing a different device.
//
// PRIMITIVES, THEN COMPOSITION
//
// The shape of this file is the shape of the RTL. bcmc_cell.v is the
// mathematics; bcmc_row.v and bcmc_column.v are replication and contain no new
// mathematics. Here, likewise:
//
//   * a PRIMITIVE is exactly one bus access -- one address, one direction.
//     It converts a register into a C name and does nothing further.
//
//   * a COMPOSITION is a sequence of primitives. It contains no arithmetic on
//     BCMC quantities, no bit twiddling beyond the field masks its primitives
//     already define, and no access that a primitive does not provide.
//
// bcmc_load() is the whole point of the distinction: it is the programming
// sequence of docs/Register_Map.md written out in primitives, so that anyone
// can read it and check that it is that sequence and nothing more.
//
// THE MATHEMATICS IS NOT IN HERE
//
// This driver never computes an offset, never evaluates M(i,j), and never
// decides which cell to look at. The first belongs to bcmc_core.v, the second
// to bcmc_cell.v, and the third to an observer (v0.5). A driver that computed
// any of them would be a second implementation of the mathematics, competing
// with validation/reference.py for the right to be correct.
//
// GEOMETRY IS DISCOVERED; STATE IS NOT GUESSED
//
// Two kinds of precondition exist, and the driver treats them differently.
//
//   Geometry -- MAX_C, VAL_W, IDX_W -- is fixed at synthesis and readable from
//   CAPS. bcmc_probe() reads it once, so afterwards the driver KNOWS it, and
//   checking an index against it locally is using knowledge rather than
//   assuming it. Those checks return BCMC_ERANGE and cost no bus access.
//
//   State -- BUSY and VALID -- changes under the driver's feet. It cannot be
//   known without an access, so the driver does not pre-check it. It issues
//   the access and reports the refusal. docs/Register_Map.md already specifies
//   exactly which accesses are denied in which state (E4), and duplicating
//   that rule here would double the traffic and create a second, staler
//   authority on it.
//
// The mathematical preconditions -- N >= 1, 0 <= weight[i] <= N, j < N -- are
// checked by nobody, here as in the hardware, for the reason given in
// docs/Register_Map.md: checking them would be new mathematics in a place that
// is not allowed to have any.
//
// NO PLATFORM, NO ALLOCATOR, NO CLOCK
//
// C99, freestanding: <stdbool.h>, <stddef.h> and <stdint.h>, all three of
// which a freestanding implementation is required to provide. No malloc, no
// OS calls, no floating point, no <time.h>. The bus is reached through two
// function pointers, so the same object file serves a bare-metal MMIO target and the
// Verilator harness in sim/bcmc_driver_test.cpp -- which is what makes it
// possible to test this driver against rtl/bcmc_wb.v rather than against a
// mock of it.
//
// There is no timer, so no timeout is expressed in seconds. See bcmc_wait().
//
//===========================================================================

#ifndef BCMC_SW_BCMC_H
#define BCMC_SW_BCMC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//---------------------------------------------------------------------------
// The register map, transcribed
//
// Byte offsets from the peripheral's base address, exactly as tabulated in
// docs/Register_Map.md. They are in the header rather than hidden in the .c
// because a register map is public: an observer that wants a raw access should
// be able to name the address instead of inventing a number.
//---------------------------------------------------------------------------

#define BCMC_REG_ID       0x000u  // RO  0x42434D43
#define BCMC_REG_VERSION  0x004u  // RO  MAJOR:MINOR:PATCH
#define BCMC_REG_CAPS     0x008u  // RO  IDX_W:VAL_W:MAX_C
#define BCMC_REG_CTRL     0x00Cu  // RW  START, IRQ_EN
#define BCMC_REG_STATUS   0x010u  // RW  BUSY, VALID, IRQ (RW1C)
#define BCMC_REG_N        0x014u  // RW  row length
#define BCMC_REG_C        0x018u  // RW  number of rows
#define BCMC_REG_CELL_ROW 0x01Cu  // RW  row index for the next CELL read
#define BCMC_REG_CELL_COL 0x020u  // RW  column index for CELL and COLUMN
#define BCMC_REG_CELL     0x024u  // RO  bit 0 = M(CELL_ROW, CELL_COL)
#define BCMC_COLUMN_BASE  0x028u  // RO  COLUMN[k] at +4k
#define BCMC_WEIGHT_BASE  0x400u  // RW  WEIGHT[i] at +4i
#define BCMC_OFFSET_BASE  0x800u  // RO  OFFSET[i] at +4i

#define BCMC_ID_VALUE 0x42434D43u  // the ASCII bytes "BCMC"

// CTRL
#define BCMC_CTRL_START  0x1u  // W1S: writing 1 begins a transform, reads 0
#define BCMC_CTRL_IRQ_EN 0x2u  // RW:  gates the interrupt pin only

// STATUS
#define BCMC_STATUS_BUSY  0x1u  // RO
#define BCMC_STATUS_VALID 0x2u  // RO
#define BCMC_STATUS_IRQ   0x4u  // RW1C

// CAPS
#define BCMC_CAPS_MAX_C(caps) ((uint32_t)((caps) & 0xFFFFu))
#define BCMC_CAPS_VAL_W(caps) ((uint8_t)(((caps) >> 16) & 0xFFu))
#define BCMC_CAPS_IDX_W(caps) ((uint8_t)(((caps) >> 24) & 0xFFu))

// VERSION
#define BCMC_VERSION_MAJOR(v) ((uint16_t)(((v) >> 16) & 0xFFFFu))
#define BCMC_VERSION_MINOR(v) ((uint8_t)(((v) >> 8) & 0xFFu))
#define BCMC_VERSION_PATCH(v) ((uint8_t)((v) & 0xFFu))

// The map this driver was written against. MAJOR is the compatibility
// promise, so it is the only component bcmc_probe() insists on: a larger MINOR
// means registers were added, which cannot break code that ignores them.
#define BCMC_DRIVER_MAJOR 0u

//---------------------------------------------------------------------------
// Results
//
// Every function that can fail returns bcmc_status_t, and the failures are
// distinguished because they mean different things to the caller. In
// particular BCMC_EREFUSED is not a driver bug: docs/Register_Map.md makes the
// refused access half the specification ("Silent success hides bugs"), so a
// driver that collapsed it into a generic error would be discarding the
// peripheral's most informative answer.
//---------------------------------------------------------------------------

typedef enum {
    BCMC_OK = 0,      // the access completed; any out-parameter is written
    BCMC_EREFUSED,    // the bus answered err: one of E1..E4
    BCMC_ERANGE,      // an index outside the geometry CAPS reported
    BCMC_EINVAL,      // a null pointer, or a device that was never attached
    BCMC_ENODEV,      // ID did not read BCMC_ID_VALUE
    BCMC_EVERSION,    // VERSION.MAJOR is not BCMC_DRIVER_MAJOR
    BCMC_ENOTREADY    // still BUSY after a bound that proves it should not be
} bcmc_status_t;

//---------------------------------------------------------------------------
// The bus
//
// Two accessors, returning zero on success and non-zero if the bus signalled
// err. The err path must be reportable, because on this peripheral it is a
// specified outcome rather than a fault -- and because a harness that could
// not report it could not test the refusals at all.
//
// On a CPU where a Wishbone err becomes a synchronous bus fault, an accessor
// may of course never return. That is the platform's business and not this
// header's; the interface merely leaves room for platforms where it is not.
//
// `ctx` is passed through untouched. The driver never dereferences it, never
// copies what it points at, and never frees it.
//---------------------------------------------------------------------------

typedef int (*bcmc_read_fn)(void *ctx, uint32_t addr, uint32_t *data);
typedef int (*bcmc_write_fn)(void *ctx, uint32_t addr, uint32_t data);

// The whole of the driver's state, and note what is absent: no copy of N, no
// copy of C, no shadow of any writable register, no "last known status". Those
// live in the peripheral, which is the only thing that can be right about
// them. What is here is the connection, plus the geometry that cannot change
// while the design is running.
typedef struct {
    uint32_t      base;   // byte address of BCMC_REG_ID
    bcmc_read_fn  read;   // never null after bcmc_attach()
    bcmc_write_fn write;  // never null after bcmc_attach()
    void         *ctx;    // opaque, handed back to the accessors

    bool     probed;  // bcmc_probe() has succeeded; the three below are valid
    uint32_t max_c;   // CAPS.MAX_C -- rows this instance can hold
    uint8_t  val_w;   // CAPS.VAL_W -- bits in N, a weight, an offset
    uint8_t  idx_w;   // CAPS.IDX_W -- bits in C and in a row index
    uint32_t version; // VERSION, as read
} bcmc_dev_t;

//---------------------------------------------------------------------------
// Attach and discover
//---------------------------------------------------------------------------

// Fills `dev`. Performs no bus access whatsoever, so it cannot fail for any
// reason other than a null argument, and it is safe before the peripheral is
// out of reset.
bcmc_status_t bcmc_attach(bcmc_dev_t *dev, uint32_t base, bcmc_read_fn read,
                          bcmc_write_fn write, void *ctx);

// Reads ID, VERSION and CAPS -- three accesses -- and records the geometry.
// This is step 1 and step 2 of the programming sequence, and it is mandatory:
// every function that takes an index refuses with BCMC_EINVAL until it has
// run, because an index check against an assumed MAX_C would be exactly the
// assumption docs/Register_Map.md forbids.
bcmc_status_t bcmc_probe(bcmc_dev_t *dev);

// Words in one COLUMN read: ceil(MAX_C / 32). Answered from the probed
// geometry, with no bus access. Zero if `dev` is null or unprobed.
uint32_t bcmc_column_words(const bcmc_dev_t *dev);

//---------------------------------------------------------------------------
// Primitives -- one bus access each, with one stated exception
//
// The exception is bcmc_start(), which costs two. CTRL puts a command bit
// beside a mode bit and rtl/bcmc_wb.v latches IRQ_EN from every CTRL write, so
// "write START" cannot be expressed without also deciding IRQ_EN. It is listed
// here rather than among the compositions because it is still one operation on
// one register: the extra access recovers a bit rather than doing anything.
// See the comment above bcmc_start() in bcmc.c.
//---------------------------------------------------------------------------

// The escape hatch, and the two calls every other primitive is built from. An
// observer that wants a register this header has not named should use these
// with a BCMC_REG_* offset rather than reach past the driver.
bcmc_status_t bcmc_read_reg(bcmc_dev_t *dev, uint32_t offset, uint32_t *value);
bcmc_status_t bcmc_write_reg(bcmc_dev_t *dev, uint32_t offset, uint32_t value);

// Context: writing any of these clears VALID, and all are denied while BUSY.
bcmc_status_t bcmc_set_n(bcmc_dev_t *dev, uint32_t n);
bcmc_status_t bcmc_set_c(bcmc_dev_t *dev, uint32_t c);
bcmc_status_t bcmc_write_weight(bcmc_dev_t *dev, uint32_t i, uint32_t weight);

// Weights read back so that a driver can verify what it wrote; offsets read
// back because they are the other half of the canonical prefix
// representation. Neither is computed here.
bcmc_status_t bcmc_read_weight(bcmc_dev_t *dev, uint32_t i, uint32_t *weight);
bcmc_status_t bcmc_read_offset(bcmc_dev_t *dev, uint32_t i, uint32_t *offset);

// Query indices. Part of the query, not of the context: writing them does not
// clear VALID and is permitted while BUSY.
bcmc_status_t bcmc_set_cell_row(bcmc_dev_t *dev, uint32_t row);
bcmc_status_t bcmc_set_cell_col(bcmc_dev_t *dev, uint32_t col);

// The raw STATUS word. bcmc_busy(), bcmc_done() and bcmc_irq_pending() are
// this call plus a mask, which is why they cannot disagree with each other:
// one access yields all three bits, so there is no window between them.
bcmc_status_t bcmc_read_status(bcmc_dev_t *dev, uint32_t *status);

// Begins a transform on the currently programmed N, C and weights. Denied
// while BUSY. START is a command with no readable value, so there is nothing
// to clear afterwards.
bcmc_status_t bcmc_start(bcmc_dev_t *dev);

// The interrupt. IRQ latches regardless of IRQ_EN, so a polling driver and an
// interrupt-driven driver see the same STATUS; IRQ_EN gates the pin only.
bcmc_status_t bcmc_irq_enable(bcmc_dev_t *dev, bool enable);
bcmc_status_t bcmc_irq_clear(bcmc_dev_t *dev);

//---------------------------------------------------------------------------
// Compositions
//---------------------------------------------------------------------------

// One access each, via bcmc_read_status().
bcmc_status_t bcmc_busy(bcmc_dev_t *dev, bool *busy);
bcmc_status_t bcmc_irq_pending(bcmc_dev_t *dev, bool *pending);

// THERE IS NO DONE BIT, and its absence is deliberate -- see "There is
// deliberately no DONE bit" in docs/Register_Map.md. `done` here means
// STATUS.VALID: a BCMC matrix exists for the programmed weights. That is the
// bit step 7 of the programming sequence waits on, and it is the bit the
// access rules use to decide whether a CELL, COLUMN or OFFSET read is
// answered. It is emphatically not !BUSY, which is also true after reset when
// no matrix exists at all.
bcmc_status_t bcmc_done(bcmc_dev_t *dev, bool *done);

// Polls until done, at most `max_polls` times, then gives up with
// BCMC_ENOTREADY. `max_polls == 0` polls without limit.
//
// A POLL COUNT IS A SOUND BOUND, AND A MICROSECOND COUNT WOULD NOT BE
//
// The transform takes C + 4 clocks of the peripheral. A poll is a bus access,
// and a bus access cannot take less than one clock of the peripheral. So after
// C + 4 polls the transform has certainly finished -- on the fastest CPU that
// could ever be attached, let alone a slow one. A driver that still sees BUSY
// is looking at broken hardware, not at slow hardware, which is precisely what
// a timeout ought to distinguish and what a wall-clock timeout on an unknown
// CPU cannot. It also needs no timer, which is why this driver needs no
// platform.
bcmc_status_t bcmc_wait(bcmc_dev_t *dev, uint32_t max_polls);

// Slack added to C + 4 by bcmc_load(). The bound above is already sound; this
// only keeps it from being tight enough to be brittle if the pipeline ever
// gains a cycle.
#define BCMC_POLL_MARGIN 16u

// Reads M(row, col) into *bit. Three accesses: CELL_ROW, CELL_COL, CELL.
//
// The register map splits a query into two index writes and a data read, so
// this is already a composition rather than a primitive; the index writes are
// exposed above so that nothing is hidden and so that an observer sweeping one
// row can write CELL_ROW once instead of once per cell.
bcmc_status_t bcmc_read_cell(bcmc_dev_t *dev, uint32_t row, uint32_t col, bool *bit);

// Reads column `col` into `words[0 .. nwords-1]`, row r in bit r % 32 of word
// r / 32. `nwords` must be at least bcmc_column_words(dev); a smaller buffer
// is BCMC_ERANGE rather than a partial answer, because a partly-filled column
// is indistinguishable from a column of zeros above C.
bcmc_status_t bcmc_read_column(bcmc_dev_t *dev, uint32_t col, uint32_t *words,
                               uint32_t nwords);

// The programming sequence, steps 3 to 7, and nothing else:
//
//     bcmc_set_c(dev, c)
//     bcmc_set_n(dev, n)
//     bcmc_write_weight(dev, i, weights[i])   for i in 0 .. c-1
//     bcmc_start(dev)
//     bcmc_wait(dev, c + 4 + BCMC_POLL_MARGIN)
//
// That listing is the implementation, not a summary of it. bcmc_load()
// contains no arithmetic on a BCMC quantity, no access its primitives do not
// provide, and no policy beyond the order -- which is the order the register
// map gives, with C first only so that the poll bound is known before it is
// needed. Weights at or above c are not written: rows there are inactive
// lanes whatever the window holds, so writing them would be harmless and
// pointless.
//
// `weights` may be null only if `c` is zero, which is a legal transform: it
// completes immediately and every CELL and COLUMN read then answers 0.
bcmc_status_t bcmc_load(bcmc_dev_t *dev, const uint32_t *weights, uint32_t n, uint32_t c);

// A human-readable name for a status, for harnesses and for logging. Never
// null, never allocates.
const char *bcmc_strstatus(bcmc_status_t status);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // BCMC_SW_BCMC_H

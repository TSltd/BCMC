//===========================================================================
// bcmc.c -- the BCMC peripheral driver
//
// Read this file next to docs/Register_Map.md, because it is a transcription
// of it and nothing else. Every function below is either
//
//   * one bus access with a name, or
//   * a sequence of such calls,
//
// and the second kind is short enough to check by eye. If a function here
// starts to look interesting, that is a bug: interesting means it has acquired
// mathematics, policy or state that belongs somewhere else.
//
// WHAT THIS FILE IS NOT ALLOWED TO CONTAIN
//
//   no offset arithmetic         bcmc_core.v computes offsets
//   no evaluation of M(i,j)      bcmc_cell.v evaluates cells
//   no traversal order           observers choose what to look at (v0.5)
//   no shadow registers          the peripheral owns its own state
//   no retry, no backoff         a refusal is an answer, not a setback
//
// The one arithmetic operation that is legitimate here is turning an index
// into an address -- base + WINDOW + 4*i -- because that is what a register
// map is, and turning a poll count into a loop bound, because that is what a
// bound is. Neither is a BCMC quantity.
//===========================================================================

#include "bcmc.h"

//---------------------------------------------------------------------------
// The two calls everything else is made of
//
// Neither requires a probed device, because bcmc_probe() itself is built from
// them. Nor does either check alignment: every address this driver forms is
// base + 4k by construction, and an unaligned offset handed to the escape
// hatch is error E1, which the peripheral already detects and reports better
// than a duplicate check here could.
//---------------------------------------------------------------------------

bcmc_status_t bcmc_read_reg(bcmc_dev_t *dev, uint32_t offset, uint32_t *value)
{
    if (dev == NULL || dev->read == NULL || value == NULL) {
        return BCMC_EINVAL;
    }
    if (dev->read(dev->ctx, dev->base + offset, value) != 0) {
        return BCMC_EREFUSED;
    }
    return BCMC_OK;
}

bcmc_status_t bcmc_write_reg(bcmc_dev_t *dev, uint32_t offset, uint32_t value)
{
    if (dev == NULL || dev->write == NULL) {
        return BCMC_EINVAL;
    }
    if (dev->write(dev->ctx, dev->base + offset, value) != 0) {
        return BCMC_EREFUSED;
    }
    return BCMC_OK;
}

//---------------------------------------------------------------------------
// Attach and discover
//---------------------------------------------------------------------------

bcmc_status_t bcmc_attach(bcmc_dev_t *dev, uint32_t base, bcmc_read_fn read,
                          bcmc_write_fn write, void *ctx)
{
    if (dev == NULL || read == NULL || write == NULL) {
        return BCMC_EINVAL;
    }

    dev->base    = base;
    dev->read    = read;
    dev->write   = write;
    dev->ctx     = ctx;
    dev->probed  = false;
    dev->max_c   = 0u;
    dev->val_w   = 0u;
    dev->idx_w   = 0u;
    dev->version = 0u;
    return BCMC_OK;
}

bcmc_status_t bcmc_probe(bcmc_dev_t *dev)
{
    bcmc_status_t s;
    uint32_t      id;
    uint32_t      version;
    uint32_t      caps;

    if (dev == NULL) {
        return BCMC_EINVAL;
    }

    // Cleared first, so that a probe which fails half way cannot leave the
    // geometry of some earlier, different peripheral behind to be trusted by
    // the range checks below.
    dev->probed = false;

    s = bcmc_read_reg(dev, BCMC_REG_ID, &id);
    if (s != BCMC_OK) {
        return s;
    }
    if (id != BCMC_ID_VALUE) {
        return BCMC_ENODEV;
    }

    s = bcmc_read_reg(dev, BCMC_REG_VERSION, &version);
    if (s != BCMC_OK) {
        return s;
    }
    if (BCMC_VERSION_MAJOR(version) != BCMC_DRIVER_MAJOR) {
        return BCMC_EVERSION;
    }

    s = bcmc_read_reg(dev, BCMC_REG_CAPS, &caps);
    if (s != BCMC_OK) {
        return s;
    }

    // A peripheral that can hold no rows has no addressable weight window, so
    // nothing this driver offers would work on it. Better to refuse the device
    // than to hand out BCMC_ERANGE for every index forever afterwards.
    if (BCMC_CAPS_MAX_C(caps) == 0u) {
        return BCMC_ENODEV;
    }

    dev->version = version;
    dev->max_c   = BCMC_CAPS_MAX_C(caps);
    dev->val_w   = BCMC_CAPS_VAL_W(caps);
    dev->idx_w   = BCMC_CAPS_IDX_W(caps);
    dev->probed  = true;
    return BCMC_OK;
}

uint32_t bcmc_column_words(const bcmc_dev_t *dev)
{
    if (dev == NULL || !dev->probed) {
        return 0u;
    }
    return (dev->max_c + 31u) / 32u;
}

//---------------------------------------------------------------------------
// Row indices into the weight and offset windows
//
// One shared check, because there is one rule: WEIGHT[i] and OFFSET[i] exist
// for i < MAX_C and are unmapped above it (E1). The check is local only
// because bcmc_probe() read MAX_C rather than assuming it.
//---------------------------------------------------------------------------

static bcmc_status_t bcmc_check_row(const bcmc_dev_t *dev, uint32_t i)
{
    if (dev == NULL) {
        return BCMC_EINVAL;
    }
    if (!dev->probed) {
        return BCMC_EINVAL;
    }
    if (i >= dev->max_c) {
        return BCMC_ERANGE;
    }
    return BCMC_OK;
}

//---------------------------------------------------------------------------
// Primitives: the context
//---------------------------------------------------------------------------

bcmc_status_t bcmc_set_n(bcmc_dev_t *dev, uint32_t n)
{
    // N >= 1 is a mathematical precondition, and this driver checks no
    // mathematical precondition -- see the header. N is passed through.
    return bcmc_write_reg(dev, BCMC_REG_N, n);
}

bcmc_status_t bcmc_set_c(bcmc_dev_t *dev, uint32_t c)
{
    if (dev == NULL) {
        return BCMC_EINVAL;
    }
    if (!dev->probed) {
        return BCMC_EINVAL;
    }
    // C == MAX_C is legal, so this is not bcmc_check_row(). C > MAX_C is
    // refused here rather than at the bus: the C register is wide enough to
    // hold it, so the peripheral would accept the write and only then find
    // that the rows it names have no addresses. Refusing locally keeps the
    // context from ever describing a matrix that cannot be programmed.
    if (c > dev->max_c) {
        return BCMC_ERANGE;
    }
    return bcmc_write_reg(dev, BCMC_REG_C, c);
}

bcmc_status_t bcmc_write_weight(bcmc_dev_t *dev, uint32_t i, uint32_t weight)
{
    bcmc_status_t s = bcmc_check_row(dev, i);
    if (s != BCMC_OK) {
        return s;
    }
    return bcmc_write_reg(dev, BCMC_WEIGHT_BASE + (4u * i), weight);
}

bcmc_status_t bcmc_read_weight(bcmc_dev_t *dev, uint32_t i, uint32_t *weight)
{
    bcmc_status_t s = bcmc_check_row(dev, i);
    if (s != BCMC_OK) {
        return s;
    }
    return bcmc_read_reg(dev, BCMC_WEIGHT_BASE + (4u * i), weight);
}

bcmc_status_t bcmc_read_offset(bcmc_dev_t *dev, uint32_t i, uint32_t *offset)
{
    bcmc_status_t s = bcmc_check_row(dev, i);
    if (s != BCMC_OK) {
        return s;
    }
    return bcmc_read_reg(dev, BCMC_OFFSET_BASE + (4u * i), offset);
}

//---------------------------------------------------------------------------
// Primitives: the query
//
// Deliberately unchecked, both of them.
//
// CELL_ROW at or above C is NOT an error: such a row is an inactive lane and
// answers 0 for every column, which is the doctrine bcmc_column.v implements
// with `active = (ROW < C)`. A range check here would turn a defined answer
// into a driver error and would make this driver disagree with the hardware
// about what the matrix is. The same goes for a row at or above MAX_C.
//
// CELL_COL is bounded by N, which is a mathematical precondition and therefore
// nobody's to check.
//---------------------------------------------------------------------------

bcmc_status_t bcmc_set_cell_row(bcmc_dev_t *dev, uint32_t row)
{
    return bcmc_write_reg(dev, BCMC_REG_CELL_ROW, row);
}

bcmc_status_t bcmc_set_cell_col(bcmc_dev_t *dev, uint32_t col)
{
    return bcmc_write_reg(dev, BCMC_REG_CELL_COL, col);
}

//---------------------------------------------------------------------------
// Primitives: status, start, interrupt
//---------------------------------------------------------------------------

bcmc_status_t bcmc_read_status(bcmc_dev_t *dev, uint32_t *status)
{
    return bcmc_read_reg(dev, BCMC_REG_STATUS, status);
}

// START DOES NOT FIT IN ONE ACCESS, AND THAT IS THE REGISTER MAP'S DOING
//
// CTRL holds a command bit (START, W1S) beside a mode bit (IRQ_EN, RW), and
// rtl/bcmc_wb.v latches irq_en_q from wb_dat_i on every CTRL write. So a lone
// write of START would silently disable a caller's interrupt, and a driver
// whose start() turned off interrupts would be a trap rather than a
// primitive. IRQ_EN is readable -- reading CTRL is explicitly not an error --
// and that is exactly what makes preserving it possible.
//
// The read-modify-write is safe against the BUSY race, too: START while BUSY
// is E4, and an access that errs has no side effect whatsoever, so a refused
// start leaves IRQ_EN precisely as it was.
bcmc_status_t bcmc_start(bcmc_dev_t *dev)
{
    bcmc_status_t s;
    uint32_t      ctrl;

    s = bcmc_read_reg(dev, BCMC_REG_CTRL, &ctrl);
    if (s != BCMC_OK) {
        return s;
    }
    return bcmc_write_reg(dev, BCMC_REG_CTRL,
                          (ctrl & BCMC_CTRL_IRQ_EN) | BCMC_CTRL_START);
}

// No read-modify-write here, and the asymmetry with bcmc_start() is not an
// oversight. The only other field in CTRL is START, writing 0 to START does
// nothing at all, and START has no readable value to preserve. Reading the
// register first would therefore cost an access to recover a bit that is
// defined to be zero.
bcmc_status_t bcmc_irq_enable(bcmc_dev_t *dev, bool enable)
{
    return bcmc_write_reg(dev, BCMC_REG_CTRL, enable ? BCMC_CTRL_IRQ_EN : 0u);
}

// Write-1-to-clear, so this writes the IRQ bit alone. VALID and BUSY are
// read-only and ignore the zeros; the reserved bits ignore them too.
bcmc_status_t bcmc_irq_clear(bcmc_dev_t *dev)
{
    return bcmc_write_reg(dev, BCMC_REG_STATUS, BCMC_STATUS_IRQ);
}

//---------------------------------------------------------------------------
// Compositions: the three status bits
//
// Each is one bcmc_read_status() and one mask. They are separate functions
// because callers ask separate questions, not because the peripheral has three
// places to look.
//---------------------------------------------------------------------------

bcmc_status_t bcmc_busy(bcmc_dev_t *dev, bool *busy)
{
    bcmc_status_t s;
    uint32_t      status;

    if (busy == NULL) {
        return BCMC_EINVAL;
    }
    s = bcmc_read_status(dev, &status);
    if (s != BCMC_OK) {
        return s;
    }
    *busy = (status & BCMC_STATUS_BUSY) != 0u;
    return BCMC_OK;
}

// done means VALID: a matrix exists. See the header for why it is not !BUSY.
bcmc_status_t bcmc_done(bcmc_dev_t *dev, bool *done)
{
    bcmc_status_t s;
    uint32_t      status;

    if (done == NULL) {
        return BCMC_EINVAL;
    }
    s = bcmc_read_status(dev, &status);
    if (s != BCMC_OK) {
        return s;
    }
    *done = (status & BCMC_STATUS_VALID) != 0u;
    return BCMC_OK;
}

bcmc_status_t bcmc_irq_pending(bcmc_dev_t *dev, bool *pending)
{
    bcmc_status_t s;
    uint32_t      status;

    if (pending == NULL) {
        return BCMC_EINVAL;
    }
    s = bcmc_read_status(dev, &status);
    if (s != BCMC_OK) {
        return s;
    }
    *pending = (status & BCMC_STATUS_IRQ) != 0u;
    return BCMC_OK;
}

//---------------------------------------------------------------------------
// Compositions: waiting
//---------------------------------------------------------------------------

bcmc_status_t bcmc_wait(bcmc_dev_t *dev, uint32_t max_polls)
{
    uint32_t polls = 0u;

    for (;;) {
        bool          done   = false;
        bcmc_status_t s      = bcmc_done(dev, &done);
        if (s != BCMC_OK) {
            return s;
        }
        if (done) {
            return BCMC_OK;
        }

        // Counted after the poll, so max_polls == 1 performs one poll rather
        // than none, and max_polls == 0 never reaches the comparison.
        polls++;
        if (max_polls != 0u && polls >= max_polls) {
            return BCMC_ENOTREADY;
        }
    }
}

//---------------------------------------------------------------------------
// Compositions: reading the matrix
//---------------------------------------------------------------------------

bcmc_status_t bcmc_read_cell(bcmc_dev_t *dev, uint32_t row, uint32_t col, bool *bit)
{
    bcmc_status_t s;
    uint32_t      value;

    if (bit == NULL) {
        return BCMC_EINVAL;
    }

    s = bcmc_set_cell_row(dev, row);
    if (s != BCMC_OK) {
        return s;
    }
    s = bcmc_set_cell_col(dev, col);
    if (s != BCMC_OK) {
        return s;
    }
    s = bcmc_read_reg(dev, BCMC_REG_CELL, &value);
    if (s != BCMC_OK) {
        return s;
    }

    *bit = (value & 1u) != 0u;
    return BCMC_OK;
}

bcmc_status_t bcmc_read_column(bcmc_dev_t *dev, uint32_t col, uint32_t *words,
                               uint32_t nwords)
{
    bcmc_status_t s;
    uint32_t      need;
    uint32_t      k;

    if (dev == NULL || words == NULL) {
        return BCMC_EINVAL;
    }
    if (!dev->probed) {
        return BCMC_EINVAL;
    }

    need = bcmc_column_words(dev);
    if (nwords < need) {
        return BCMC_ERANGE;
    }

    s = bcmc_set_cell_col(dev, col);
    if (s != BCMC_OK) {
        return s;
    }

    // CELL_ROW takes no part in a COLUMN read, so it is not written here.
    for (k = 0u; k < need; k++) {
        s = bcmc_read_reg(dev, BCMC_COLUMN_BASE + (4u * k), &words[k]);
        if (s != BCMC_OK) {
            return s;
        }
    }

    // Words the caller supplied beyond the column are cleared rather than left
    // as they were, so that a buffer sized for a larger MAX_C reads as the
    // column it holds and not as that column plus whatever was there before.
    for (k = need; k < nwords; k++) {
        words[k] = 0u;
    }
    return BCMC_OK;
}

//---------------------------------------------------------------------------
// The composition that is the point of all the others
//
// Steps 3 to 7 of the programming sequence in docs/Register_Map.md. Compare it
// with that list: there is nothing here that is not on it.
//---------------------------------------------------------------------------

bcmc_status_t bcmc_load(bcmc_dev_t *dev, const uint32_t *weights, uint32_t n, uint32_t c)
{
    bcmc_status_t s;
    uint32_t      i;

    if (dev == NULL) {
        return BCMC_EINVAL;
    }
    // C == 0 is a legal transform with no weights, so a null vector is only an
    // error when there is a weight to fetch from it.
    if (weights == NULL && c != 0u) {
        return BCMC_EINVAL;
    }

    // C first. The register map allows steps 3 to 5 in any order, and this
    // order is chosen so that the range check on c happens before any of the
    // context is disturbed: a caller who asks for more rows than exist gets
    // BCMC_ERANGE with N and the weights still as they were.
    s = bcmc_set_c(dev, c);
    if (s != BCMC_OK) {
        return s;
    }
    s = bcmc_set_n(dev, n);
    if (s != BCMC_OK) {
        return s;
    }

    // Weights at or above c are not written. Rows there are inactive lanes
    // whatever the window holds, so writing them could not affect the matrix.
    for (i = 0u; i < c; i++) {
        s = bcmc_write_weight(dev, i, weights[i]);
        if (s != BCMC_OK) {
            return s;
        }
    }

    s = bcmc_start(dev);
    if (s != BCMC_OK) {
        return s;
    }

    // C + 4 clocks of transform, one clock minimum per poll, plus slack. See
    // bcmc_wait() in the header for why a poll count is a sound bound and a
    // microsecond count would not be.
    return bcmc_wait(dev, c + 4u + BCMC_POLL_MARGIN);
}

//---------------------------------------------------------------------------
// Diagnostics
//---------------------------------------------------------------------------

const char *bcmc_strstatus(bcmc_status_t status)
{
    switch (status) {
    case BCMC_OK:
        return "ok";
    case BCMC_EREFUSED:
        return "refused by the peripheral (err)";
    case BCMC_ERANGE:
        return "index outside the geometry CAPS reported";
    case BCMC_EINVAL:
        return "null argument or unprobed device";
    case BCMC_ENODEV:
        return "no BCMC peripheral at this address";
    case BCMC_EVERSION:
        return "register map major version not supported";
    case BCMC_ENOTREADY:
        return "still busy after the poll bound";
    default:
        return "unknown status";
    }
}

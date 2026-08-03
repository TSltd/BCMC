"""
BCMC peripheral model -- the executable form of docs/Register_Map.md.

This module stands to `rtl/bcmc_wb.v` exactly as `reference.py` stands to
`rtl/bcmc_core.v`:

    docs/BCMC.md          ->  reference.py     ->  rtl/bcmc_{core,cell}.v
    docs/Register_Map.md  ->  THIS MODULE      ->  rtl/bcmc_wb.v

It contains no mathematics of its own. Every matrix bit it returns comes from
`reference.py`, so a bus testbench never has to invent an expected value -- the
same rule that has governed the project since v0.2.

What this module models
-----------------------
The register map: address decode, access rules, the error model, and the three
bits of visible state (BUSY, VALID, IRQ). It is the peripheral seen from the
CPU side.

What it does not model
----------------------
Wishbone signalling. There is no `cyc`, no `stb`, no `ack`, no clock. A bus
cycle is a call to `read()` or `write()`, and `err` is a `BusError` exception.
Turning that into wires is the RTL's job, and checking that the RTL does it
correctly is `sim/bcmc_wb_test.cpp`'s job.

The one deliberate approximation is time: see `latency` in `BcmcPeripheral`.
"""

from reference import bcmc_cell, bcmc_core, check_weights

__all__ = [
    "ID_VALUE",
    "VERSION_VALUE",
    "REG_ID",
    "REG_VERSION",
    "REG_CAPS",
    "REG_CTRL",
    "REG_STATUS",
    "REG_N",
    "REG_C",
    "REG_CELL_ROW",
    "REG_CELL_COL",
    "REG_CELL",
    "COLUMN_BASE",
    "COLUMN_LIMIT",
    "WEIGHT_BASE",
    "OFFSET_BASE",
    "REGION_SIZE",
    "CTRL_START",
    "CTRL_IRQ_EN",
    "STATUS_BUSY",
    "STATUS_VALID",
    "STATUS_IRQ",
    "BusError",
    "BcmcPeripheral",
    "program",
    "read_column",
    "read_matrix",
    "read_offsets",
]

# ---------------------------------------------------------------------------
# The address map, transcribed from docs/Register_Map.md
# ---------------------------------------------------------------------------

REG_ID = 0x000
REG_VERSION = 0x004
REG_CAPS = 0x008
REG_CTRL = 0x00C
REG_STATUS = 0x010
REG_N = 0x014
REG_C = 0x018
REG_CELL_ROW = 0x01C
REG_CELL_COL = 0x020
REG_CELL = 0x024

COLUMN_BASE = 0x028
COLUMN_LIMIT = 0x048          # one past the reserved COLUMN span

WEIGHT_BASE = 0x400
OFFSET_BASE = 0x800
WINDOW_SPAN = 0x400           # bytes reserved for each window: MAX_C <= 256

REGION_SIZE = 0x1000          # 4 KiB

ID_VALUE = 0x42434D43         # 'BCMC'
VERSION_VALUE = 0x00000400    # major 0, minor 4, patch 0

CTRL_START = 1 << 0
CTRL_IRQ_EN = 1 << 1

STATUS_BUSY = 1 << 0
STATUS_VALID = 1 << 1
STATUS_IRQ = 1 << 2

SEL_WORD = 0xF


# ---------------------------------------------------------------------------
# Errors
# ---------------------------------------------------------------------------

class BusError(Exception):
    """
    A cycle that terminated in `err` rather than `ack`.

    `reason` is one of the four structural conditions E1..E4 named in
    docs/Register_Map.md. Nothing else may raise this.
    """

    def __init__(self, reason, addr, write, detail=""):
        self.reason = reason
        self.addr = addr
        self.write = write
        kind = "write" if write else "read"
        text = f"{reason}: {kind} at 0x{addr:03X}"
        if detail:
            text += f" -- {detail}"
        super().__init__(text)


class PreconditionError(Exception):
    """
    A mathematical precondition was violated.

    The specification permits the peripheral to do anything at all here, so
    there is nothing to model. Following `reference.py`, this model refuses
    rather than returning a wrong answer. Hardware is under no such obligation,
    which is exactly why testbenches must never drive these cases.
    """


# ---------------------------------------------------------------------------
# The peripheral
# ---------------------------------------------------------------------------

class BcmcPeripheral:
    """
    A BCMC peripheral, addressed by byte offset within its 4 KiB region.

    >>> dev = BcmcPeripheral()
    >>> hex(dev.read(REG_ID))
    '0x42434d43'
    >>> hex(dev.read(REG_CAPS))
    '0x10100040'

    The programming sequence of docs/Register_Map.md, for the worked example
    N = 8, weights = [6, 3, 5]:

    >>> program(dev, 8, [6, 3, 5])
    >>> read_offsets(dev, 3)
    [0, 6, 1]
    >>> read_matrix(dev, 8, 3)
    [[1, 1, 1, 1, 1, 1, 0, 0], [1, 0, 0, 0, 0, 0, 1, 1], [0, 1, 1, 1, 1, 1, 0, 0]]

    No matrix bit above is computed here; every one came from reference.py.

    Time
    ----
    `latency` is the number of subsequent bus accesses that observe
    `BUSY = 1`. It is a *test fixture*, not part of the register map: the
    specification says only that a transform takes some time and that BUSY is
    true while it does. `latency = 0` models a transform that software can
    never catch in progress; a positive value lets a test exercise the E4 rules
    and a polling driver.

    >>> slow = BcmcPeripheral(latency=2)
    >>> slow.write(REG_N, 8); slow.write(REG_C, 1)
    >>> slow.write(WEIGHT_BASE, 4)
    >>> slow.write(REG_CTRL, CTRL_START)
    >>> [slow.read(REG_STATUS) & STATUS_BUSY for _ in range(3)]
    [1, 1, 0]
    """

    def __init__(self, max_c=64, val_w=16, idx_w=16, latency=0):
        if not 1 <= max_c <= 256:
            raise ValueError("the windows are sized for 1 <= MAX_C <= 256")
        if not 1 <= val_w <= 32 or not 1 <= idx_w <= 32:
            raise ValueError("VAL_W and IDX_W must fit in a register field")
        if latency < 0:
            raise ValueError("latency must be >= 0")

        self.max_c = max_c
        self.val_w = val_w
        self.idx_w = idx_w
        self.latency = latency

        self.column_words = (max_c + 31) // 32

        self._val_mask = (1 << val_w) - 1
        self._idx_mask = (1 << idx_w) - 1

        self.n_accesses = 0
        self.reset()

    # -- state ------------------------------------------------------------

    def reset(self):
        """Synchronous reset. Every visible bit of state is defined here."""
        self.weights = [0] * self.max_c
        self.offsets = [0] * self.max_c

        self.n = 0
        self.c = 0
        self.cell_row = 0
        self.cell_col = 0

        self.busy = False
        self.valid = False
        self.irq = False
        self.irq_en = False

        # Sampled at START, and used to assert that a successful evaluation can
        # never mix a freshly written N or C with stale offsets.
        self._n_used = None
        self._c_used = None

        self._pending = None      # the offsets a running transform will deliver
        self._remaining = 0

    @property
    def irq_out(self):
        """The interrupt output pin: level-sensitive, gated by IRQ_EN."""
        return self.irq and self.irq_en

    @property
    def caps(self):
        return (self.idx_w << 24) | (self.val_w << 16) | self.max_c

    # -- time -------------------------------------------------------------

    def _advance(self):
        """
        Called once at the start of every bus access, before it is serviced.
        See the note on `latency` in the class docstring.
        """
        if not self.busy:
            return
        if self._remaining == 0:
            self._complete()
        else:
            self._remaining -= 1

    def _complete(self):
        """The transform finishes: the Core has delivered its whole stream."""
        self.offsets = list(self._pending)
        self._pending = None
        self.busy = False
        self.valid = True
        self.irq = True

    def _start(self):
        """
        Begin a transform.

        The Core's stream is computed here in one go because this model has no
        clock. `_pending` is not visible anywhere until `_complete`, which is
        what keeps OFFSET[] unreadable-and-stale rather than half written: the
        register map forbids reading it while BUSY in any case.
        """
        weights = self.weights[: self.c]
        try:
            check_weights(weights, self.n)
        except ValueError as exc:
            raise PreconditionError(
                f"START with N = {self.n}, C = {self.c}: {exc}"
            ) from exc

        # The offsets of rows that do not exist are cleared, so the window is
        # fully defined after every completed transform.
        self._pending = bcmc_core(weights, self.n) + [0] * (self.max_c - self.c)

        self._n_used = self.n
        self._c_used = self.c

        self.valid = False        # cleared before BUSY rises: never both true
        self.busy = True
        self._remaining = self.latency

    # -- evaluation -------------------------------------------------------

    def _evaluate(self, row):
        """
        M(row, CELL_COL), for a row that may or may not exist.

        Rows at or above C are inactive lanes: weight 0, offset 0, hence 0 for
        every column. This is the rule `rtl/bcmc_column.v` already implements
        as `active = (ROW < C)`, and it depends on C alone -- never on what is
        sitting in the WEIGHT window.
        """
        assert self._n_used == self.n and self._c_used == self.c, (
            "a readable matrix must be the one the transform computed"
        )
        if row >= self.c:
            return 0
        return bcmc_cell(self.weights[row], self.offsets[row], self.cell_col, self.n)

    def _column_word(self, k):
        word = 0
        for b in range(32):
            row = 32 * k + b
            if row >= self.max_c:
                break
            if self._evaluate(row):
                word |= 1 << b
        return word

    # -- the bus ----------------------------------------------------------

    def read(self, addr, sel=SEL_WORD):
        """One read cycle. Returns 32 bits, or raises BusError for `err`."""
        self.n_accesses += 1
        self._advance()
        self._check_access(addr, sel, write=False)

        if addr == REG_ID:
            return ID_VALUE
        if addr == REG_VERSION:
            return VERSION_VALUE
        if addr == REG_CAPS:
            return self.caps
        if addr == REG_CTRL:
            # START is a command, not a state, and always reads 0.
            return CTRL_IRQ_EN if self.irq_en else 0
        if addr == REG_STATUS:
            return (
                (STATUS_BUSY if self.busy else 0)
                | (STATUS_VALID if self.valid else 0)
                | (STATUS_IRQ if self.irq else 0)
            )
        if addr == REG_N:
            return self.n
        if addr == REG_C:
            return self.c
        if addr == REG_CELL_ROW:
            return self.cell_row
        if addr == REG_CELL_COL:
            return self.cell_col

        if addr == REG_CELL:
            self._require_matrix(addr, write=False)
            return self._evaluate(self.cell_row)

        if COLUMN_BASE <= addr < COLUMN_LIMIT:
            self._require_matrix(addr, write=False)
            return self._column_word((addr - COLUMN_BASE) // 4)

        if WEIGHT_BASE <= addr < WEIGHT_BASE + WINDOW_SPAN:
            return self.weights[(addr - WEIGHT_BASE) // 4]

        if OFFSET_BASE <= addr < OFFSET_BASE + WINDOW_SPAN:
            self._require_matrix(addr, write=False)
            return self.offsets[(addr - OFFSET_BASE) // 4]

        raise AssertionError("unreachable: _check_access should have refused")

    def write(self, addr, data, sel=SEL_WORD):
        """
        One write cycle.

        A write that raises BusError has no side effect of any kind: no
        register changes, no flag moves, no transform starts.
        """
        self.n_accesses += 1
        self._advance()
        self._check_access(addr, sel, write=True)

        if not 0 <= data <= 0xFFFFFFFF:
            raise ValueError(f"data 0x{data:X} does not fit in 32 bits")

        if addr == REG_CTRL:
            # START while BUSY is E4, and it is checked here rather than in
            # _check_access because it depends on the data, not the address:
            # writing CTRL with START clear is legal at any time. The guard
            # comes first so that an erring write leaves IRQ_EN untouched.
            if data & CTRL_START:
                self._start_guard(addr)
            # IRQ_EN before START: enabling it in the same write that starts a
            # transform must arm the pin for that transform's completion.
            self.irq_en = bool(data & CTRL_IRQ_EN)
            if data & CTRL_START:
                self._start()
            return

        if addr == REG_STATUS:
            if data & STATUS_IRQ:
                self.irq = False       # write 1 to clear; the rest is ignored
            return

        if addr == REG_N:
            self.n = data & self._val_mask
            self.valid = False
            return
        if addr == REG_C:
            self.c = data & self._idx_mask
            self.valid = False
            return
        if addr == REG_CELL_ROW:
            self.cell_row = data & self._idx_mask
            return                     # a query index is not the context
        if addr == REG_CELL_COL:
            self.cell_col = data & self._val_mask
            return

        if WEIGHT_BASE <= addr < WEIGHT_BASE + WINDOW_SPAN:
            self.weights[(addr - WEIGHT_BASE) // 4] = data & self._val_mask
            self.valid = False
            return

        raise AssertionError("unreachable: _check_access should have refused")

    # -- the error model --------------------------------------------------

    def _check_access(self, addr, sel, write):
        """The four structural conditions, in the order E3, E1, E2, E4."""
        if addr < 0 or addr >= REGION_SIZE or addr % 4 != 0:
            raise BusError("E1", addr, write, "outside the region or unaligned")

        if sel != SEL_WORD:
            raise BusError("E3", addr, write, f"sel = 0x{sel:X}, not a full word")

        readable = {
            REG_ID, REG_VERSION, REG_CAPS, REG_CTRL, REG_STATUS,
            REG_N, REG_C, REG_CELL_ROW, REG_CELL_COL, REG_CELL,
        }
        writable = {
            REG_CTRL, REG_STATUS, REG_N, REG_C, REG_CELL_ROW, REG_CELL_COL,
        }

        if addr in readable:
            if write and addr not in writable:
                raise BusError("E2", addr, write, "register is read-only")
        elif COLUMN_BASE <= addr < COLUMN_LIMIT:
            k = (addr - COLUMN_BASE) // 4
            if k >= self.column_words:
                raise BusError("E1", addr, write, f"COLUMN[{k}] does not exist")
            if write:
                raise BusError("E2", addr, write, "COLUMN is read-only")
        elif WEIGHT_BASE <= addr < WEIGHT_BASE + WINDOW_SPAN:
            self._check_index(addr, write, WEIGHT_BASE, "WEIGHT")
        elif OFFSET_BASE <= addr < OFFSET_BASE + WINDOW_SPAN:
            self._check_index(addr, write, OFFSET_BASE, "OFFSET")
            if write:
                raise BusError("E2", addr, write, "OFFSET is read-only")
        else:
            raise BusError("E1", addr, write, "unmapped")

        # E4: not meaningful in this state.
        if self.busy:
            if write and (
                addr in (REG_N, REG_C)
                or WEIGHT_BASE <= addr < WEIGHT_BASE + WINDOW_SPAN
            ):
                raise BusError("E4", addr, write, "context is busy")
            if not write and (
                addr == REG_CELL
                or COLUMN_BASE <= addr < COLUMN_LIMIT
                or OFFSET_BASE <= addr < OFFSET_BASE + WINDOW_SPAN
            ):
                raise BusError("E4", addr, write, "context is busy")

    def _check_index(self, addr, write, base, name):
        i = (addr - base) // 4
        if i >= self.max_c:
            raise BusError("E1", addr, write, f"{name}[{i}] exceeds MAX_C")

    def _require_matrix(self, addr, write):
        """
        START while BUSY, and matrix reads while !VALID, are the same rule:
        do not answer a question about a matrix that does not exist.
        """
        if not self.valid:
            raise BusError("E4", addr, write, "no valid matrix (VALID = 0)")

    # -- CTRL.START while busy is E4, checked where START is decoded -------

    def _start_guard(self, addr):
        if self.busy:
            raise BusError("E4", addr, True, "START while BUSY")


# ---------------------------------------------------------------------------
# Clients of the register map
#
# Everything below this line is a *consumer* of the peripheral, not part of it.
# These functions are the shape sw/bcmc.c will take, and nothing here may know
# anything the register map does not expose.
# ---------------------------------------------------------------------------

def program(dev, N, weights, poll=True):
    """
    The programming sequence of docs/Register_Map.md, steps 3 to 7.

    Note what is absent: no weight_valid, no offset_valid, no cycle counting.
    The wrapper owns the Prefix Stream Interface; software owns none of it.
    """
    if dev.read(REG_ID) != ID_VALUE:
        raise RuntimeError("not a BCMC peripheral")

    caps = dev.read(REG_CAPS)
    max_c = caps & 0xFFFF
    if len(weights) > max_c:
        raise ValueError(f"C = {len(weights)} exceeds MAX_C = {max_c}")

    dev.write(REG_N, N)
    dev.write(REG_C, len(weights))
    for i, w in enumerate(weights):
        dev.write(WEIGHT_BASE + 4 * i, w)

    dev.write(REG_CTRL, CTRL_START)

    if poll:
        # Wait for VALID, not for !BUSY. BUSY is already 0 after reset, when no
        # matrix exists, so !BUSY is the absence of a completion condition
        # rather than one. See step 7 of docs/Register_Map.md.
        while not dev.read(REG_STATUS) & STATUS_VALID:
            pass


def read_offsets(dev, C):
    """OFFSET[0 .. C-1], the second half of the canonical representation."""
    return [dev.read(OFFSET_BASE + 4 * i) for i in range(C)]


def read_column(dev, j, C):
    """
    Column j as a list of C bits, via CELL_COL and the COLUMN window.

    This is the projection an allocator wants: one bus write and
    ceil(C/32) reads per scheduling slot. Words above ceil(C/32) exist, but
    they hold only inactive lanes, so software has no reason to fetch them.
    """
    dev.write(REG_CELL_COL, j)
    words = [dev.read(COLUMN_BASE + 4 * k) for k in range((C + 31) // 32)]
    return [(words[i // 32] >> (i % 32)) & 1 for i in range(C)]


def read_matrix(dev, N, C, by_cell=False):
    """
    The whole matrix, read back through the register map alone.

    With `by_cell`, every element is fetched through CELL_ROW/CELL_COL/CELL
    instead, which is a different set of registers computing the same object --
    the bus-level echo of "cell and column are two projections of one
    function".
    """
    if by_cell:
        rows = []
        for i in range(C):
            dev.write(REG_CELL_ROW, i)
            row = []
            for j in range(N):
                dev.write(REG_CELL_COL, j)
                row.append(dev.read(REG_CELL) & 1)
            rows.append(row)
        return rows

    columns = [read_column(dev, j, C) for j in range(N)]
    return [[columns[j][i] for j in range(N)] for i in range(C)]


# ---------------------------------------------------------------------------
# Self-check
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    import doctest

    failures, tests = doctest.testmod()
    print(f"doctests: {tests - failures}/{tests} passed")
    if failures:
        raise SystemExit(1)

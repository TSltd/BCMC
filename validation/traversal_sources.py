"""
The v2.0c traversal sources, as a model with three separable layers.

Derived from `docs/Traversal_Sources_Specification.md` in order to falsify it --
the fourth time this project has written a model for that purpose, and the fourth
time it is expected to find something.

The layers are separated so that a failure says *where* it is. A composition
failure that could have come from the permutation, the preparation, the readiness
contract or the seam tells you nothing; these three do not share code.

    1. PURE SOURCE FUNCTIONS      pi(t) given a bound source. No time, no state.
                                  identity, affine (closed form AND the
                                  incremental form section 4.2 specifies), and a
                                  bank lookup.

    2. STATEFUL PREPARATION       seed -> ready. The affine's rejection stream
                                  and Euclid, the shuffled bank's fill, the
                                  double-buffered lead, and underrun.

    3. COMPOSITION                a source attached to the *existing* engine
                                  model. Same trigger schedule, same context:
                                  identity and affine must differ in order and
                                  agree in multiset, and identity wiring must
                                  reproduce the v2.0a trace exactly.

What it reuses, and what it refuses to reimplement
--------------------------------------------------
`observers.SplitMix32` for the generator and the rejection draw, because those
are pinned by `docs/Observers.md` and a second implementation of them would be a
second thing to get wrong. `observer_hw.bank_fill_cycles` for the fill, because
that figure is a *measurement* the specification declines to re-derive.
`observer_periph.replay` for the composition invariant, for the same reason it
was reused last time. And `sim/vectors/observer_prng.txt` for the *identity of
the draws the affine source consumes* -- which is what makes the rejection-stream
test a check against a frozen artefact rather than against this model.

Usage:
    python3 traversal_sources.py            # doctests
"""

from math import gcd

from observers import SplitMix32, permuted_order
from observer_hw import identity_source, bank_fill_cycles

# The width of N, of a step index, and of a column index. `rtl/bcmc_observer.v`'s
# VAL_W, and the affine source's arithmetic is where this number does real work:
# section 4.2's sum is VAL_W + 1 bits, and the width is not cosmetic.
VAL_W = 16

# Section 5.3's parameters.
BANK_N_MAX = 256
LEAD = 2


# ---------------------------------------------------------------------------
# Layer 1 -- pure source functions
# ---------------------------------------------------------------------------

def identity_sequence(N):
    """
    pi(t) = t. The control case, and the wiring the v2.0a suites re-run through.

    >>> identity_sequence(5)
    [0, 1, 2, 3, 4]
    """
    if N < 1:
        raise ValueError(f"N must be >= 1, got {N}")
    return list(range(N))


def affine_sequence(a, b, N):
    """
    The closed form of section 4.1: pi(t) = (a t + b) mod N.

    This is the *definition*, and it is what the incremental realisation must be
    held to. It is deliberately computed the expensive way -- with a real
    multiplication and a real modulus -- so that it shares no arithmetic with the
    form the RTL will use.

    >>> affine_sequence(3, 1, 7)
    [1, 4, 0, 3, 6, 2, 5]
    >>> sorted(affine_sequence(3, 1, 7)) == list(range(7))
    True
    """
    if N < 1:
        raise ValueError(f"N must be >= 1, got {N}")
    return [(a * t + b) % N for t in range(N)]


def affine_incremental(a, b, N, val_w=VAL_W):
    """
    The realisation section 4.2 specifies: one adder, one comparator, one
    conditional subtract, and **no multiplier anywhere**.

        pi(0)   = b
        pi(t+1) = pi(t) + a  mod N

    The sum is `val_w + 1` bits, which is the detail the specification calls out.
    `affine_incremental_narrow` is the same thing with that detail wrong.

    The precondition is `a < N`, and it is enforced rather than assumed: one
    conditional subtract only reduces a sum below `N` if the sum was below `2N`,
    which requires `a < N`. Section 4.3 always yields `a <= N - 1`, so this is
    free in practice -- but a caller that reuses a step from a larger `N` would
    otherwise get a silently wrong walk, and the whole point of this function is
    to be the form the RTL uses.

    >>> affine_incremental(3, 1, 7)
    [1, 4, 0, 3, 6, 2, 5]
    """
    if N < 1:
        raise ValueError(f"N must be >= 1, got {N}")
    if a >= N:
        raise ValueError(f"a must be < N (got a={a}, N={N})")
    out = []
    r = b % N
    for t in range(N):
        if t == 0:
            out.append(r)
        else:
            inc = r + a                  # val_w + 1 bits: nothing is dropped
            out.append(inc - N if inc >= N else inc)
        r = out[-1]
    return out


def affine_incremental_narrow(a, b, N, val_w=VAL_W):
    """
    **A mutation, kept because it is the trap the specification warns about.** The
    sum is truncated to `val_w` bits before the comparison, which is wrong for any
    `N` above `2^(val_w - 1)` and right for every `N` below it -- the worst shape
    of bug, because a testbench of small `N` never sees it.

    >>> affine_incremental_narrow(3, 1, 8) == affine_incremental(3, 1, 8)
    True
    """
    if N < 1:
        raise ValueError(f"N must be >= 1, got {N}")
    if a >= N:
        raise ValueError(f"a must be < N (got a={a}, N={N})")
    mask = (1 << val_w) - 1
    out = []
    r = b % N
    for t in range(N):
        if t == 0:
            out.append(r)
        else:
            inc = (r + a) & mask         # the truncation
            out.append(inc - N if inc >= N else inc)
        r = out[-1]
    return out


def bank_read(bank, t):
    """
    A completed bank's lookup. Combinational, and that is the specification's
    claim: the *fill* costs cycles, the *read* costs none.

    >>> bank_read([4, 2, 0, 3, 1], 3)
    3
    """
    return bank[t]


# ---------------------------------------------------------------------------
# Layer 2 -- stateful preparation
#
# What a source needs before it can answer: the affine's (a, b) from a seed, and
# the shuffled source's bank. Both are off the per-visit path; both are where
# `SEED_READY` comes from.
# ---------------------------------------------------------------------------

def uniform_with_stream(rng, m):
    """
    The pinned draw of `docs/Observers.md`, returning the value **and the raw
    draws it consumed**.

    The second half is the point. Section 4.3 makes consumed rejections part of
    the contract, and the tempting implementation mistake is to rewind the
    generator after a rejection -- which still yields a coprime `a`, eventually,
    and a different `pi` from the same seed. Recording the stream makes that
    visible; hiding it would not.

    >>> rng = SplitMix32(1)
    >>> v, draws = uniform_with_stream(rng, 5)
    >>> 0 <= v <= 5 and len(draws) == 1
    True
    >>> uniform_with_stream(SplitMix32(9), 0)
    (0, [])
    """
    if m < 0:
        raise ValueError(f"m must be >= 0, got {m}")
    if m == 0:
        return 0, []
    mask = 1
    while mask < m:
        mask = (mask << 1) | 1
    draws = []
    while True:
        z = rng.next()
        draws.append(z)
        x = z & mask
        if x <= m:
            return x, draws


def euclid_steps(a, b):
    """
    The step count a small *sequential subtraction* Euclid takes, which is what
    section 4.3's `gcd(a, N) = 1` check costs off the per-visit path.

    >>> euclid_steps(0, 1)
    1
    >>> euclid_steps(3, 7)
    6
    """
    steps = 0
    while b:
        if a < b:
            a, b = b, a
        a -= b
        steps += 1
    return steps


def derive_ab(N, seed):
    """
    Section 4.3, exactly: `a` by the pinned rejection draw, rejecting until
    `gcd(a, N) == 1` **with the rejected draws consumed**, and then `b` from the
    same stream. The order is contractual -- drawing `b` first gives a different
    permutation from the same seed and passes every property test except
    agreement with this reference.

    Returns `(a, b, draws)`, the draws being every `next()` the construction
    consumed.

    `N = 1` consumes nothing at all, because `uniform(0)` is defined as 0 without
    a draw and `gcd(0, 1) = 1`:

    >>> derive_ab(1, 0xBEEF)
    (0, 0, [])
    >>> a, b, draws = derive_ab(12, 0x1234)
    >>> gcd(a, 12) == 1 and 0 <= a < 12 and 0 <= b < 12
    True
    """
    if N < 1:
        raise ValueError(f"N must be >= 1, got {N}")
    rng = SplitMix32(seed)
    draws = []
    while True:
        a, d = uniform_with_stream(rng, N - 1)
        draws.extend(d)
        if gcd(a, N) == 1:
            break
    b, d = uniform_with_stream(rng, N - 1)
    draws.extend(d)
    return a, b, draws


def shuffle_from(rng, N):
    """
    One bank, from the *current* state of a stream -- which is what makes
    successive banks different permutations rather than the same one repeated.

    A fresh `SplitMix32(seed)` must reproduce the pinned reference exactly:

    >>> shuffle_from(SplitMix32(2024), 8) == permuted_order(8, 2024)
    True
    """
    p = list(range(N))
    for i in range(N - 1, 0, -1):
        j = rng.uniform(i)
        p[i], p[j] = p[j], p[i]
    return p


def bank_and_cost(rng, N):
    """
    Fill one bank and report its cycle cost, on the model `bank_fill_cycles`
    specifies: one `next()` per clock, and a rejection costs an extra clock.

    >>> rng = SplitMix32(7)
    >>> p, cost = bank_and_cost(rng, 16)
    >>> sorted(p) == list(range(16)) and cost >= 15
    True
    """
    p = list(range(N))
    cycles = 0
    for i in range(N - 1, 0, -1):
        mask = 1
        while mask < i:
            mask = (mask << 1) | 1
        while True:
            cycles += 1
            x = rng.next() & mask
            if x <= i:
                break
        p[i], p[x] = p[x], p[i]
    return p, cycles


def required_rate(N, seed):
    """
    The smallest `R` with `N * R >= c_fill(N)`: section 5.4's *sustained*
    condition, and the reason `R = 1` is not reachable for a shuffled source.

    >>> required_rate(8, 1)
    1
    >>> required_rate(128, 1) >= 2
    True
    """
    cost = bank_fill_cycles(N, seed)
    return max(1, -(-cost // N))


class Source:
    """The shape every source presents: `load`, `tick`, `ready`, `pi`."""

    kind = "?"

    def __init__(self, N, seed=0):
        if N < 1:
            raise ValueError(f"N must be >= 1, got {N}")
        self.N = N
        self.seed = seed & 0xFFFFFFFF
        self.loads = 0
        self.underrun_flag = False     # the PRESENTED level
        self.underrun_next = False     # the boundary decision, scheduled
        self._on_load()

    def load(self, seed):
        """A write to `OBS_SEED`, or to the selector: readiness is void again."""
        self.seed = seed & 0xFFFFFFFF
        self.loads += 1
        self.underrun_flag = False
        self.underrun_next = False
        self._on_load()

    def _on_load(self):
        raise NotImplementedError

    def tick(self, ts_t=0):
        raise NotImplementedError

    def ready(self):
        raise NotImplementedError

    def pi(self, ts_t):
        raise NotImplementedError


class IdentitySource(Source):
    """
    `pi(t) = t`, and **ready as a constant** -- section 7.1:

        The identity's contribution to that AND is a constant `1`, which is also
        why section 8.1's table is right to give it no `ready` port.

    `rtl/bcmc_src_identity.v` is one `assign` with no clock and no `ready` port,
    because `pi(t) = t` is a function of its argument and a function cannot be
    late. So this source is never unready -- **not after a load, and not at
    reset**. The one-cycle recovery after a write to `OBS_SEED`, the selector or
    `N` is the WINDOW's uniform latch (section 7.1), ANDed with this constant.
    Modelling it here as well made the identity unready at construction, i.e. at
    reset, which section 7.1 does not list among the events that clear readiness.

    (This paragraph previously said the opposite -- that this `ready()` modelled
    the window's rule "to keep the *system* observable in one place". That
    convenience put the recovery in the wrong layer and imported a post-load
    countdown into the reset state. It is a model defect against the frozen
    contract, and the frozen-corpus differ caught it: eight of the frozen
    corpus's eleven START writes land in cycle 0.)

    >>> s = IdentitySource(4)
    >>> s.ready()                    # a constant 1, at reset and after a load
    True
    >>> s.tick()
    >>> s.ready()
    True
    >>> [s.pi(t) for t in range(4)]
    [0, 1, 2, 3]
    """

    kind = "identity"

    def _on_load(self):
        # A constant 1 (section 7.1): a load does not make a function of its
        # argument late. The window's latch provides the uniform one-cycle
        # recovery for every source, this one included -- see Window._load_all.
        self.countdown = 0

    def tick(self, ts_t=0):
        self.countdown = max(0, self.countdown - 1)

    def ready(self):
        return self.countdown == 0

    def pi(self, ts_t):
        return ts_t


def walk_pass(source, cycles_per_visit=1):
    """
    One pass: ask `pi(0 .. N-1)` in order, advancing `cycles_per_visit` clocks
    between asks.

    The step register advances once per *visit*, not once per clock, which is the
    whole reason a rate above one is expressible at all. Modelling the two as the
    same thing would make the rate bound untestable.

    >>> walk_pass(IdentitySource(3))
    [0, 1, 2]
    """
    out = []
    for t in range(source.N):
        out.append(source.pi(t))
        for _ in range(cycles_per_visit):
            source.tick(t)
    return out


class AffineSource(Source):
    """
    `(a, b)` from the seed by section 4.3, and the answer computed the way
    section 4.2 says: a register holding `pi` at the step last asked about, plus
    `a`, reduced by one comparison and one conditional subtract.

    `pi(t)` is the combinational answer for the cycle; `tick(t)` is the edge.
    That is the same split the seam has, and keeping it means a pass and a
    per-cycle trace can both be expressed without either shaping the source.

    >>> s = AffineSource(7, 0x1234)
    >>> s.ready()
    False
    >>> while not s.ready():
    ...     s.tick()
    >>> walk_pass(s) == affine_sequence(s.a, s.b, 7)
    True
    """

    kind = "affine"

    def _on_load(self):
        self.a, self.b, self.draws = derive_ab(self.N, self.seed)
        self.latency = len(self.draws) + euclid_steps(self.a, self.N)
        self.countdown = max(1, self.latency)
        self.r = self.b % self.N
        self.t_last = None

    def tick(self, ts_t=0):
        answer = self.pi(ts_t)
        self.countdown = max(0, self.countdown - 1)
        if self.ready() and (self.t_last is None or ts_t != self.t_last):
            self.r = answer
            self.t_last = ts_t

    def ready(self):
        return self.countdown == 0

    def pi(self, ts_t):
        if ts_t == 0:
            return self.b % self.N
        inc = self.r + self.a              # VAL_W + 1 bits
        return inc - self.N if inc >= self.N else inc


class ShuffledSource(Source):
    """
    The bank, the lead, and `SEED_UNDERRUN`.

    Readiness is the *lead*: `lead` complete banks must exist before a pass may
    start. At a pass boundary the next bank is activated if one is complete, and
    otherwise **the bank already playing repeats and `underrun` is set** -- never
    a partial bank, which is what keeps O1 unconditional.

    >>> s = ShuffledSource(8, 1)
    >>> s.ready()
    False
    >>> for _ in range(40):
    ...     s.tick()
    >>> s.ready()
    True
    >>> s.start_pass() is not None
    True
    >>> sorted(s.walk_bank()) == list(range(8))
    True
    """

    kind = "shuffled"

    def __init__(self, N, seed=0, lead=LEAD, bank_n_max=BANK_N_MAX):
        self.lead = lead
        self.bank_n_max = bank_n_max
        super().__init__(N, seed)

    def _on_load(self):
        self.rng = SplitMix32(self.seed)
        # Section 5.5's TWO banks, and nothing that could grow into a queue: a slot
        # holds a completed bank or None (EMPTY / being filled), `playing` names the
        # PLAYING slot, and `filling` is the slot the current fill is going into.
        # `filling` can only ever be a FREE slot, which is what bounds the buffer at
        # two -- "it funds the buffer, it does not create a queue". (Finding 31.)
        self.bank = [None, None]
        # Bank 0 is the presented bank from reset -- `playing_q`'s reset value in
        # the RTL, where "The first pass plays bank 0" follows from nothing having
        # switched it, NOT from a first-pass event. There is no None state.
        # (Finding 32.)
        self.playing = 0
        self.filling = None
        self.built = 0                 # banks completed since the load
        self.startup_done = False
        self.ts_t_q = 0                # finding 30's boundary memory
        self.unservable = self.N > self.bank_n_max
        if not self.unservable:
            self.filling = self._free_slot()
            self._start_fill()

    def _free_slot(self):
        """
        A slot the fill may write into: one that holds **no bank**.

        Not "one that is not playing": `playing` is only a *pointer*, and at reset
        it is 0 while bank 0's state is EMPTY -- so excluding it deadlocks the fill
        and `ready()` can never be satisfied. The RTL fills any EMPTY bank, and the
        read side is safe for the same reason it is there: during a pass the read
        slot always holds a bank (COMPLETE or PLAYING), never EMPTY. §5.5's "the
        fill can never write the bank being read" therefore holds without this
        exclusion, and the two-bank bound still holds because at most one slot can
        be empty at a time once a pass is playing.
        """
        for i in (0, 1):
            if self.bank[i] is None:
                return i
        return None

    def _start_fill(self):
        """
        Begin one bank's serial fill: **one setup cycle** (the per-bank mask clear --
        section 5.5: "cleared at each fill start", "a single assignment") followed by
        `c_fill(N)` shuffle-step cycles (section 5.2). Section 5.4's arithmetic is
        therefore `1 + c_fill(N)` per bank, for EVERY bank including the first, and the
        schedule is serial: `tick` only calls here from a free slot, so the next bank's
        setup cannot begin until the preceding bank has completed.

        The cost change is timing only -- the bank and the generator draws are exactly
        what `bank_and_cost` produced before, so the permutation, the draw stream and
        `bank_fill_cycles`' meaning are all untouched. (Finding 34, adjudicated: the
        specification's `LEAD * c_fill(N)` had silently assumed a zero-cost setup.)
        """
        self.fill, cost = bank_and_cost(self.rng, self.N)
        self.fill_remaining = 1 + cost          # 1 setup cycle + c_fill(N) steps

    def tick(self, ts_t=0):
        if self.unservable:
            return
        # The level is REGISTERED -- section 7.3: "a level describing the boundary,
        # set when a boundary had to repeat" -- so the decision this edge makes is
        # presented from the NEXT edge, exactly as rtl/bcmc_src_shuffled.v's
        # `underrun_q` register behaves. (That module's port comment said "sticky
        # until load", which finding 25 corrected into this per-boundary level.)
        # The transfer therefore happens BEFORE this edge's boundary can change it.
        self.underrun_flag = self.underrun_next

        # The pass boundary, derived from the shared seam exactly as
        # rtl/bcmc_src_shuffled.v derives it: a TRANSITION into `ts_t == 0`. The
        # first ask of a pass is `ts_t == 0` with `ts_t_q` already 0, so it is not a
        # transition -- which is why the first pass plays the first bank with no
        # special case, and why `bind_pass`'s explicit `start_pass()` cannot
        # double-consume a boundary for a bound pass (which never wraps).
        #
        # Read BEFORE the fill advance below, so a bank completing on this same
        # edge is not seen by this boundary -- matching the RTL, where the boundary's
        # `do_switch` and the fill's completion both register from the pre-edge
        # state.
        #
        # Deriving it HERE rather than from a composition callback is what gives an
        # UNSELECTED source the same boundary observations as a selected one: the
        # window already ticks every source with the same `ts_t`. (Finding 30.)
        if (ts_t == 0) and (ts_t != self.ts_t_q):
            self.start_pass()
        self.ts_t_q = ts_t

        if self.filling is None:
            return                     # both slots account for themselves

        self.fill_remaining -= 1
        if self.fill_remaining <= 0:
            self.bank[self.filling] = self.fill
            self.built += 1
            if self.built >= self.lead:
                # Latched, and never cleared by an underrun: this is the
                # start-up condition of section 5.4, not a queue-depth test.
                self.startup_done = True
            # The next fill starts only if a slot is free. Once a pass is playing,
            # the other slot is either COMPLETE (waiting to hand over) or the one a
            # switch just freed -- never a third queued bank.
            self.filling = self._free_slot()
            if self.filling is not None:
                self._start_fill()

    def ready(self):
        """
        Start-up complete and the geometry is servable -- **latched**, not a
        queue-depth test.

        This was `len(self.banks) >= lead` until the RTL forced the question, and
        that formulation cannot survive steady state. With the two banks section
        5.4's table implies, the queue holds at most one bank once a pass is
        playing (the other is active), so a depth test drops `ready` the moment
        the first pass starts -- refusing START for a source that is working
        perfectly, and, worse, making `ready` a *transient* that composition code
        would trip over. `bind_pass` did exactly that: it refuses a source whose
        `ready()` is false, so binding a second pass to a shuffled source raised.

        The property that matters is section 5.4's: LEAD banks have been built
        since the load, so the buffer is funded. Nothing needs to un-set it, and
        an underrun must not: violating the rate bound is a *freshness* fault
        (section 7.3), reported on `underrun`, and a source that repeats a
        complete bank is still perfectly serviceable.

        Unservable `N` is still a readiness failure (section 5.3) and is the one
        thing that is not latched, because no amount of time fixes it.

        >>> s = ShuffledSource(8, 1)
        >>> s.ready()
        False
        >>> while s.built < LEAD:
        ...     s.tick()
        >>> s.ready()
        True
        >>> _ = s.start_pass()          # a boundary: it SWITCHES, freeing the other slot
        >>> sum(1 for i in (0, 1) if i != s.playing and s.bank[i] is not None)
        0
        >>> s.ready()                   # ... and still ready, because it latched
        True

        (This model's queue is unbounded, so it holds a bank for every fill it
        finishes. The hardware has two banks, so its queue is at most one while a
        pass is playing. The *rate* condition of section 5.4 is the same
        statement about both, and it is the one the phase is built on.)
        """
        return (not self.unservable) and self.startup_done

    def walk_bank(self):
        """The pass the active bank defines. Combinational, and free."""
        return [bank_read(self.bank[self.playing], t) for t in range(self.N)]

    def pi(self, ts_t):
        b = self.bank[self.playing]
        if b is None:
            return 0
        return bank_read(b, ts_t)

    def start_pass(self):
        """
        A pass boundary. Returns the bank the pass will read.

        `ready()` is what admits the *first* pass, so the pop condition is the
        steady-state one: **any** completed bank is taken. Holding one back in
        reserve would be a scheduling policy the specification does not state,
        and it would hide the underrun that section 5.4's rate bound exists to
        predict.

        `underrun_flag` is a *level* for this boundary -- set when the boundary
        had to repeat, cleared when it found its bank -- and not a sticky latch.
        A sticky latch could not answer the only question asked of it ("did
        *this* pass get a fresh bank?"), and the wrapper still gets a sticky
        software view by latching the level into its own RW1C bit.

        >>> s = ShuffledSource(4, 3)
        >>> while not s.ready():
        ...     s.tick()
        >>> _ = s.start_pass()          # boundary 1: bank 1 is COMPLETE, so it switches
        >>> s.tick()                    # the level is REGISTERED: presented from here
        >>> s.underrun_flag
        False
        >>> for _ in range(64):
        ...     s.tick()                # serial refill of the freed slot: setup + c_fill
        >>> _ = s.start_pass()          # boundary 2: finds its bank
        >>> s.tick()
        >>> s.underrun_flag
        False
        >>> _ = s.start_pass()          # boundary 3: no clocks since, so it repeats
        >>> s.tick()
        >>> s.underrun_flag
        True
        >>> sorted(s.walk_bank()) == list(range(4))
        True
        """
        # NO first-pass special case (finding 32): bank 0 is already playing from
        # reset, so every boundary is an ordinary switch attempt. The RTL says the
        # same thing -- "there is NO special case for the first pass here -- and
        # there must not be: an earlier revision initialised `started_q` at the
        # *first* boundary, which consumed the first wrap without switching".
        other = 1 - self.playing
        if self.bank[other] is not None:
            # The alternate bank is COMPLETE: hand it in, and free the slot that was
            # playing so it can be refilled.
            self.bank[self.playing] = None
            self.playing = other
            self.underrun_next = False     # this boundary found its bank
            if self.filling is None:
                self.filling = self._free_slot()
                if self.filling is not None:
                    self._start_fill()
        else:
            # The invariant, as a transition: at EVERY boundary either the alternate
            # bank is COMPLETE and becomes PLAYING, or the current bank remains
            # PLAYING and `underrun_flag` asserts. Never a third bank, and never a
            # stall.
            self.underrun_next = True      # repeat the bank already playing
        return self.bank[self.playing]


# ---------------------------------------------------------------------------
# Layer 3 -- composition
#
# `SequentialEngine` already has the seam: it takes `source`, a pure `t -> column`,
# and calls it where the RTL has `col_q`. What layer 3 has to establish is that a
# *stateful* source can be placed behind a *pure* seam at all, and that doing so
# changes the ordering and nothing else.
# ---------------------------------------------------------------------------

def bind_pass(source):
    """
    Adapt a stateful `Source` for one pass, returning the pure `t -> column` the
    engine's seam takes.

    This is where the layer boundary becomes explicit, and it is the
    specification's own claim restated as code: the seam is **combinational**, so
    a source may not be late, and anything needing time must have been done
    before the pass. `ready()` is that precondition, and `start_pass()` is the
    boundary at which a completed bank becomes the active one.

    The consequence worth naming: the returned function is only stable because
    the pass cannot change the state it reads. A source that prepared itself on
    the fly would be withholding, which section 2 declines.

    >>> s = IdentitySource(4)
    >>> s.tick()
    >>> bind_pass(s)(3)
    3
    """
    if not source.ready():
        raise ValueError("source is not ready; no pass is admitted until it is")
    # NO start_pass() here (finding 32). The RTL has no first-pass event: bank 0 is
    # presented from reset, and `ready()` above is the whole precondition -- it is
    # section 5.4's, and it is the same condition the window's E4 uses. Injecting a
    # boundary here consumed the first wrap as a "selection" and made the first-pass
    # underrun the RTL reports unrepresentable.
    return lambda t: source.pi(t)


def pass_through_engine(N, source_fn, oneshot=False, trigger_every=1):
    """
    Run one pass through the **existing** v2.0a engine and return the visits it
    emitted, in order.

    The stimulus is deliberately the dullest one that completes a pass: a `start`
    at cycle 0 and a trigger at each step thereafter. What is being tested is the
    source, not the FSM, and a complicated stimulus would make a failure
    ambiguous between the two.

    >>> pass_through_engine(4, identity_source)
    [0, 1, 2, 3]
    """
    from observer_hw import simulate
    n_triggers = N - 1 if N >= 1 else 0
    step = max(1, trigger_every)
    trig = [1 + step * i for i in range(n_triggers)]
    cycles = (max(trig) if trig else 1) + 2
    r = simulate(N, start_cycle=0, trigger_cycles=trig, oneshot=oneshot,
                 cycles=cycles, source=source_fn)
    return r.visits


def engine_signature(N, source_fn, **kw):
    """
    The engine's full observable trace for one pass, as a tuple. Used to compare
    two wirings for *identity* rather than merely for equal visit sets.

    >>> engine_signature(2, identity_source) == engine_signature(2, identity_source)
    True
    """
    from observer_hw import simulate
    n_triggers = N - 1 if N >= 1 else 0
    trig = list(range(1, 1 + n_triggers))
    cycles = (max(trig) if trig else 1) + 2
    r = simulate(N, start_cycle=0, trigger_cycles=trig, cycles=cycles,
                 source=source_fn, **kw)
    return tuple((c.running, c.column, c.visit_valid, c.done, c.aborted)
                 for c in r.cycles)


def affine_pure(a, b, N):
    """
    The closed form as a pure seam function, which is what the engine takes.

    The distinction from `AffineSource.pi` is the whole reason section 4.2 needs
    justifying: the incremental realisation is **not** a pure function of `t`. It
    holds `pi` at the step last asked about and adds `a`, so it is correct only
    because the engine asks `0, 1, 2, ...` within a pass and reloads at every
    pass start. That is true of this engine and is worth stating rather than
    assuming.

    >>> affine_pure(3, 1, 7)(2)
    0
    """
    return lambda t: (a * t + b) % N


def incremental_survives_abort(a, b, N):
    """
    After a pass that died at step `k`, the *next* pass must reproduce the
    reference sequence -- even though the register still holds a value from the
    middle of the dead pass.

    This works only because `t == 0` is answered directly from `b` rather than
    derived, and it is the reason section 4.2's `pi(0) = b` is a load rather than
    an iteration. A derivation would be one-shot: correct the first time through
    and wrong on every restart.

    The first pass here is left *mid-step* on purpose -- `r` is deliberately not
    reset -- so that a restart leaning on the stale value fails.

    >>> incremental_survives_abort(3, 1, 7)
    True
    """
    ref = affine_sequence(a, b, N)
    s = AffineSource(N, 0)
    s.a, s.b = a, b
    s.countdown = 0
    for died_at in range(1, N):
        s.r = s.b
        s.t_last = None
        for t in range(died_at):           # the dead pass, left mid-step
            _ = s.pi(t)
            s.tick(t)
        s.t_last = None                    # a restart reloads the step register
        got = []
        for t in range(N):                 # and r is *not* reloaded
            got.append(s.pi(t))
            s.tick(t)
        if got != ref:
            return False
    return True


# ---------------------------------------------------------------------------
# Self-check
# ---------------------------------------------------------------------------

def _headlines():
    """A few facts worth seeing at a glance, printed by the self-check."""
    lines = []

    bij = True
    for N in (1, 2, 3, 7, 16):
        if sorted(pass_through_engine(N, identity_source)) != list(range(N)):
            bij = False
    lines.append(f"identity, N <= 16, every pass a bijection: {bij}")

    aff = pass_through_engine(7, affine_pure(3, 1, 7))
    ident = pass_through_engine(7, identity_source)
    lines.append(f"affine N=7 order differs from identity: {aff != ident}")
    lines.append(f"affine N=7 multiset equal: {sorted(aff) == sorted(ident)}")

    a7, b7, _ = derive_ab(7, 1)
    from_engine = pass_through_engine(7, affine_pure(a7, b7, 7))
    lines.append(f"affine N=7 equals the closed form: "
                 f"{from_engine == affine_sequence(a7, b7, 7)}")

    wide = True
    for N in (2, 3, 4, 5, 7, 64, 210, 2310):
        aN, bN, _ = derive_ab(N, 5)
        if affine_incremental(aN, bN, N) != affine_sequence(aN, bN, N):
            wide = False
    lines.append(f"affine incremental == closed form, N up to 2310: {wide}")

    sh = ShuffledSource(16, 1)
    ready_at = None
    for c in range(1, 200):
        sh.tick()
        if sh.ready() and ready_at is None:
            ready_at = c
    lines.append(f"shuffled N=16 ready at cycle {ready_at} "
                 f"(lead {LEAD} banks, fill >= 15 each)")
    sh.start_pass()
    lines.append(f"shuffled N=16 bank is a permutation: "
                 f"{sorted(sh.walk_bank()) == list(range(16))}")
    return lines


if __name__ == "__main__":
    import doctest

    failures, tests = doctest.testmod()
    if failures:
        raise SystemExit(1)

    print(f"traversal_sources.py: {tests} doctests pass")
    for line in _headlines():
        print(f"  {line}")

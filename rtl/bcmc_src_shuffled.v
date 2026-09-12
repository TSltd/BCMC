//===========================================================================
// bcmc_src_shuffled.v -- the shuffled traversal source (v2.0c)
//
//     (N, seed)  ->  ts_pi = the next Fisher-Yates bank's entry for ts_t
//
// A source behind the seam of rtl/bcmc_observer.v, and the only buffered one:
// pi(t) of a shuffle cannot be computed on demand, because the state after t
// swaps depends on the whole history. So a bank is built in the background and
// read combinationally, which is why a slow fill does not become a slow visit
// (section 5.2).
//
// Index for index with the software family
// ----------------------------------------
// The bank is the permutation `docs/Observers.md` pins: the 32-bit SplitMix
// generator, the rejection draw, downward swaps, `j == i` legal. For the same
// seed and N this reproduces `observers.permuted_order` exactly, so the existing
// table `sim/vectors/observer_prng.txt` holds this module to account and no new
// reference is needed for the family. That is the whole reason the bank is worth
// paying for (section 5.1).
//
// The identity is implicit, and that is load-bearing
// -------------------------------------------------
// Section 5.2's `~1.4 N` fill counts `next()` calls, and writing `N` entries of
// identity first would add `N` cycles -- making the fill about 2.25 N and moving
// section 5.4's sustained rate from two clocks per visit to three. So the
// identity is never materialised: each entry has a **written-mask**, an unwritten
// entry reads as its own index, and a swap marks both endpoints written. For a
// complete bank that is identical to the reference, because an entry no swap ever
// touched holds its index there too. The mask clears in one cycle, which `N`
// writes cannot be. Section 5.5 records the decision.
//
// Two write ports, because a swap is two writes
// --------------------------------------------
// A shuffle step writes position `i` *and* position `j`. Both land in the cycle of
// the accepted draw, which is what makes the fill the draw count rather than twice
// the step count. `j == i` is a single write (and a no-op).
//
// The generator's state is its COUNTER, not its output
// ----------------------------------------------------
// SplitMix32 keeps a counter as its state and derives each output by mixing the
// counter after incrementing it. `rtl/bcmc_src_affine.v` assigned the mixed value
// back into the state; the first draw from a seed is correct either way, so only a
// derivation needing a second draw went wrong -- and only a bench could see it.
// Here the two are separate wires with separate names, and the register advances
// to `p_count`.
//
// The bank states, which is where the invariant lives
// --------------------------------------------------
//     EMPTY     the mask is being (or about to be) cleared; not readable
//     COMPLETE  holds a whole permutation; queued or ready
//     PLAYING   holds the permutation the read path is using
//
// A bank is only ever filled in EMPTY, and the read path only ever uses PLAYING
// (or, at a boundary, the COMPLETE bank about to play). With two banks there is at
// most one EMPTY at a time, so **the fill can never write the bank being read** --
// which is the property section 5.5 says O1 cannot establish, and which a partial
// in-place shuffle cannot be caught violating, because it is still a permutation.
// The assertion below checks it directly; no debug port is added to make that
// possible.
//
// The mask is combinational, and it has to be
// -------------------------------------------
// `uniform(i)` needs the smallest `2^k - 1 >= i`, and i changes every step. A
// sequential mask loop -- which is what `rtl/bcmc_src_affine.v` uses, correctly,
// because it runs once per seed -- would cost up to VAL_W cycles *per step* here
// and destroy the fill figure. `mask_for` below is one combinational function.
//
// Generic Verilog-2005. Nothing here is specific to any FPGA family.
//===========================================================================

`default_nettype none

module bcmc_src_shuffled #(
    // N, a step index and a column index all fit in VAL_W bits.
    parameter VAL_W = 16,
    // Entries per bank. A power of two, so that an address is {bank, index}.
    parameter BANK_N_MAX = 256,
    // Banks built before the first pass is admitted (section 5.4). With two banks
    // this is the start-up reserve, not a queue. Two bits, because it is compared
    // against the banks-built counter and nothing else.
    parameter LEAD = 2
) (
    input  wire                clk,
    input  wire                rst,       // synchronous, active high
    input  wire [VAL_W-1:0]    N,         // row length, N >= 1
    input  wire [31:0]         seed,
    input  wire                load,      // one-cycle: re-seed and rebuild
    input  wire [VAL_W-1:0]    ts_t,      // the step the engine is asking about
    output wire [VAL_W-1:0]    ts_pi,     // pi(ts_t), combinational
    output wire                ready,     // SEED_READY (start-up latched)
    output wire                underrun   // set at a boundary that had to repeat
);

    localparam AW    = $clog2(BANK_N_MAX);     // index bits within a bank
    localparam DEPTH = 2 * BANK_N_MAX;         // both banks, address {bank, idx}

    // Bank states. Two bits each, three values.
    localparam [1:0] ST_EMPTY    = 2'd0,
                     ST_COMPLETE = 2'd1,
                     ST_PLAYING  = 2'd2;

    //--- the pinned generator (docs/Observers.md): counter, and output ---------

    localparam [31:0] GOLDEN = 32'h9E3779B9;
    localparam [31:0] MUL1   = 32'h21F0AAAD;
    localparam [31:0] MUL2   = 32'h735A2D97;

    //--- the two banks, and the mask that makes the identity implicit ---------

    reg [VAL_W-1:0] bank_q  [0:DEPTH-1];
    reg [DEPTH-1:0] wmask_q;               // 1 = this entry has been written

    //--- registers ------------------------------------------------------------

    reg [31:0]      pstate_q;      // the COUNTER. Never the mixed output.
    reg [1:0]       st0_q;         // bank 0's state
    reg [1:0]       st1_q;         // bank 1's state
    reg             playing_q;     // the bank the read path uses
    reg             fill_busy_q;   // a fill is in progress
    reg             fill_bank_q;   // which bank is being filled
    reg [VAL_W-1:0] i_q;           // the shuffle index, N-1 down to 1
    reg [1:0]       built_q;       // banks completed since load, saturating
    reg             ready_q;       // start-up latched
    reg             underrun_q;    // sticky until load
    reg [VAL_W-1:0] ts_t_q;        // ts_t last cycle: the boundary detector

    wire [31:0] p_count = pstate_q + GOLDEN;               // next counter value
    wire [31:0] p_z2    = (p_count ^ (p_count >> 16)) * MUL1;
    wire [31:0] p_z3    = (p_z2 ^ (p_z2 >> 15)) * MUL2;
    wire [31:0] p_out   = p_z3 ^ (p_z3 >> 15);             // this draw's output

    //--- the smallest 2^k - 1 that is >= m: `uniform(m)`, combinationally -----
    //
    // The same recurrence the affine source runs sequentially, unrolled by the
    // synthesiser into a chain of VAL_W-1 steps. It has to be combinational here
    // because `m` changes every shuffle step (see the header).

    function [31:0] mask_for;
        input [VAL_W-1:0] m;
        integer           k;
        reg   [31:0]      acc;
        begin
            acc = 32'd1;
            for (k = 0; k < VAL_W-1; k = k + 1)
                if (acc < m)
                    acc = (acc << 1) | 32'd1;
            mask_for = acc;
        end
    endfunction

    //--- the pass boundary ----------------------------------------------------
    //
    // The engine asks about `ts_t = t_next`, and `ts_t == 0` is the ask for step
    // 0 -- which happens during IDLE, at a start, and at the wrap from step N-1.
    // So the ask for the *first* step of a pass is also the ask that switches
    // banks, and detecting it as a *transition* into 0 makes it fire once per pass
    // rather than on every IDLE cycle. The engine drives 0 in IDLE (section 8.2)
    // for the accumulator's sake; this is the second thing that depends on it.
    wire at_boundary = (ts_t == {VAL_W{1'b0}}) && (ts_t != ts_t_q);

    //--- the read path --------------------------------------------------------

    wire [1:0] st_other  = playing_q ? st0_q : st1_q;

    // The first pass plays bank 0; later ones play the other bank, and only if it
    // is complete -- otherwise this is an underrun and the current bank repeats.
    // The bank to hand in at the next boundary. `ready` (LEAD banks built)
    // guarantees both banks are complete before the first pass is admitted, so
    // there is NO special case for the first pass here -- and there must not be:
    // an earlier revision initialised `started_q` at the *first* boundary, which
    // consumed the first wrap without switching and left the second pass
    // repeating bank 0.
    wire       do_switch = (st_other == ST_COMPLETE);
    wire       next_bank = ~playing_q;
    wire       read_bank = (at_boundary && do_switch) ? next_bank : playing_q;
    wire [1:0] st_read   = read_bank ? st1_q : st0_q;

    wire [AW:0] rd_addr = {read_bank, ts_t[AW-1:0]};

    // An unreadable bank reads as the identity rather than as x. That is not a
    // licence: a bank in EMPTY is never admitted to a pass (`ready` gates START),
    // and the assertion below says so. It means that if the invariant is ever
    // broken the observable is a *valid but wrong* permutation rather than
    // rubbish -- which is exactly why the assertion, and not O1, is the check.
    wire readable = (st_read == ST_COMPLETE) || (st_read == ST_PLAYING);

    assign ts_pi = (readable && wmask_q[rd_addr]) ? bank_q[rd_addr] : ts_t;

    assign ready    = ready_q;
    assign underrun = underrun_q;

    //--- the fill datapath ----------------------------------------------------
    //
    // One shuffle step per accepted draw: mask `i`, draw from the generator, and
    // if the draw lands inside the range, swap entries `i` and `j`. A rejected
    // draw is *consumed* -- the counter advances either way -- which is what makes
    // the fill cost the draw count and nothing else.

    wire [31:0]      f_mask = mask_for(i_q);
    wire [31:0]      f_draw = p_out & f_mask;
    wire             f_take = (f_draw <= {{(32-VAL_W){1'b0}}, i_q});

    wire [AW:0]      f_iaddr = {fill_bank_q, i_q[AW-1:0]};
    wire [AW:0]      f_jaddr = {fill_bank_q, f_draw[AW-1:0]};

    // The implicit identity: an entry no swap has written reads as its own index.
    wire [VAL_W-1:0] f_vi = wmask_q[f_iaddr] ? bank_q[f_iaddr] : i_q;
    wire [VAL_W-1:0] f_vj = wmask_q[f_jaddr] ? bank_q[f_jaddr]
                                            : f_draw[VAL_W-1:0];

    // Setting a bit in a packed mask is ONE assignment, not two: a shift per
    // address, ORed in together. Two non-blocking assignments to the same vector
    // at different bits would fight and the last would win, which is why the mask
    // is a vector and the writes are a single expression.
    wire [DEPTH-1:0] f_m_i  = {{(DEPTH-1){1'b0}}, 1'b1} << f_iaddr;
    wire [DEPTH-1:0] f_m_j  = {{(DEPTH-1){1'b0}}, 1'b1} << f_jaddr;
    // The clear mask for the bank being filled, and the halves are not
    // interchangeable: bank 0's entries are addresses 0 .. BANK_N_MAX-1, so bank
    // 0 is the LOW half. Getting this backwards clears the bank that is about to
    // be read -- which reads as its own index thereafter, i.e. as an identity
    // traversal, and no check but this one would say so.
    wire [DEPTH-1:0] f_bclr = f_empty_bank
                            ? {{BANK_N_MAX{1'b1}}, {BANK_N_MAX{1'b0}}}   // bank 1: high
                            : {{BANK_N_MAX{1'b0}}, {BANK_N_MAX{1'b1}}};  // bank 0: low

    wire [0:0] f_empty_bank = (st0_q == ST_EMPTY) ? 1'b0 : 1'b1;
    wire       f_any_empty  = (st0_q == ST_EMPTY) || (st1_q == ST_EMPTY);

    // The geometry test is done at 32 bits, the width the generator and the
    // parameter share, rather than truncating either side to VAL_W.
    wire [31:0] n32   = {{(32-VAL_W){1'b0}}, N};
    wire [31:0] bnm32 = BANK_N_MAX;

    wire f_can_start = f_any_empty && (n32 != 32'd0) && (n32 <= bnm32) &&
                       !fill_busy_q;

    // The banks-built counter, at the width its comparisons are made in.
    wire [31:0] built32 = {{30{1'b0}}, built_q};

    //--- the rising edge ------------------------------------------------------

    always @(posedge clk) begin
        if (rst) begin
            // A reset is equivalent to a load with the seed currently held, so the
            // stream restarts at the seed rather than at counter zero. The
            // difference only shows when `seed` is not zero, which is exactly what
            // the corpus's reset run exercises: seed = 0x1234, and the first bank
            // after the reset must be bank 0 of that stream again.
            pstate_q    <= seed;
            st0_q       <= ST_EMPTY;
            st1_q       <= ST_EMPTY;
            playing_q   <= 1'b0;
            fill_busy_q <= 1'b0;
            fill_bank_q <= 1'b0;
            i_q         <= {VAL_W{1'b0}};
            built_q     <= 2'd0;
            ready_q     <= 1'b0;
            underrun_q  <= 1'b0;
            ts_t_q      <= {VAL_W{1'b0}};
            // The whole mask is cleared here as well as at each fill start. It is
            // not strictly needed -- a bank is only readable in COMPLETE or
            // PLAYING, and both are reached through a fill that cleared it -- but
            // it costs one cycle and it means no entry of the mask is ever X.
            wmask_q     <= {DEPTH{1'b0}};
        end else if (load) begin
            // A new seed means a new stream: the counter restarts and both banks
            // are emptied. Readiness clears with them and latches again once LEAD
            // banks exist (section 5.4).
            pstate_q    <= seed;
            st0_q       <= ST_EMPTY;
            st1_q       <= ST_EMPTY;
            playing_q   <= 1'b0;
            fill_busy_q <= 1'b0;
            fill_bank_q <= 1'b0;
            i_q         <= {VAL_W{1'b0}};
            built_q     <= 2'd0;
            ready_q     <= 1'b0;
            underrun_q  <= 1'b0;
            // A re-seed restarts the *pass* order as well as the stream, and the
            // detector's job is to see a wrap -- which cannot span a re-seed. It
            // must be cleared here for the same reason the reset clears it, or a
            // `ts_t` left non-zero by the caller turns the next pass's ask for
            // step 0 into a spurious boundary and hands in the wrong bank.
            ts_t_q      <= {VAL_W{1'b0}};
            wmask_q     <= {DEPTH{1'b0}};
        end else begin
            ts_t_q <= ts_t;

            //--- the fill -----------------------------------------------------
            if (!fill_busy_q) begin
                if (f_can_start) begin
                    fill_busy_q <= 1'b1;
                    fill_bank_q <= f_empty_bank;
                    i_q         <= N - {{(VAL_W-1){1'b0}}, 1'b1};    // N-1 down to 1
                    // The identity is implicit: every entry of the bank is
                    // marked unwritten in ONE cycle, in parallel, and an entry no
                    // swap touches then reads as its own index. BLOCKING
                    // assignment, deliberately: Verilator refuses a non-blocking
                    // write to an array inside a loop (BLKLOOPINIT), and nothing in
                    // this block reads the mask, so there is no ordering hazard
                    // between this and the swap writes below.
                    wmask_q <= wmask_q & ~f_bclr;
                end
            end else if (i_q == {VAL_W{1'b0}}) begin
                // N = 1. The shuffle has no steps at all and `permuted_order(1,
                // seed)` consumes no draws, so the bank is the identity, complete.
                fill_busy_q <= 1'b0;
                if (fill_bank_q) st1_q <= ST_COMPLETE;
                else             st0_q <= ST_COMPLETE;
                if ((built32 + 32'd1) >= LEAD) ready_q <= 1'b1;
                built_q <= (built32 >= LEAD) ? 2'd2 : (built_q + 2'd1);
            end else begin
                pstate_q <= p_count;             // consumed, taken or not
                if (f_take) begin
                    // The swap: two writes, in this cycle, to two entries. When
                    // j == i they name the same entry, and the second is skipped.
                    bank_q[f_iaddr] <= f_vj;
                    if (f_jaddr != f_iaddr) bank_q[f_jaddr] <= f_vi;
                    wmask_q <= wmask_q | f_m_i | f_m_j;
                    if (i_q == {{(VAL_W-1){1'b0}}, 1'b1}) begin
                        fill_busy_q <= 1'b0;     // i = 1 was the last step
                        if (fill_bank_q) st1_q <= ST_COMPLETE;
                        else             st0_q <= ST_COMPLETE;
                        if ((built32 + 32'd1) >= LEAD) ready_q <= 1'b1;
                        built_q <= (built32 >= LEAD) ? 2'd2 : (built_q + 2'd1);
                    end else begin
                        i_q <= i_q - {{(VAL_W-1){1'b0}}, 1'b1};
                    end
                end
            end

            //--- the pass boundary --------------------------------------------
            //
            // The ask for step 0 *is* the switch point: the engine latches the
            // answer for its first visit on this cycle, so the bank for this cycle
            // must be the new one combinationally -- which is why `read_bank` above
            // is not simply `playing_q`.
            if (at_boundary) begin
                if (do_switch) begin
                    if (playing_q) begin         // hand the other bank in
                        st1_q <= ST_EMPTY;       // the freed one
                        st0_q <= ST_PLAYING;
                    end else begin
                        st0_q <= ST_EMPTY;
                        st1_q <= ST_PLAYING;
                    end
                    playing_q  <= ~playing_q;
                    underrun_q <= 1'b0;          // this boundary found its bank
                end else begin
                    underrun_q <= 1'b1;          // repeat this bank, and say so
                end
            end
        end
    end

    //--- preconditions and the invariant that O1 cannot see -------------------
    //
    // The bank-isolation check below is the whole reason this module can claim
    // section 7.3's rule. A partial in-place shuffle is still a permutation, so O1
    // cannot detect a fill writing the bank being read; the observable would be a
    // valid but wrong traversal. This assertion is therefore not a nicety, it is
    // the verification mechanism for a property deliberately outside the
    // mathematical observable -- and it is an assertion rather than a debug port,
    // because a debug port would change the interface to make a test easier.

`ifndef SYNTHESIS
    initial begin
        if (VAL_W < 1) begin
            $display("bcmc_src_shuffled: ERROR VAL_W = %0d (VAL_W >= 1 required)",
                     VAL_W);
            $stop;
        end
        if (BANK_N_MAX < 2) begin
            $display("bcmc_src_shuffled: ERROR BANK_N_MAX = %0d (>= 2 required)",
                     BANK_N_MAX);
            $stop;
        end
        if (BANK_N_MAX != (1 << AW)) begin
            $display("bcmc_src_shuffled: ERROR BANK_N_MAX = %0d is not a power of",
                     BANK_N_MAX);
            $display("  two, so {bank, index} is not an address");
            $stop;
        end
        if ((LEAD < 1) || (LEAD > 2)) begin
            // There are two banks. A larger LEAD could never be satisfied, so
            // `ready` would never latch and every START would be refused --
            // silently, which is why it is checked.
            $display("bcmc_src_shuffled: ERROR LEAD = %0d (1 or 2 required: two",
                     LEAD);
            $display("  banks exist, so LEAD > 2 can never be satisfied");
            $stop;
        end
    end

    always @(posedge clk) begin
        if (!rst) begin
            // THE INVARIANT, in two forms, because they catch different mistakes.
            //
            // (1) The bank the fill is working on must not be a bank whose content
            // the read path is using. Gated on `readable`: a bank in EMPTY is not
            // yet worth protecting, and one in COMPLETE or PLAYING is.
            if (fill_busy_q && readable && (fill_bank_q == read_bank)) begin
                $display("bcmc_src_shuffled: ERROR filling bank %0d while the read",
                         fill_bank_q);
                $display("  path is using it (section 5.5: not isolated)");
                $stop;
            end
            // (2) The *address* being written must be in the fill's own bank. A
            // module that computed the selector correctly but addressed the other
            // bank would pass (1) and corrupt the traversal anyway -- which is what
            // scripts/mutate_bank_isolation.sh builds, precisely to check that this
            // second form earns its place.
            if (fill_busy_q && f_take && readable &&
                ((f_iaddr[AW] == read_bank) || (f_jaddr[AW] == read_bank))) begin
                $display("bcmc_src_shuffled: ERROR a shuffle write addresses bank %0d",
                         read_bank);
                $display("  which the read path is using");
                $stop;
            end
            // The bank about to play must be whole. In a correct build `ready` gates
            // START and this cannot fire; if it does fire, the observable without it
            // would be the identity rather than an error.
            if (at_boundary && do_switch) begin
                if ((next_bank ? st1_q : st0_q) != ST_COMPLETE) begin
                    $display("bcmc_src_shuffled: ERROR handing in bank %0d, which",
                             next_bank);
                    $display("  is not complete (SEED_READY did not gate START)");
                    $stop;
                end
            end
            // While filling, the shuffle index is a real index.
            if (fill_busy_q && (i_q > (N - {{(VAL_W-1){1'b0}}, 1'b1}))) begin
                $display("bcmc_src_shuffled: ERROR shuffle index %0d > N-1 = %0d",
                         i_q, N - {{(VAL_W-1){1'b0}}, 1'b1});
                $stop;
            end
            // The clear must cover the bank being FILLED and must not touch the
            // bank being READ. The two halves of the mask are easy to mix up, and
            // the only symptom is that the traversal silently becomes the identity
            // -- which is what the corpus reported, with no other clue. This was
            // written after getting it wrong, so it checks the one bit of each half
            // rather than trusting the reading of a replication.
            if (f_can_start) begin
                if (f_bclr[f_empty_bank ? BANK_N_MAX : 0] !== 1'b1) begin
                    $display("bcmc_src_shuffled: ERROR the fill clear does not cover");
                    $display("  bank %0d, which is the one being filled", f_empty_bank);
                    $stop;
                end
                if (readable && (f_bclr[read_bank ? BANK_N_MAX : 0] !== 1'b0)) begin
                    $display("bcmc_src_shuffled: ERROR the fill clear would wipe bank");
                    $display("  %0d, which the read path is using", read_bank);
                    $stop;
                end
            end
        end
    end
`endif

endmodule

`default_nettype wire


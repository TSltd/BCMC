//===========================================================================
// bcmc_src_affine.v -- the affine traversal source (v2.0c)
//
//     (N, seed, ts_t)  ->  ts_pi = (a*ts_t + b) mod N,  gcd(a, N) = 1
//
// A source behind the seam of rtl/bcmc_observer.v: the engine asks which step t
// it is about to take, and this module answers with the column that step visits.
// It holds no BCMC mathematics and no cursor; it holds (a, b), the last answer it
// gave, and the machinery that derives (a, b) from a seed.
//
// No multiplier, and no divider
// -----------------------------
// pi(t) is never computed from scratch, because (a * t) mod N would need a real
// reduction and this project has no divider anywhere ("The Wrap is not a
// Division", rtl/bcmc_cell.v). Instead the step is incremental:
//
//     pi(0)   = b
//     pi(t+1) = (pi(t) + a) mod N
//
// so the sequence is one adder, one comparator and one conditional subtract --
// exactly the Cell's wrap. r_q holds pi at the step last asked about, and ts_pi
// is r_q + a reduced, or b when the answer is about step 0.
//
// Why step 0 is answered from b rather than derived
// -------------------------------------------------
// A pass restarts at step 0, and a continuous pass wraps back to step 0. Neither
// is "the previous step plus a". Answering step 0 directly is what makes a
// restart correct at *any* point: the accumulator may hold a value from the
// middle of a dead pass, and it does not matter. A derived answer would be right
// the first time through and wrong on every restart -- the case
// validation/traversal_sources.py's `incremental_survives_abort` exists to catch.
//
// The seam, and the one thing the engine owes it
// ---------------------------------------------
// The answer is combinational and must be, because the engine latches it on the
// very cycle it advances. That leaves the accumulator with a problem no
// continuous-time design has: it must advance when the engine takes a step, and
// **hold when it does not**. Advancing it every clock instead makes it walk
// forward on every cycle without a trigger, which desynchronises it at the first
// gap between visits -- the common case, not a corner.
//
// On a two-wire seam with no `ts_ack` the only enable available is a change in
// `ts_t` itself:
//
//     ts_t changed  ->  a step was taken       ->  advance the accumulator
//     ts_t held     ->  the same step is asked ->  answer it again unchanged
//
// That is `moved` below, and it is why `cur_q` and `ts_t_q` exist. **It relies on
// `ts_t` being 0 while the engine is IDLE.** With `ts_t = t_next` during IDLE a
// restart would ask about step 1 while the cursor is at step 0, and the first
// visit of the restart would take the wrong column. Section 8.2 writes
// `assign ts_t = t_next;`, and that is not sufficient as written: it must be
// `assign ts_t = (state_q == STATE_IDLE) ? 0 : t_next;`. Both conditions were
// checked as negative controls before this file was written -- removing either
// produces wrong columns, on the first visit for the IDLE rule and at the first
// gap for the detector.
//
// The width, which is the trap
// ----------------------------
//     wire [VAL_W:0] inc = r_q + a;                        // VAL_W + 1 bits
//     wire [VAL_W-1:0] nxt = (inc >= N) ? inc - N : inc;   // the wrap, again
//
// `inc` is VAL_W + 1 bits and must be. With r_q <= N-1 and a <= N-1 the sum can
// reach 2N - 2, which overflows VAL_W bits for any N above 2^(VAL_W-1). Reducing
// in VAL_W bits is correct for every small N -- that is, for every N a testbench
// tries first -- and wrong above the boundary. No permitted testbench size
// catches it, which is why it is written down here and asserted below.
//
// The precondition `a < N`, which is not optional
// ----------------------------------------------
// One conditional subtract reduces a sum below N only if the sum was below 2N,
// which requires a < N. Section 4.3 of the specification always yields a <= N-1,
// so the construction satisfies it -- but the assertion below makes that a
// checked property rather than an assumption, because a step carried over from a
// larger N satisfies every type in this file and walks the wrong permutation.
//
// Ready, and why readiness is not instant
// ---------------------------------------
// (a, b) are *derived from the seed* and are not available immediately: the
// rejection draws and the gcd take as many cycles as they take. `ready` is
// therefore a real handshake, clear until they exist. Section 4.4 promises no
// cycle count -- the number of draws is a random variable -- so software polls
// SEED_READY rather than waiting a fixed number of cycles.
//
// That is also why this module's latency is deliberately *not* required to match
// validation/traversal_sources.py's `latency` estimate cycle for cycle: no cycle
// count is in the contract, so the harness treats `ready` as a handshake and
// compares the *answers*, not the schedule.
//
// Readiness is cleared by anything that can invalidate (a, b)
// ----------------------------------------------------------
// A `load` pulse -- a write to OBS_SEED or to the selector -- re-derives from the
// new seed. **A change in `N` does too**, and that case is easy to miss: gcd(a, N)
// depends on N, and N is *not* in this module's register window. It arrives from
// the core over the observation sideband, so a core write that shortens the row
// can leave a_q sharing a factor with the new N -- making the traversal a
// non-bijection while every register read still looks perfectly correct. The
// source therefore watches N itself rather than trusting a writer to pulse
// `load`. (Section 4.4 lists seed and source writes; N belongs on that list, and
// the specification is owed the sentence.)
//
// Generic Verilog-2005. Nothing here is specific to any FPGA family.
//===========================================================================

`default_nettype none

module bcmc_src_affine #(
    // N, the step index and the column index all fit in VAL_W bits.
    parameter VAL_W = 16
) (
    input  wire                clk,
    input  wire                rst,       // synchronous, active high
    input  wire [VAL_W-1:0]    N,         // row length, N >= 1
    input  wire [31:0]         seed,      // OBS_SEED
    input  wire                load,      // one-cycle: re-derive (a, b) from seed
    input  wire [VAL_W-1:0]    ts_t,      // the step the engine is asking about
    output wire [VAL_W-1:0]    ts_pi,     // pi(ts_t), combinational
    output wire                ready,     // SEED_READY: (a, b) exist and are valid
    output wire [VAL_W-1:0]    a_out,     // OBS_A
    output wire [VAL_W-1:0]    b_out      // OBS_B
);

    //-----------------------------------------------------------------------
    // The pinned generator (docs/Observers.md, "The random source")
    //
    // SplitMix32, exactly: the constants, the shifts and the order of operations
    // are part of the specification, because the traversal is only reproducible
    // if its random source is. This is the same generator observers.SplitMix32
    // implements in Python and sim/vectors/observer_prng.txt pins.
    //-----------------------------------------------------------------------

    localparam [31:0] GOLDEN = 32'h9E3779B9;
    localparam [31:0] MUL1   = 32'h21F0AAAD;
    localparam [31:0] MUL2   = 32'h735A2D97;

    reg  [31:0] pstate_q;      // the *counter*. Not the output: see below.

    // SplitMix32 keeps the counter as its state, and derives each output by
    // mixing the counter after incrementing it. The register therefore advances
    // to `p_count`, never to `p_next`.
    //
    // This module assigned `p_next` back into the state until the bench caught
    // it, and the failure is worth recording because it is invisible to every
    // check except this one: the first draw from a given seed is *correct*
    // either way, so a derivation that accepts its first draw agrees with the
    // reference, and agreement then decays one draw at a time. It reproduced
    // 8/8 of the observed wrong pairs exactly.
    wire [31:0] p_count = pstate_q + GOLDEN;               // next counter value
    wire [31:0] p_z2    = (p_count ^ (p_count >> 16)) * MUL1;
    wire [31:0] p_z3    = (p_z2 ^ (p_z2 >> 15)) * MUL2;
    wire [31:0] p_out   = p_z3 ^ (p_z3 >> 15);             // this draw's output

    // `p_out` is left at its full 32 bits and the *mask* is the same width, so
    // the draw and the range check happen at the generator's own width and every
    // bit of it is read. Narrowing `p_out` to VAL_W here would be honest about
    // what is consumed but would leave the discarded bits unused, and the two
    // ways to silence that -- a lint waiver, or a part-select tuned to VAL_W --
    // are both worse than keeping the widths natural.

    //-----------------------------------------------------------------------
    // Registers
    //
    // a_q, b_q  the derived constants -- the only state the seam reads
    // r_q       pi at the step last asked about
    // n_q       N as of the last derivation, so a change in N can be seen
    // mask_q    the rejection mask for the draw in progress
    // state_q   which phase of the derivation is running
    //-----------------------------------------------------------------------

    localparam [2:0] S_IDLE   = 3'd0,   // ready; waiting for load or a new N
                     S_MASK_A = 3'd1,   // build the mask for uniform(N-1)
                     S_DRAW_A = 3'd2,   // draw a; retry until gcd(a, N) = 1
                     S_G_STEP = 3'd3,   // one subtraction step of the Euclid
                     S_G_DONE = 3'd4,   // coprime? then draw b, else redraw a
                     S_MASK_B = 3'd5,
                     S_DRAW_B = 3'd6;

    reg [2:0]       state_q;
    reg             ready_q;
    reg [31:0]      mask_q;
    reg [VAL_W-1:0] a_q;
    reg [VAL_W-1:0] b_q;
    reg [VAL_W-1:0] eu_q;
    reg [VAL_W-1:0] ev_q;
    reg [VAL_W-1:0] n_q;
    reg [VAL_W-1:0] cur_q;      // pi(ts_t) as of the last cycle
    reg [VAL_W-1:0] ts_t_q;     // ts_t as of the last cycle -- see the seam

    wire [31:0] m = {{(32-VAL_W){1'b0}}, N} - 32'd1;   // uniform(N-1)

    // A draw is the next generator output masked to the current width, accepted
    // when it lands inside the range. A rejected draw is *consumed*: the state
    // advances either way, which is what section 4.3 requires, and what drawing
    // `b` from the same advancing stream depends on.
    wire [31:0] draw    = p_out & mask_q;
    wire             draw_ok = (draw <= m);
    wire             mask_ok = (mask_q >= m);

    assign ready = ready_q;
    assign a_out = a_q;
    assign b_out = b_q;

    //-----------------------------------------------------------------------
    // The seam answer: combinational, every cycle, regardless of when a visit
    // actually happens (section 2 -- a source never withholds)
    //
    // The step the engine is asking about is `ts_t`, and the answer must be
    // pi(ts_t) *in the same cycle*, because the engine latches it on the cycle it
    // advances. An accumulator cannot be advanced by its own output every clock:
    // that would walk forward on cycles where the engine does nothing, which is
    // every cycle a trigger is not asserted. So the accumulator advances only
    // when `ts_t` has actually moved, and holds otherwise:
    //
    //     ts_t changed  ->  the engine took a step  ->  advance cur_q
    //     ts_t held     ->  asking the same question  ->  answer cur_q again
    //
    // `ts_t != ts_t_q` is therefore the enable, and it is the *only* enable
    // available on a two-wire seam with no `ts_ack`. (Checked: without this
    // detector a delayed trigger desynchronises the accumulator on the first
    // gap; with it, nothing does.)
    //
    // This relies on one thing from the engine, and it is not optional: **`ts_t`
    // must be 0 while the engine is IDLE**, so that a pass beginning -- or
    // beginning again after a completed one -- asks about step 0 and reloads
    // cur_q from b_q. With `ts_t = t_next` during IDLE the first visit of a
    // restart takes the wrong column. See the module header; section 8.2's
    // `assign ts_t = t_next;` is not sufficient as written.
    //-----------------------------------------------------------------------

    wire [VAL_W:0]   inc   = cur_q + a_q;                 // VAL_W + 1 bits
    wire [VAL_W-1:0] nxt   =
        (inc >= {1'b0, N}) ? inc[VAL_W-1:0] - N : inc[VAL_W-1:0];
    wire             moved = (ts_t != ts_t_q);

    assign ts_pi = (ts_t == {VAL_W{1'b0}}) ? b_q : (moved ? nxt : cur_q);

    //-----------------------------------------------------------------------
    // Anything that can invalidate (a, b): a seed write, the selector, or a
    // change in N. See the header for why N belongs in this list even though the
    // specification's section 4.4 lists only writes to this window.
    //-----------------------------------------------------------------------

    wire restart = load || (N != n_q);

    //-----------------------------------------------------------------------
    // The rising edge
    //
    // Two independent things happen here, and conflating them is how this module
    // would go wrong. `cur_q`/`ts_t_q` track the *seam*, one cycle behind, and are
    // updated unconditionally every clock. The derivation FSM runs only while
    // (a, b) do not exist.
    //-----------------------------------------------------------------------

    always @(posedge clk) begin
        if (rst) begin
            state_q  <= S_IDLE;
            ready_q  <= 1'b0;
            pstate_q <= 32'd0;
            a_q      <= {VAL_W{1'b0}};
            b_q      <= {VAL_W{1'b0}};
            cur_q    <= {VAL_W{1'b0}};
            ts_t_q   <= {VAL_W{1'b0}};
            n_q      <= {VAL_W{1'b0}};
            mask_q   <= 32'd1;
            eu_q     <= {VAL_W{1'b0}};
            ev_q     <= {VAL_W{1'b0}};
        end else begin
            // The seam tracking: unconditional, and unrelated to the derivation.
            cur_q  <= ts_pi;
            ts_t_q <= ts_t;

            if (restart) begin
                // (a, b) are void the moment the seed, the selector or N moves.
                // Readiness clears in the same cycle, so E4's `& SEED_READY`
                // refuses a start while the old (a, b) are still in the
                // registers -- there is no window in which a stale pair is used.
                ready_q  <= 1'b0;
                n_q      <= N;
                pstate_q <= seed;
                mask_q   <= 32'd1;
                state_q  <= S_MASK_A;
            end else begin
                case (state_q)
                    // Ready, and staying ready until something invalidates.
                    S_IDLE: ready_q <= 1'b1;

                    // mask <- the smallest 2^k - 1 that is >= m. Nought
                    // iterations when m <= 1, which is what uniform(0) wants.
                    S_MASK_A: begin
                        if (mask_ok) state_q <= S_DRAW_A;
                        else mask_q <= (mask_q << 1) | 32'd1;
                    end

                    S_DRAW_A: begin
                        if (m == 32'd0) begin
                            // uniform(0) is 0 and consumes nothing -- the pinned
                            // generator says so, and N = 1 must agree with it.
                            a_q     <= {VAL_W{1'b0}};
                            eu_q    <= {VAL_W{1'b0}};
                            ev_q    <= N;
                            state_q <= S_G_STEP;
                        end else begin
                            pstate_q <= p_count; // the COUNTER, not p_next
                            if (draw_ok) begin
                                a_q     <= draw[VAL_W-1:0];
                                eu_q    <= draw[VAL_W-1:0];
                                ev_q    <= N;
                                state_q <= S_G_STEP;
                            end
                        end
                    end

                    // Euclid by sequential subtraction, one step per cycle. It is
                    // allowed to take cycles because it runs once per seed; the
                    // per-visit path is the combinational answer above.
                    S_G_STEP: begin
                        if (ev_q == {VAL_W{1'b0}}) state_q <= S_G_DONE;
                        else if (eu_q < ev_q) begin
                            eu_q <= ev_q;        // a swap: both from old values
                            ev_q <= eu_q;
                        end else begin
                            eu_q <= eu_q - ev_q;
                        end
                    end

                    S_G_DONE: begin
                        mask_q <= 32'd1;
                        if (eu_q == {{(VAL_W-1){1'b0}}, 1'b1}) begin
                            state_q <= S_MASK_B;    // coprime: draw b next
                        end else begin
                            state_q <= S_MASK_A;    // redraw a, nothing rewound
                        end
                    end

                    S_MASK_B: begin
                        if (mask_ok) state_q <= S_DRAW_B;
                        else mask_q <= (mask_q << 1) | 32'd1;
                    end

                    S_DRAW_B: begin
                        if (m == 32'd0) begin
                            b_q     <= {VAL_W{1'b0}};
                            state_q <= S_IDLE;
                        end else begin
                            pstate_q <= p_count; // consumed, taken or not
                            if (draw_ok) begin
                                b_q     <= draw[VAL_W-1:0];
                                state_q <= S_IDLE;
                            end
                        end
                    end

                    default: state_q <= S_IDLE;
                endcase
            end
        end
    end

    //-----------------------------------------------------------------------
    // Preconditions and local invariants
    //
    // The timing contract is deliberately not among them: it is checked by
    // comparing every cycle against validation/traversal_sources.py and by the
    // engine's own harness. These restate nothing the FSM decides; they make the
    // two things a reader would otherwise have to hold in their head checkable.
    //-----------------------------------------------------------------------

`ifndef SYNTHESIS
    initial begin
        if (VAL_W > 32) begin
            $display("bcmc_src_affine: ERROR VAL_W = %0d (VAL_W <= 32)", VAL_W);
            $stop;
        end
        if (VAL_W < 1) begin
            $display("bcmc_src_affine: ERROR VAL_W = %0d (VAL_W >= 1 required)", VAL_W);
            $stop;
        end
    end

    always @(posedge clk) begin
        if (!rst && ready_q) begin
            // Section 4.2's precondition. One conditional subtract reduces a sum
            // below N only when a < N; a step carried over from a larger N would
            // satisfy every type in this file and walk the wrong permutation.
            if (a_q >= N) begin
                $display("bcmc_src_affine: ERROR a = %0d >= N = %0d (section 4.2)",
                         a_q, N);
                $stop;
            end
            // gcd(a, N) = 1, which for a = 0 holds only when N = 1. A source that
            // reports itself ready with a = 0 and N > 1 is not a bijection.
            if ((a_q == {VAL_W{1'b0}}) && (N != {{(VAL_W-1){1'b0}}, 1'b1})) begin
                $display("bcmc_src_affine: ERROR a = 0 with N = %0d (gcd != 1)", N);
                $stop;
            end
        end
    end
`endif

endmodule

`default_nettype wire

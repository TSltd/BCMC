//===========================================================================
// bcmc_obs_wb.v -- the observer's register window, on real wires
//
//     Wishbone B4 Classic  <->  the frozen sequential observer engine
//
// This module connects the six wires of rtl/bcmc_observer.v to a bus, and does
// nothing else. It implements docs/Observer_Register_Map.md -- seven registers,
// four error conditions, one-cycle ack/err -- and it introduces no traversal
// semantics of its own. Where a register bit does something, it reaches an
// engine input; where it reports something, it reads an engine output.
//
//     bcmc_observer.v        instantiated here, UNCHANGED
//     the trigger-source mux instantiated here, with one input populated
//
// What this module is careful about
// ---------------------------------
// **Bus transaction state and engine state are kept apart.** This module owns
// registers and one-cycle pulses: the ONESHOT mode bit, the DONE and ABORTED
// latches, the pass counter, and this cycle's accepted request. It does NOT own
// RUNNING, the cursor, the visit sequence or completion -- those are the
// engine's, and they are read out of it. A wrapper that kept its own copy would
// be a second specification of one state machine, and the two would drift.
//
// The consequence is testable, and is the differential test's whole basis: from
// the bus sequence the Python model produces an *engine input trace*, and this
// module must produce the same trace while separately satisfying the Wishbone
// timing contract. If it ever disagreed, the first question is whether section 6
// of the register map is being implemented incorrectly -- not whether
// bcmc_observer.v needs something new.
//
// The bus boundary
// ----------------
// This module IS the slave (section 3.1). wb_adr_i is a 12-bit byte address
// *within* this module's 4 KiB region: the interconnect decodes the region and
// the address width is the check, exactly as in rtl/bcmc_wb.v. "Outside the
// region" is therefore not a condition this block can observe, and everything
// inside the region that is unmapped is E1.
//
// Why E4 is derived rather than declared
// --------------------------------------
// The engine *ignores* a start it cannot accept and a trigger in the wrong
// state. That silence is right for a wire and wrong for a bus, so it becomes an
// error here -- by the engine's own acceptance condition, asked of the engine:
//
//     START refused unless  !RUNNING & VALID & N >= 1
//     STEP  refused unless   RUNNING
//
// What is deliberately absent
// ---------------------------
// No EN (the engine has no enable), no readable column (it would invite a second,
// polled traversal path underneath the hardware one), no NEXT_COLUMN. The
// register-map review rejected all three and the adversarial suite found nothing
// that requires them.
//
// Generic Verilog-2005. Nothing here is specific to any FPGA family.
//===========================================================================

`default_nettype none

module bcmc_obs_wb #(
    parameter VAL_W = 16,     // N and a column index
    parameter IDX_W = 16,     // C and a row index
    parameter MAX_C = 32      // rows this observer can evaluate
) (
    //--- Wishbone B4 Classic, slave, 32-bit, region-local -------------------
    input  wire                   wb_clk_i,
    input  wire                   wb_rst_i,     // synchronous, active high
    input  wire [11:0]            wb_adr_i,     // byte address, 0x000 .. 0xFFF
    // Bits 31:4 of a write are reserved and ignored (section 5), so this module
    // genuinely reads only the four bits OBS_CTRL defines. That is the
    // document's doing, not an oversight, and it is the only waiver here.
    /* verilator lint_off UNUSEDSIGNAL */
    input  wire [31:0]            wb_dat_i,
    /* verilator lint_on UNUSEDSIGNAL */
    input  wire [3:0]             wb_sel_i,
    input  wire                   wb_we_i,
    input  wire                   wb_stb_i,
    input  wire                   wb_cyc_i,
    output reg  [31:0]            wb_dat_o,
    output reg                    wb_ack_o,
    output reg                    wb_err_o,

    //--- the observation sideband, read-only, from bcmc_wb (section 4.1) ----
    input  wire [VAL_W-1:0]       obs_n_i,
    input  wire [IDX_W-1:0]       obs_c_i,
    input  wire                   obs_valid_i,
    input  wire [MAX_C*VAL_W-1:0] obs_weights_flat_i,
    input  wire [MAX_C*VAL_W-1:0] obs_offsets_flat_i,

    //--- the visit stream, out to the output engine (v2.0b) -----------------
    output wire [VAL_W-1:0]       column_o,
    output wire                   visit_valid_o,
    output wire [MAX_C-1:0]       column_bits_o,
    output wire                   done_o,
    output wire                   running_o,
    output wire                   aborted_o
);

    //-----------------------------------------------------------------------
    // The address map (section 5). Seven registers, and nothing else.
    //-----------------------------------------------------------------------

    localparam [11:0] OBS_ID      = 12'h000;
    localparam [11:0] OBS_VERSION = 12'h004;
    localparam [11:0] OBS_CAPS    = 12'h008;
    localparam [11:0] OBS_CTRL    = 12'h00C;
    localparam [11:0] OBS_STATUS  = 12'h010;
    localparam [11:0] OBS_PASS    = 12'h014;
    localparam [11:0] OBS_TRIG    = 12'h018;
    // v2.0c. These three addresses are chosen so that every address the frozen
    // v2.0a corpus probes as unmapped (0x01C, 0x040, 0x3FC) stays unmapped:
    // docs/Traversal_Sources_Specification.md section 6.4 reserves them
    // permanently for exactly that reason.
    localparam [11:0] OBS_SEED    = 12'h020;   // RW: the traversal source's seed
    localparam [11:0] OBS_A       = 12'h024;   // RO: the affine's a, 0 until ready
    localparam [11:0] OBS_B       = 12'h028;   // RO: the affine's b, 0 until ready

    // OBS_CTRL[5:4] is the source selector. 3 names no source and is refused.
    localparam CTRL_SEL_HI = 5;
    localparam CTRL_SEL_LO = 4;
    localparam [1:0] SEL_IDENTITY = 2'd0;
    localparam [1:0] SEL_AFFINE   = 2'd1;
    localparam [1:0] SEL_SHUFFLED = 2'd2;

    localparam [31:0] ID_VALUE      = 32'h4F425356;   // "OBSV"
    localparam [31:0] VERSION_VALUE = 32'h0000_0100;  // 0.1.0
    localparam [31:0] TRIG_VALUE    = 32'h0000_0001;  // software only, in v2.0a
    localparam [31:0] CAPS_VALUE    = (IDX_W << 24) | (VAL_W << 16) | MAX_C;

    //-----------------------------------------------------------------------
    // The engine's wires, and the trigger-source mux
    //
    // v2.0a populates one input of the mux and nothing else. v2.0c adds a timer
    // and a pin at this seam, and nothing in the decode below changes.
    //-----------------------------------------------------------------------

    wire eng_running;
    wire eng_aborted;
    wire eng_done;
    wire eng_visit_valid;

    wire ctrl_start;
    wire ctrl_step;
    wire ctrl_reset;

    // The window's own registers. ONESHOT is a mode bit; DONE and ABORTED are
    // latched projections of engine events; PASS counts completions.
    reg         oneshot_q;
    reg         done_q;
    reg         aborted_q;
    reg  [31:0] pass_q;
    reg         eng_aborted_q;     // for detecting the engine's abort edges

    // v2.0c: the selector, the seed, and the window's own copy of N. The copy
    // exists because N arrives over the read-only observation sideband rather
    // than as a register, so "did N change?" can only be asked of a register
    // here -- which is what finding 26's protection is built on.
    reg  [1:0]       sel_q;
    reg  [31:0]      seed_q;
    reg  [VAL_W-1:0] n_q;
    // The underrun level, latched. The level belongs to the source and the
    // stickiness lives here, which is what makes clearing it re-assert while the
    // level is still high (section 8.4 item 5).
    reg              under_lat;

    //-----------------------------------------------------------------------
    // The traversal seam (v2.0c, section 8.2)
    //
    // The engine no longer computes the traversal; it asks a source about the
    // step it is about to present. v2.0a's control case is the identity, wired
    // here as a REAL instance so the regression exercises the module rather than
    // a copy of it. Section 8.3 has this become the selector and the mux over
    // three sources -- which is precisely why the source lives here and not in
    // the engine.
    //-----------------------------------------------------------------------

    wire [VAL_W-1:0] seam_t;
    wire [VAL_W-1:0] seam_pi;
    wire             seam_ready;

    // The pulse that re-derives a source. Three events earn it and nothing else
    // does (section 8.4 item 3): an accepted write to OBS_SEED, an accepted write
    // that actually CHANGES the selector, and a change in N. The re-selection
    // term is `sel_changed`, not the field, so that re-stating the current
    // selector -- which every STEP does -- cannot discard a filled bank.
    //
    // It doubles as the readiness recovery: the cycle a load is issued is exactly
    // the one cycle for which SEED_READY is void, for every source (section 7.1).
    // Expressing it combinationally is what makes that exactly one cycle, and it
    // is the same wire the sources see.
    wire sel_changed = wr_ctrl_req & (wb_dat_i[CTRL_SEL_HI:CTRL_SEL_LO] != sel_q);
    wire n_changed   = (obs_n_i != n_q);

    // `N < 1` is outside every source's domain (section 3.4), and the model's
    // answer -- which this mirrors -- is that no source exists at all. In
    // hardware the instances are necessarily present, so they are HELD RESET and
    // readiness is forced low instead. Suppressing `load` here would not be
    // enough: bcmc_src_affine re-derives on `N != n_q` by itself, so it has to be
    // the reset that keeps it out of an illegal `N = 0` state.
    wire no_source   = (obs_n_i == {VAL_W{1'b0}});

    wire load_seed   = wr_seed_ok & accept;
    wire load_sel    = wr_ctrl_ok & accept & sel_changed;
    wire load_all    = (load_seed | load_sel | n_changed) & ~no_source;

    // TWO recovery terms, and the difference is not cosmetic.
    //
    // `load_all` is what SEED_READY reports, and it includes the selector change
    // this very write is making: a driver reading STATUS in the cycle it
    // re-selects must see readiness void, because it is.
    //
    // `recovery_start` is what START's admission is judged against, and it must
    // NOT include this write's own selector change. Otherwise the logic is
    // circular -- is the write accepted? depends on whether the selector changed;
    // does that void readiness? depends on whether the write is accepted -- and
    // the loop is real, not an artefact: a CTRL write carrying both a new
    // selector and START would oscillate rather than resolve.
    //
    // The resolution follows the model, which is what the v2.0c corpus records:
    // `ObserverPeriphV2C.write` asks `start_acceptable()` BEFORE it applies the
    // selector change, so such a write is judged on its pre-change readiness and,
    // if accepted, both the start and the load happen. `recovery_start` is that
    // reading, expressed in hardware.
    wire recovery_start = load_seed | n_changed;

    // All three sources are instantiated and clocked whether selected or not, so
    // an unselected source keeps its own state -- which is why a pass on one does
    // not disturb a fill in another. Only ts_pi and ready are muxed.
    wire [VAL_W-1:0] pi_identity;
    wire [VAL_W-1:0] pi_affine;
    wire [VAL_W-1:0] pi_shuffled;
    wire             rdy_affine;
    wire             rdy_shuffled;
    wire             under_shuffled;
    wire [VAL_W-1:0] aff_a;
    wire [VAL_W-1:0] aff_b;

    bcmc_src_identity #(
        .VAL_W (VAL_W)
    ) u_src_identity (
        .ts_t  (seam_t),
        .ts_pi (pi_identity)
    );

    bcmc_src_affine #(
        .VAL_W (VAL_W)
    ) u_src_affine (
        .clk    (wb_clk_i),
        .rst    (wb_rst_i | no_source),
        .N      (obs_n_i),
        .seed   (seed_q),
        .load   (load_all),
        .ts_t   (seam_t),
        .ts_pi  (pi_affine),
        .ready  (rdy_affine),
        .a_out  (aff_a),
        .b_out  (aff_b)
    );

    bcmc_src_shuffled #(
        .VAL_W (VAL_W)
    ) u_src_shuffled (
        .clk      (wb_clk_i),
        .rst      (wb_rst_i | no_source),
        .N        (obs_n_i),
        .seed     (seed_q),
        .load     (load_all),
        .ts_t     (seam_t),
        .ts_pi    (pi_shuffled),
        .ready    (rdy_shuffled),
        .underrun (under_shuffled)
    );

    // The identity is a function of its argument, so it cannot be late: its
    // contribution to the AND is a constant 1 (section 7.1, which is why
    // rtl/bcmc_src_identity.v has no ready port). The window's uniform one-cycle
    // recovery is the `~load_all` term below, and it applies to every source.
    wire             rdy_identity   = 1'b1;
    wire [VAL_W-1:0] selected_pi    = (sel_q == SEL_AFFINE)   ? pi_affine
                                    : (sel_q == SEL_SHUFFLED) ? pi_shuffled
                                    :                           pi_identity;
    wire             selected_ready = (sel_q == SEL_AFFINE)   ? rdy_affine
                                    : (sel_q == SEL_SHUFFLED) ? rdy_shuffled
                                    :                           rdy_identity;

    assign seam_pi    = selected_pi;
    // Readiness is forced low at `N < 1`, where no source exists -- so the
    // identity's legal-domain constant 1 is not allowed to leak into a context
    // that has no source at all. The identity's contract is unchanged; what is
    // added is the window's own answer for an unusable context (finding 29).
    assign seam_ready = ~no_source & selected_ready & ~load_all;
    // What START is admitted against. See `recovery_start` above for why this is
    // not simply seam_ready. `START` is refused at `N < 1` by this term and by
    // section 3.4's own `N >= 1`, which is belt and braces on purpose.
    wire   start_ready = ~no_source & selected_ready & ~recovery_start;

    // OBS_A/OBS_B are the affine's REGARDLESS of the selector, and zero before it
    // is ready -- which is how a driver tells "not derived yet" from a real value
    // (section 8.4 item 4).
    wire [VAL_W-1:0] obs_a = rdy_affine ? aff_a : {VAL_W{1'b0}};
    wire [VAL_W-1:0] obs_b = rdy_affine ? aff_b : {VAL_W{1'b0}};
    // Zero-extended for the read path, in one place, so the read mux stays a mux.
    wire [31:0]      obs_a32 = {{(32 - VAL_W){1'b0}}, obs_a};
    wire [31:0]      obs_b32 = {{(32 - VAL_W){1'b0}}, obs_b};

    bcmc_observer #(
        .VAL_W (VAL_W),
        .IDX_W (IDX_W),
        .MAX_C (MAX_C)
    ) u_observer (
        .clk          (wb_clk_i),
        .rst          (ctrl_reset | wb_rst_i),
        .start        (ctrl_start),
        .trigger      (ctrl_step),
        .oneshot      (oneshot_q),
        .N            (obs_n_i),
        .C            (obs_c_i),
        .valid        (obs_valid_i),
        .weights_flat (obs_weights_flat_i),
        .offsets_flat (obs_offsets_flat_i),
        .column       (column_o),
        .visit_valid  (eng_visit_valid),
        .column_bits  (column_bits_o),
        .done         (eng_done),
        .running      (eng_running),
        .aborted      (eng_aborted),
        .ts_t         (seam_t),
        .ts_pi        (seam_pi)
    );

    assign visit_valid_o = eng_visit_valid;
    assign done_o        = eng_done;
    assign running_o     = eng_running;
    assign aborted_o     = eng_aborted;

    //-----------------------------------------------------------------------
    // Decode (section 5), and the error model (section 6)
    //-----------------------------------------------------------------------

    wire        req    = wb_stb_i & wb_cyc_i;
    wire        accept = req & ~wb_ack_o & ~wb_err_o;
    wire        sel_ok = (wb_sel_i == 4'hF);
    wire [11:0] addr   = wb_adr_i;

    wire hit_id      = (addr == OBS_ID);
    wire hit_version = (addr == OBS_VERSION);
    wire hit_caps    = (addr == OBS_CAPS);
    wire hit_ctrl    = (addr == OBS_CTRL);
    wire hit_status  = (addr == OBS_STATUS);
    wire hit_pass    = (addr == OBS_PASS);
    wire hit_trig    = (addr == OBS_TRIG);
    wire hit_seed    = (addr == OBS_SEED);
    wire hit_a       = (addr == OBS_A);
    wire hit_b       = (addr == OBS_B);

    wire mapped = hit_id | hit_version | hit_caps | hit_ctrl | hit_status
                | hit_pass | hit_trig | hit_seed | hit_a | hit_b;

    // The selector field decodes like an address: 3 names no source, so a write
    // carrying it is not decoded at all (E1) rather than being something the
    // engine would ignore (E4). Section 6.5.
    wire sel_dec_ok = (wb_dat_i[CTRL_SEL_HI:CTRL_SEL_LO] != 2'd3);

    // E4, derived from the engine's acceptance condition rather than declared:
    // this START, or this STEP, is one the engine would ignore. v2.0c adds the
    // selected source's readiness to START's condition -- uniformly, for every
    // source (section 7.2) -- and refuses to re-select a source while a pass is
    // in flight, which is the same reason START is refused then.
    wire start_ok    = ~eng_running & obs_valid_i & (obs_n_i != {VAL_W{1'b0}})
                     & start_ready;
    wire wr_ctrl_req = sel_ok & hit_ctrl & wb_we_i;
    wire e4          = (wb_dat_i[0] & ~start_ok)
                     | (wb_dat_i[1] & ~eng_running)
                     | (sel_changed & eng_running);

    wire rd_ok      = sel_ok & mapped & ~wb_we_i;                 // E1, E2, E3
    wire wr_stat_ok = sel_ok & hit_status & wb_we_i;              // RW1C
    wire wr_ctrl_ok = wr_ctrl_req & ~e4 & sel_dec_ok;             // E4, E1
    wire wr_seed_ok = sel_ok & hit_seed & wb_we_i & ~eng_running; // E4
    wire wr_ok      = wr_stat_ok | wr_ctrl_ok | wr_seed_ok;

    // The one-cycle pulses the mux hands the engine, in the accepting cycle.
    assign ctrl_start = wr_ctrl_ok & accept & wb_dat_i[0];
    assign ctrl_step  = wr_ctrl_ok & accept & wb_dat_i[1];
    assign ctrl_reset = wr_ctrl_ok & accept & wb_dat_i[2];

    // Reads. ONESHOT is bit 3. STATUS is RUNNING | DONE | ABORTED, and v2.0c adds
    // SEED_READY as bit 3 -- the LEVEL, not a latch, so it follows the source --
    // and SEED_UNDERRUN as bit 4, which is latched here. CTRL also reads back the
    // selector, which is what a driver needs to tell which source it is polling.
    wire [31:0] rdata =
          hit_id      ? ID_VALUE
        : hit_version ? VERSION_VALUE
        : hit_caps    ? CAPS_VALUE
        : hit_ctrl    ? {26'b0, sel_q, oneshot_q, 3'b000}
        : hit_status  ? {27'b0, under_lat, seam_ready, aborted_q, done_q,
                         eng_running}
        : hit_pass    ? pass_q
        : hit_trig    ? TRIG_VALUE
        : hit_seed    ? seed_q
        : hit_a       ? obs_a32
        : hit_b       ? obs_b32
        :               32'd0;

    //-----------------------------------------------------------------------
    // Bus response, and the window's registers
    //
    // One cycle, one outcome; and a refused write changes nothing at all --
    // which is why every acceptance term (`rd_ok`, `wr_ok`) is evaluated
    // combinationally in the access cycle, and the side effects below are gated
    // by the same terms rather than by the response.
    //-----------------------------------------------------------------------

    always @(posedge wb_clk_i) begin
        if (wb_rst_i) begin
            wb_ack_o      <= 1'b0;
            wb_err_o      <= 1'b0;
            wb_dat_o      <= 32'd0;
            oneshot_q     <= 1'b0;
            done_q        <= 1'b0;
            aborted_q     <= 1'b0;
            pass_q        <= 32'd0;
            eng_aborted_q <= 1'b0;
            // v2.0c. n_q starts at 0 so that a valid context present at reset
            // produces a load on the first cycle, which is how the sources come
            // up already derived for the N they are being asked about.
            sel_q         <= SEL_IDENTITY;
            seed_q        <= 32'd0;
            n_q           <= {VAL_W{1'b0}};
            under_lat     <= 1'b0;
        end else begin
            wb_ack_o <= 1'b0;
            wb_err_o <= 1'b0;

            if (accept) begin
                if (rd_ok) begin
                    wb_ack_o <= 1'b1;
                    wb_dat_o <= rdata;
                end else if (wr_ok) begin
                    wb_ack_o <= 1'b1;
                    wb_dat_o <= 32'd0;
                end else begin
                    wb_err_o <= 1'b1;      // E1, E2, E3 or E4: no side effect
                end
            end

            // --- the status projections, and the engine's abort edges -------
            eng_aborted_q <= eng_aborted;
            if (ctrl_reset) begin
                // RESET clears the observer and only the observer: not ONESHOT,
                // not the context, not VALID.
                done_q    <= 1'b0;
                aborted_q <= 1'b0;
                pass_q    <= 32'd0;
            end else begin
                if (eng_done) begin
                    done_q <= 1'b1;
                    pass_q <= pass_q + 32'd1;
                end
                if (eng_aborted & ~eng_aborted_q)  aborted_q <= 1'b1;
                if (eng_aborted_q & ~eng_aborted)  aborted_q <= 1'b0;
                if (wr_stat_ok & accept) begin
                    if (wb_dat_i[1]) done_q    <= 1'b0;
                    if (wb_dat_i[2]) aborted_q <= 1'b0;
                end
            end

            // --- the mode bit, the selector, and v2.0c's registers -----------
            //
            // A refused write changes nothing at all, which is why every one of
            // these is gated by the same acceptance term the response uses --
            // and why a selector is stored only from a write that was accepted.
            if (wr_ctrl_ok & accept) begin
                oneshot_q <= wb_dat_i[3];
                sel_q     <= wb_dat_i[CTRL_SEL_HI:CTRL_SEL_LO];
            end
            if (wr_seed_ok & accept) seed_q <= wb_dat_i;

            // The window's copy of N. This is NOT a register in the map: N
            // arrives over the read-only observation sideband, so the copy is the
            // only place a change in it can be seen -- and `n_changed`, which
            // pulses `load` above, is asked of it.
            n_q <= obs_n_i;

            // SEED_UNDERRUN, set from the sources' level and cleared by writing 1.
            // The clear is asked second, so a clear and a level in the same cycle
            // resolve to the clear -- and the level then re-asserts it on the next
            // cycle, which is what a level-backed RW1C bit has to do.
            if (under_shuffled) under_lat <= 1'b1;
            if (wr_stat_ok & accept & wb_dat_i[4]) under_lat <= 1'b0;
        end
    end

    //-----------------------------------------------------------------------
    // Local invariants -- the frozen contract's own properties, made checkable
    // in simulation. The transaction behaviour is checked by replay against
    // validation/observer_periph.py, not restated here.
    //-----------------------------------------------------------------------

`ifndef SYNTHESIS
    always @(posedge wb_clk_i) begin
        if (!wb_rst_i) begin
            if (wb_ack_o && wb_err_o) begin
                $display("bcmc_obs_wb: ERROR ack and err in the same cycle");
                $stop;
            end
            // A start pulse exists only because an accepted START asked for it.
            if (ctrl_start && !(wr_ctrl_ok & accept & wb_dat_i[0])) begin
                $display("bcmc_obs_wb: ERROR start pulse without an accepted START");
                $stop;
            end
        end
    end
`endif

endmodule

`default_nettype wire

//===========================================================================
// bcmc_wb.v -- Classic Wishbone B4 slave: the BCMC peripheral
//
// This is the first module in the project whose primary job is communication
// rather than computation. It contains no BCMC mathematics of its own: every
// matrix bit it returns comes from bcmc_cell (through bcmc_column), every
// offset comes from bcmc_core, and every stored number comes from
// bcmc_context. What this file adds is exactly the four things a bus needs and
// the mathematics does not:
//
//     1. address decode, including the projection multiplexer,
//     2. the error model -- when an access is refused,
//     3. the register state: N, C, CELL_ROW, CELL_COL, CTRL, STATUS,
//     4. the sequencer that drives the Prefix Stream Interface.
//
// The authority for all of it is docs/Register_Map.md. docs/Transaction_
// Sequences.md says which access follows which. Where they disagree, the
// register map wins.
//
// The Evaluator is still not a module
// -----------------------------------
// docs/Register_Map.md: "The projection multiplexer -- the logic that decides
// whether a read wants a cell or a column -- is address decode. It is part of
// the bus adapter, because that is the only place a *bus* is mentioned."
//
// So it is here, and it is three lines: one bcmc_column instance answers
// M(i, CELL_COL) for every row at once, a COLUMN[k] read slices 32 of those
// bits out, and a CELL read selects one. The cell is not computed a second
// time by a second bcmc_cell instance, and that is deliberate: two
// implementations of one number are two things that can disagree. `CELL` is a
// bit of `COLUMN` because M(CELL_ROW, CELL_COL) *is* a bit of column
// CELL_COL. The decode chooses how many bits of the projection to return.
//
// Ownership
// ---------
// bcmc_context has no N port and no C port -- it cannot compute BCMC because
// it is not told what a BCMC problem is. Everything it therefore cannot own
// lives here: N, C, both query indices, CTRL, STATUS, the START sequencer and
// the decode. The context contributes storage and arbitration only.
//
//     +--------------------------------------------------------------+
//     |  bcmc_wb            decode | error model | registers | seq   |
//     |    +--------------+   +----------+   +------------------+    |
//     |    | bcmc_context |<->| bcmc_core|   |    bcmc_column   |    |
//     |    |  storage     |   |  prefix  |   |  MAX_C x cell    |    |
//     |    +--------------+   +----------+   +------------------+    |
//     +--------------------------------------------------------------+
//                                  ^
//                                  |  Wishbone B4 Classic
//
// Bus timing
// ----------
// One access per request, terminating in exactly one of `ack` or `err`, in the
// cycle after `cyc & stb`, and never in both and never in neither. `ack` and
// `err` are registered and one cycle wide; `dat_o` is registered alongside
// `ack`, and is not updated by an erring cycle. There are no bursts, no
// pipelining and no wait states beyond that single cycle.
//
// A read is answered in the same access as it is requested even when it is a
// matrix bit, because the Evaluator is combinational: "There is no 'start
// query, poll ready' sequence anywhere in this map, and there never will be."
//
// Generic Verilog-2005. Nothing here is specific to any FPGA family.
//===========================================================================

`default_nettype none

module bcmc_wb #(
    // Every stored value -- N, a weight, an offset, a column index -- fits in
    // VAL_W bits. 1 <= VAL_W <= 32.
    parameter VAL_W = 16,
    // Every index fits in IDX_W bits. The windows are 256 words wide, so
    // 8 <= IDX_W <= 32.
    parameter IDX_W = 16,
    // The most rows this instance can hold, 1 <= MAX_C <= 256. Reported in
    // CAPS, so software never needs a #define for it.
    parameter MAX_C = 64
) (
    //-----------------------------------------------------------------------
    // Wishbone B4 Classic, slave, 32-bit data, 32-bit granularity.
    //
    // The address is a byte address within a 4 KiB region. It is 12 bits, so
    // "outside the region" is not a condition this module can observe: the
    // interconnect decodes the region, and the address width is the check.
    // Everything *inside* the region that is not mapped is E1, below.
    //-----------------------------------------------------------------------
    input  wire        wb_clk_i,
    input  wire        wb_rst_i,     // synchronous, active high

    input  wire [11:0] wb_adr_i,     // byte address, 0x000 .. 0xFFF
    input  wire [31:0] wb_dat_i,
    input  wire [3:0]  wb_sel_i,
    input  wire        wb_we_i,
    input  wire        wb_stb_i,
    input  wire        wb_cyc_i,

    output reg  [31:0] wb_dat_o,
    output reg         wb_ack_o,
    output reg         wb_err_o,

    //-----------------------------------------------------------------------
    // Level-sensitive interrupt: IRQ & IRQ_EN. IRQ_EN gates this pin only and
    // never gates the latching of IRQ, so a polling driver and an
    // interrupt-driven driver observe the same STATUS.
    //-----------------------------------------------------------------------
    output wire        irq_o
);

    //=======================================================================
    // The address map -- docs/Register_Map.md
    //=======================================================================

    localparam [11:0] ADR_ID       = 12'h000;
    localparam [11:0] ADR_VERSION  = 12'h004;
    localparam [11:0] ADR_CAPS     = 12'h008;
    localparam [11:0] ADR_CTRL     = 12'h00C;
    localparam [11:0] ADR_STATUS   = 12'h010;
    localparam [11:0] ADR_N        = 12'h014;
    localparam [11:0] ADR_C        = 12'h018;
    localparam [11:0] ADR_CELL_ROW = 12'h01C;
    localparam [11:0] ADR_CELL_COL = 12'h020;
    localparam [11:0] ADR_CELL     = 12'h024;

    // The COLUMN span is 32 bytes: eight words, of which the first
    // ceil(MAX_C/32) exist and the rest are E1.
    localparam [11:0] COLUMN_BASE  = 12'h028;
    localparam [11:0] COLUMN_LIMIT = 12'h048;
    localparam        SPAN_WORDS   = 8;

    localparam [11:0] WEIGHT_BASE  = 12'h400;
    localparam [11:0] OFFSET_BASE  = 12'h800;
    localparam [11:0] OFFSET_LIMIT = 12'hC00;

    localparam [31:0] ID_VALUE      = 32'h4243_4D43;   // "BCMC"
    localparam [31:0] VERSION_VALUE = 32'h0000_0400;   // v0.4
    localparam [31:0] CAPS_VALUE    =
        (IDX_W << 24) | (VAL_W << 16) | MAX_C;

    // CTRL
    localparam CTRL_START_BIT  = 0;
    localparam CTRL_IRQ_EN_BIT = 1;

    // STATUS
    localparam STATUS_IRQ_BIT = 2;

    //=======================================================================
    // Geometry
    //=======================================================================

    // The number of COLUMN words that exist.
    localparam COLUMN_WORDS = (MAX_C + 31) / 32;

    // A window index is 8 bits wide because a window is 256 words wide. This
    // is the one place where the map's geometry and the index width meet.
    localparam WIN_IDX_W = 8;

    // MAX_C at index width, so every bound check below is IDX_W against
    // IDX_W. Written as an explicit slice rather than an implicit truncation;
    // a MAX_C that does not survive the narrowing is rejected in the
    // precondition block at the bottom of the file.
    localparam [31:0]      MAX_C_W = MAX_C;
    localparam [IDX_W-1:0] LIMIT   = MAX_C_W[IDX_W-1:0];

    // Bits needed to select one of MAX_C lanes.
    localparam ROW_W = (MAX_C > 1) ? $clog2(MAX_C) : 1;

    //=======================================================================
    // Register state
    //
    // Three bits of it are the externally visible state machine of
    // docs/Register_Map.md: BUSY, VALID, IRQ. BUSY is not a register -- see
    // the sequencer below.
    //=======================================================================

    reg [VAL_W-1:0] n_q;
    reg [IDX_W-1:0] c_q;
    reg [IDX_W-1:0] cell_row_q;
    reg [VAL_W-1:0] cell_col_q;

    reg             valid_q;
    reg             irq_q;
    reg             irq_en_q;

    assign irq_o = irq_q && irq_en_q;

    //=======================================================================
    // The sequencer
    //
    // This is the whole of the Prefix Stream Interface as software sees it,
    // which is to say: not at all. A driver writes START and later observes
    // VALID. It never sees weight_valid, offset_valid, busy or a cycle count.
    //
    //   IDLE   -- software owns the context
    //   KICK   -- one cycle: `start` to the Core, `load_start` to the context
    //   STREAM -- present one weight per cycle until C have gone in, then
    //             wait for the Core's `done`
    //
    // BUSY is `seq != IDLE` rather than a register of its own, which makes
    // "BUSY is true exactly while a transform is in progress" structural
    // instead of a claim about two pieces of logic agreeing.
    //=======================================================================

    localparam [1:0] SEQ_IDLE   = 2'd0;
    localparam [1:0] SEQ_KICK   = 2'd1;
    localparam [1:0] SEQ_STREAM = 2'd2;

    reg  [1:0]       seq;
    reg  [IDX_W-1:0] stream_ptr;      // weights presented so far

    wire busy = (seq != SEQ_IDLE);

    //=======================================================================
    // Address decode
    //
    // Purely combinational from wb_adr_i. `hit_*` says which region the
    // address names; whether that region is *mapped*, and whether the access
    // is allowed, are the next two sections.
    //=======================================================================

    wire hit_id       = (wb_adr_i == ADR_ID);
    wire hit_version  = (wb_adr_i == ADR_VERSION);
    wire hit_caps     = (wb_adr_i == ADR_CAPS);
    wire hit_ctrl     = (wb_adr_i == ADR_CTRL);
    wire hit_status   = (wb_adr_i == ADR_STATUS);
    wire hit_n        = (wb_adr_i == ADR_N);
    wire hit_c        = (wb_adr_i == ADR_C);
    wire hit_cell_row = (wb_adr_i == ADR_CELL_ROW);
    wire hit_cell_col = (wb_adr_i == ADR_CELL_COL);
    wire hit_cell     = (wb_adr_i == ADR_CELL);

    wire hit_column   = (wb_adr_i >= COLUMN_BASE) && (wb_adr_i < COLUMN_LIMIT);
    wire hit_weight   = (wb_adr_i >= WEIGHT_BASE) && (wb_adr_i < OFFSET_BASE);
    wire hit_offset   = (wb_adr_i >= OFFSET_BASE) && (wb_adr_i < OFFSET_LIMIT);

    // Both windows are 0x400 bytes -- 256 words -- so the index is simply the
    // low bits of the address, and the same expression serves both.
    wire [IDX_W-1:0] win_index =
        {{(IDX_W-WIN_IDX_W){1'b0}}, wb_adr_i[WIN_IDX_W+1:2]};

    wire idx_ok = (win_index < LIMIT);

    // Which COLUMN word, counted from the base of the span.
    wire [9:0] col_widx  = wb_adr_i[11:2] - COLUMN_BASE[11:2];
    wire       col_ok    = (col_widx < COLUMN_WORDS[9:0]);

    wire aligned = (wb_adr_i[1:0] == 2'b00);

    wire mapped = hit_id | hit_version | hit_caps | hit_ctrl | hit_status
                | hit_n  | hit_c       | hit_cell_row | hit_cell_col
                | hit_cell
                | (hit_column && col_ok)
                | ((hit_weight || hit_offset) && idx_ok);

    //=======================================================================
    // The error model
    //
    //     "Silent success hides bugs."
    //
    // Four structural conditions, and this module reports no reason code, so
    // their precedence is nobody's business but its own -- the contract is
    // refusal, not which condition is blamed. docs/Transaction_Sequences.md
    // names them E1..E4 so that a test can reason about *why* an access is
    // refused; the bus carries only `err`.
    //=======================================================================

    // E1 -- not mapped, or not aligned.
    wire e_addr = !aligned || !mapped;

    // E3 -- not a full 32-bit word.
    wire e_sel = (wb_sel_i != 4'b1111);

    // E2 -- wrong access type. There are no write-only registers, so this is
    // one-sided: a write to a read-only address.
    wire e_dir = wb_we_i && (hit_id | hit_version | hit_caps | hit_cell
                             | hit_column | hit_offset);

    // E4 -- not meaningful in this state.
    //
    // While BUSY the context is mid-update, so it may not be perturbed and may
    // not be interrogated. While !VALID no matrix exists, so a question about
    // one of its bits is refused rather than answered with a plausible zero.
    //
    // START is the only condition here that depends on the data rather than
    // the address: writing CTRL with START clear is legal at any time.
    wire wr_ctrl_start = wb_we_i && hit_ctrl && wb_dat_i[CTRL_START_BIT];

    wire e_state =
        (busy && wb_we_i && (hit_n | hit_c | hit_weight))
      | (busy && wr_ctrl_start)
      | ((busy || !valid_q) && !wb_we_i && (hit_cell | hit_column | hit_offset));

    wire err_now = e_addr | e_sel | e_dir | e_state;

    //=======================================================================
    // Bus handshake
    //
    // `access` is the one cycle in which a request is serviced. Requiring
    // that neither ack nor err is already asserted is what makes the response
    // exactly one cycle wide even if a master holds stb through it, and it is
    // also what stops a held stb from being serviced twice.
    //=======================================================================

    wire access   = wb_cyc_i && wb_stb_i && !wb_ack_o && !wb_err_o;
    wire do_write = access &&  wb_we_i && !err_now;
    wire do_read  = access && !wb_we_i && !err_now;

    // A START that is actually accepted. err_now already contains
    // START-while-BUSY, so this cannot fire during a transform.
    wire start_write = do_write && hit_ctrl && wb_dat_i[CTRL_START_BIT];

    //=======================================================================
    // The persistent context
    //=======================================================================

    wire [VAL_W-1:0]       sw_weight;
    wire [VAL_W-1:0]       sw_offset;
    wire [VAL_W-1:0]       core_weight;
    wire                   ctx_loading;
    wire [MAX_C*VAL_W-1:0] weights_flat;
    wire [MAX_C*VAL_W-1:0] offsets_flat;

    wire ctx_we         = do_write && hit_weight;
    wire ctx_load_start = (seq == SEQ_KICK);

    wire core_start;
    wire core_busy;
    wire core_done;
    wire core_weight_valid;
    wire [VAL_W-1:0] core_offset_out;
    wire             core_offset_valid;

    wire ctx_load_done = (seq == SEQ_STREAM) && core_done;

    bcmc_context #(
        .VAL_W (VAL_W),
        .IDX_W (IDX_W),
        .MAX_C (MAX_C)
    ) u_context (
        .clk          (wb_clk_i),
        .rst          (wb_rst_i),

        .sw_we        (ctx_we),
        .sw_windex    (win_index),
        .sw_wdata     (wb_dat_i[VAL_W-1:0]),

        // One read index serves both windows, because one bus access reads at
        // most one of them.
        .sw_rindex    (win_index),
        .sw_weight    (sw_weight),
        .sw_offset    (sw_offset),

        .core_rindex  (stream_ptr),
        .core_weight  (core_weight),

        .load_start   (ctx_load_start),
        .offset_valid (core_offset_valid),
        .offset_in    (core_offset_out),
        .load_done    (ctx_load_done),
        .loading      (ctx_loading),

        .weights_flat (weights_flat),
        .offsets_flat (offsets_flat)
    );

    //=======================================================================
    // The Core, and the Prefix Stream Interface
    //
    // The Core has no backpressure: it accepts one weight per cycle whenever
    // weight_valid is asserted while running. So the stream is just a counter,
    // and the counter is the whole protocol.
    //
    // Two of the Core's preconditions are kept here rather than checked:
    //   - weight_valid is asserted only while `busy`, never outside a run;
    //   - it is deasserted the moment C weights have gone in.
    //=======================================================================

    assign core_start        = (seq == SEQ_KICK);
    assign core_weight_valid = (seq == SEQ_STREAM) && core_busy
                               && (stream_ptr != c_q);

    bcmc_core #(
        .VAL_W (VAL_W),
        .IDX_W (IDX_W)
    ) u_core (
        .clk          (wb_clk_i),
        .rst          (wb_rst_i),
        .start        (core_start),
        .busy         (core_busy),
        .done         (core_done),
        .N            (n_q),
        .C            (c_q),
        .weight_in    (core_weight),
        .weight_valid (core_weight_valid),
        .offset_out   (core_offset_out),
        .offset_valid (core_offset_valid)
    );

    //=======================================================================
    // The projection
    //
    // One bcmc_column answers M(i, CELL_COL) for every row of the instance at
    // once. N and C come from the registers, and that is safe because writing
    // either clears VALID: an evaluation is only ever answered while the
    // offsets in the context are the ones this N and this C produced.
    //=======================================================================

    wire [MAX_C-1:0] column_bits;

    bcmc_column #(
        .VAL_W (VAL_W),
        .IDX_W (IDX_W),
        .MAX_C (MAX_C)
    ) u_column (
        .N            (n_q),
        .C            (c_q),
        .column       (cell_col_q),
        .weights_flat (weights_flat),
        .offsets_flat (offsets_flat),
        .column_bits  (column_bits)
    );

    // The column packed into the eight words of the COLUMN span, row 0 in bit
    // 0 of word 0. Bits at or above MAX_C read 0; bits at or above C are
    // already 0, because bcmc_column zeroes an inactive lane's arguments.
    //
    // All eight words are built, not just the ceil(MAX_C/32) that exist, so
    // that the part-select below is in range for any address in the span. The
    // words that do not exist are constant zero and are refused by E1 before
    // anything can read them; they cost nothing after constant folding.
    wire [32*SPAN_WORDS-1:0] column_span;

    genvar gb;
    generate
        for (gb = 0; gb < 32 * SPAN_WORDS; gb = gb + 1) begin : g_span
            if (gb < MAX_C) begin : g_present
                assign column_span[gb] = column_bits[gb];
            end else begin : g_absent
                assign column_span[gb] = 1'b0;
            end
        end
    endgenerate

    wire [31:0] col_word = column_span[{col_widx[2:0], 5'b00000} +: 32];

    // CELL is one bit of COLUMN, because M(CELL_ROW, CELL_COL) is one bit of
    // column CELL_COL. A CELL_ROW naming a lane that does not exist reads 0:
    // CELL_ROW >= C is an inactive lane and not an error, and CELL_ROW >= MAX_C
    // is a row this instance could never hold.
    wire             cell_in_range = (cell_row_q < LIMIT);
    wire [ROW_W-1:0] cell_addr     = cell_row_q[ROW_W-1:0];
    wire             cell_bit      = cell_in_range && column_bits[cell_addr];

    //=======================================================================
    // Read data
    //
    // Reserved bits read 0, which is why every branch starts from a cleared
    // word and then drops its field in. Nothing here depends on the state
    // bits: a read that should not be answered has already been refused.
    //=======================================================================

    reg [31:0] rdata;

    always @(*) begin
        rdata = 32'h0000_0000;

        if      (hit_id)       rdata                = ID_VALUE;
        else if (hit_version)  rdata                = VERSION_VALUE;
        else if (hit_caps)     rdata                = CAPS_VALUE;
        // START is a command, not a state, and always reads 0.
        else if (hit_ctrl)     rdata[CTRL_IRQ_EN_BIT] = irq_en_q;
        else if (hit_status)   rdata[2:0]           = {irq_q, valid_q, busy};
        else if (hit_n)        rdata[VAL_W-1:0]     = n_q;
        else if (hit_c)        rdata[IDX_W-1:0]     = c_q;
        else if (hit_cell_row) rdata[IDX_W-1:0]     = cell_row_q;
        else if (hit_cell_col) rdata[VAL_W-1:0]     = cell_col_q;
        else if (hit_cell)     rdata[0]             = cell_bit;
        else if (hit_column)   rdata                = col_word;
        else if (hit_weight)   rdata[VAL_W-1:0]     = sw_weight;
        else if (hit_offset)   rdata[VAL_W-1:0]     = sw_offset;
    end

    //=======================================================================
    // Bus response
    //
    // dat_o is not updated by an erring cycle. An `err` carries no data, and
    // leaving the previous word in place is more honest than manufacturing a
    // zero for software to mistake for an answer.
    //=======================================================================

    always @(posedge wb_clk_i) begin
        if (wb_rst_i) begin
            wb_ack_o <= 1'b0;
            wb_err_o <= 1'b0;
            wb_dat_o <= 32'h0000_0000;
        end else begin
            wb_ack_o <= access && !err_now;
            wb_err_o <= access &&  err_now;
            if (do_read) begin
                wb_dat_o <= rdata;
            end
        end
    end

    //=======================================================================
    // Registers, state bits and the sequencer
    //
    // Every update below is gated by do_write, and do_write excludes err_now.
    // That is the whole of "a write that returns err has no side effect
    // whatsoever": there is no path from a refused access to any enable.
    //=======================================================================

    always @(posedge wb_clk_i) begin
        if (wb_rst_i) begin
            n_q        <= {VAL_W{1'b0}};
            c_q        <= {IDX_W{1'b0}};
            cell_row_q <= {IDX_W{1'b0}};
            cell_col_q <= {VAL_W{1'b0}};
            valid_q    <= 1'b0;
            irq_q      <= 1'b0;
            irq_en_q   <= 1'b0;
            seq        <= SEQ_IDLE;
            stream_ptr <= {IDX_W{1'b0}};
        end else begin

            //---------------------------------------------------------------
            // Writes
            //---------------------------------------------------------------
            if (do_write) begin
                if (hit_ctrl) begin
                    // IRQ_EN is taken before START acts, so that enabling it
                    // in the same write that starts a transform arms the pin
                    // for that transform's completion.
                    irq_en_q <= wb_dat_i[CTRL_IRQ_EN_BIT];
                end
                if (hit_status && wb_dat_i[STATUS_IRQ_BIT]) begin
                    irq_q <= 1'b0;              // write 1 to clear
                end
                if (hit_n) begin
                    n_q     <= wb_dat_i[VAL_W-1:0];
                    valid_q <= 1'b0;
                end
                if (hit_c) begin
                    c_q     <= wb_dat_i[IDX_W-1:0];
                    valid_q <= 1'b0;
                end
                // A query index is part of the query, not part of the
                // context, so it does not disturb VALID.
                if (hit_cell_row) begin
                    cell_row_q <= wb_dat_i[IDX_W-1:0];
                end
                if (hit_cell_col) begin
                    cell_col_q <= wb_dat_i[VAL_W-1:0];
                end
                if (hit_weight) begin
                    // The context stores it; here it only invalidates, since
                    // the offsets now describe weights nobody programmed.
                    valid_q <= 1'b0;
                end
            end

            //---------------------------------------------------------------
            // The sequencer
            //---------------------------------------------------------------
            case (seq)

                SEQ_IDLE: begin
                    if (start_write) begin
                        // VALID falls in the same edge that BUSY rises, so
                        // there is no cycle in which VALID is true and the
                        // context is being overwritten.
                        valid_q    <= 1'b0;
                        stream_ptr <= {IDX_W{1'b0}};
                        seq        <= SEQ_KICK;
                    end
                end

                SEQ_KICK: begin
                    // `start` and `load_start` are asserted for exactly this
                    // cycle. The Core is in ST_IDLE now and in ST_RUN next.
                    seq <= SEQ_STREAM;
                end

                SEQ_STREAM: begin
                    if (core_weight_valid) begin
                        stream_ptr <= stream_ptr + {{(IDX_W-1){1'b0}}, 1'b1};
                    end
                    if (core_done) begin
                        // `done` means the entire offset stream has been
                        // emitted, so the context is complete as of now.
                        valid_q <= 1'b1;
                        irq_q   <= 1'b1;
                        seq     <= SEQ_IDLE;
                    end
                end

                default: begin
                    seq <= SEQ_IDLE;
                end

            endcase
        end
    end

    //=======================================================================
    // Deliberately ignored inputs
    //
    // The register map says a write to a reserved bit is ignored rather than
    // refused, so the upper bits of wb_dat_i genuinely have no reader. The
    // context's `loading` output is its arbiter's view of the same state this
    // module's sequencer owns, and is used only to check that the two agree.
    // Naming both here records that the omission is a decision.
    //=======================================================================

    // verilator lint_off UNUSED
    wire unused_ok = ^{wb_dat_i, ctx_loading};
    // verilator lint_on UNUSED

    //=======================================================================
    // Preconditions
    //
    // Note what is absent: nothing here checks N >= 1, or a weight against N,
    // or CELL_COL against N. Those are preconditions of the Balance Theorem,
    // not of the bus, and "the peripheral does not check them, because
    // checking them would be new mathematics in a bus adapter". A START that
    // violates one is answered with `ack` and then trips the Core's own
    // assertion, which is exactly where the violation belongs.
    //
    // What *is* checked is geometry, and the two contracts this module owes
    // the modules underneath it.
    //=======================================================================

`ifndef SYNTHESIS
    initial begin
        if ((MAX_C < 1) || (MAX_C > 256)) begin
            $display("bcmc_wb: ERROR MAX_C = %0d (1 <= MAX_C <= 256 required)",
                     MAX_C);
            $stop;
        end
        if ((VAL_W < 1) || (VAL_W > 32)) begin
            $display("bcmc_wb: ERROR VAL_W = %0d (1 <= VAL_W <= 32 required)",
                     VAL_W);
            $stop;
        end
        if ((IDX_W < WIN_IDX_W) || (IDX_W > 32)) begin
            $display("bcmc_wb: ERROR IDX_W = %0d (%0d <= IDX_W <= 32 required)",
                     IDX_W, WIN_IDX_W);
            $stop;
        end
        if ((MAX_C_W >> IDX_W) != 32'd0) begin
            $display("bcmc_wb: ERROR MAX_C = %0d does not fit in IDX_W = %0d",
                     MAX_C, IDX_W);
            $stop;
        end
    end

    always @(posedge wb_clk_i) begin
        if (!wb_rst_i) begin
            // The bus contract, stated as an assertion rather than as prose.
            if (wb_ack_o && wb_err_o) begin
                $display("bcmc_wb: ERROR ack and err asserted together");
                $stop;
            end
            // C <= MAX_C is geometry, and a C above it would stream weights
            // through lanes that do not exist.
            if (start_write && (c_q > LIMIT)) begin
                $display("bcmc_wb: ERROR START with C = %0d > MAX_C = %0d",
                         c_q, MAX_C);
                $stop;
            end
            // The two contracts owed downwards.
            if (core_weight_valid && !core_busy) begin
                $display("bcmc_wb: ERROR weight_valid while the Core is idle");
                $stop;
            end
            if (ctx_we && ctx_loading) begin
                $display("bcmc_wb: ERROR weight write while the context is loading");
                $stop;
            end
            // The sequencer's own invariant: never more than C weights.
            if ((seq == SEQ_STREAM) && (stream_ptr > c_q)) begin
                $display("bcmc_wb: ERROR stream_ptr %0d exceeds C = %0d",
                         stream_ptr, c_q);
                $stop;
            end
        end
    end
`endif

endmodule

`default_nettype wire

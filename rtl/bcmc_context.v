//===========================================================================
// bcmc_context.v -- The persistent BCMC context: storage and arbitration
//
//     MAX_C x (weight, offset)
//
// This module contains NO BCMC MATHEMATICS. There is no prefix recursion
// here, no reduction modulo N, no characteristic function. And so that this
// is checkable rather than merely claimed: this module has no N port and no
// C port. It cannot compute BCMC because it is not told the two numbers that
// define a BCMC problem. It does not know what a weight means.
//
// The only arithmetic in this file is `wr_ptr + 1`. That addresses storage;
// it computes nothing about BCMC.
//
// What the module is
// ------------------
// The persistent BCMC context -- the object that exists between software and
// hardware. It is the canonical prefix representation given somewhere to
// live, which is the storage the BCMC Core deliberately refused to contain.
// Its three responsibilities are exactly:
//
//     1. store the weights,
//     2. store the offsets,
//     3. arbitrate between the three clients below.
//
// If prefix sums, modulo arithmetic or characteristic functions ever appear
// in this file, responsibilities have bled across a module boundary.
//
// The three clients
// -----------------
//         software                the Core              the Evaluator
//            |                       |                        |
//     write weights            read weights          read both windows,
//     read both windows        write offsets         all lanes, always
//            |                       |                        |
//            v                       v                        v
//     +---------------------------------------------------------------+
//     |   weight_mem[0 .. MAX_C-1]         offset_mem[0 .. MAX_C-1]   |
//     +---------------------------------------------------------------+
//
// Software cannot write an offset. That is not enforced by a check; there is
// simply no port through which it could. `offset[0] = 0` therefore remains a
// structural fact and cannot be seeded away -- the same door the Core closes
// by withholding `P_C mod N`, closed here by an absent port rather than by a
// rule. See "OFFSET[i]" in docs/Register_Map.md.
//
// Flip-flops, not a RAM
// ---------------------
// The Evaluator reads every lane of both windows combinationally, because
// `M(i,j)` depends on nothing but its own arguments and so a query has no
// handshake to wait for ("Evaluation has no handshake", docs/Register_Map.md).
// A synchronous single-port RAM cannot answer that, so the storage is a
// register file: 2 * MAX_C * VAL_W flip-flops, 2048 at MAX_C = 64, VAL_W = 16.
// That cost is bought deliberately, and it buys the absence of latency the
// mathematics does not have.
//
// Arbitration
// -----------
// The two writers never contend for a location -- software writes weights,
// the Core writes offsets -- so arbitration here is about ownership in time,
// not about ports:
//
//     while `loading`, the Core owns the context and software is locked out;
//     otherwise, software owns the weight window.
//
// A refused write is silently dropped, and reported in simulation. It should
// never happen: the register map already denies such a write at the bus with
// E4, so a refusal reaching this module means the wrapper has a bug. This is
// defence in depth, so that a wrapper bug cannot corrupt a context mid
// transform.
//
// Clearing the offset window
// --------------------------
// `load_start` clears the whole offset window, then the stream refills
// [0 .. C-1]. This delivers the register map's requirement that a transform
// leaves OFFSET[C .. MAX_C-1] at zero, never a stale offset from an earlier,
// larger C -- and it delivers it without this module ever learning C.
//
// Generic Verilog-2005. Nothing here is specific to any FPGA family.
//===========================================================================

`default_nettype none

module bcmc_context #(
    // Every stored value -- weight or offset -- fits in VAL_W bits.
    parameter VAL_W = 16,
    // Every index fits in IDX_W bits.
    parameter IDX_W = 16,
    // The most rows this instance can hold. A geometry bound, not a C.
    parameter MAX_C = 16
) (
    input  wire                   clk,
    input  wire                   rst,          // synchronous, active high

    //-----------------------------------------------------------------------
    // Client 1: software
    //
    // Writes the weight window; reads both. There is no read enable, because
    // reading a register file has no side effect: sw_weight and sw_offset are
    // simply always the contents at sw_rindex.
    //-----------------------------------------------------------------------
    input  wire                   sw_we,        // write weight[sw_windex]
    input  wire [IDX_W-1:0]       sw_windex,
    input  wire [VAL_W-1:0]       sw_wdata,

    input  wire [IDX_W-1:0]       sw_rindex,
    output wire [VAL_W-1:0]       sw_weight,    // weight[sw_rindex]
    output wire [VAL_W-1:0]       sw_offset,    // offset[sw_rindex]

    //-----------------------------------------------------------------------
    // Client 2: the BCMC Core
    //
    // Reads weights so a sequencer can stream them in, and writes the
    // offsets that stream back out. offset_in/offset_valid connect directly
    // to the Core's offset_out/offset_valid: this module is the sink of the
    // Prefix Stream Interface's output half.
    //
    // The sequencer that walks the window into the Core lives in the bus
    // wrapper, not here. This module holds no traversal.
    //-----------------------------------------------------------------------
    input  wire [IDX_W-1:0]       core_rindex,
    output wire [VAL_W-1:0]       core_weight,  // weight[core_rindex]

    input  wire                   load_start,   // clear offsets, take ownership
    input  wire                   offset_valid, // from the Core
    input  wire [VAL_W-1:0]       offset_in,    // from the Core
    input  wire                   load_done,    // release ownership

    output wire                   loading,      // the arbiter's state

    //-----------------------------------------------------------------------
    // Client 3: the BCMC Evaluator
    //
    // Both windows, every lane, every cycle. Row i occupies bits
    // [VAL_W*i +: VAL_W], matching the flat-vector convention of
    // bcmc_row.v and bcmc_column.v, so these ports connect straight across.
    //
    // The Evaluator is stateless and holds nothing; it is given the
    // representation, and this is where it is given it.
    //-----------------------------------------------------------------------
    output wire [MAX_C*VAL_W-1:0] weights_flat,
    output wire [MAX_C*VAL_W-1:0] offsets_flat
);

    //-----------------------------------------------------------------------
    // Storage
    //-----------------------------------------------------------------------

    reg [VAL_W-1:0] weight_mem [0:MAX_C-1];
    reg [VAL_W-1:0] offset_mem [0:MAX_C-1];

    //-----------------------------------------------------------------------
    // Arbiter and write pointer
    //
    // wr_ptr is the offset window's fill level. It is addressing, not
    // mathematics: this module cannot tell an offset from any other VAL_W-bit
    // number, and does not need to.
    //-----------------------------------------------------------------------

    reg             loading_q;
    reg [IDX_W-1:0] wr_ptr;

    assign loading = loading_q;

    //-----------------------------------------------------------------------
    // Bounds
    //
    // MAX_C is a geometry bound and indices are IDX_W bits wide, so an index
    // can name a lane that does not exist. LIMIT is MAX_C in index width, so
    // every comparison below is IDX_W against IDX_W.
    //
    // MAX_C_W is the same number at the width a parameter expression carries,
    // and exists only so that LIMIT can be written as an explicit slice of it
    // rather than as an implicit truncation. IDX_W <= 32 is required for the
    // slice to be legal, and the check below rejects a MAX_C that does not
    // survive the narrowing.
    //-----------------------------------------------------------------------

    localparam [31:0]      MAX_C_W = MAX_C;
    localparam [IDX_W-1:0] LIMIT   = MAX_C_W[IDX_W-1:0];

    wire sw_w_ok   = (sw_windex   < LIMIT);
    wire sw_r_ok   = (sw_rindex   < LIMIT);
    wire core_r_ok = (core_rindex < LIMIT);
    wire wr_ok     = (wr_ptr      < LIMIT);

    //-----------------------------------------------------------------------
    // Addressing
    //
    // An index is IDX_W bits because that is the width of the interface, but
    // the storage needs only ADDR_W bits to address it. The bounds checks
    // above guarantee that every index actually used to reach the storage
    // satisfies index < MAX_C, so the truncation below discards nothing: an
    // index that would not survive it is one that has already been refused.
    //
    // MAX_C fitting in IDX_W, checked below, implies ADDR_W <= IDX_W.
    //-----------------------------------------------------------------------

    localparam ADDR_W = (MAX_C > 1) ? $clog2(MAX_C) : 1;

    wire [ADDR_W-1:0] sw_waddr   = sw_windex[ADDR_W-1:0];
    wire [ADDR_W-1:0] sw_raddr   = sw_rindex[ADDR_W-1:0];
    wire [ADDR_W-1:0] core_raddr = core_rindex[ADDR_W-1:0];
    wire [ADDR_W-1:0] wr_addr    = wr_ptr[ADDR_W-1:0];

    //-----------------------------------------------------------------------
    // Reads
    //
    // An index naming a lane that does not exist reads 0. That keeps the
    // module total -- defined for every input -- rather than returning x.
    //
    // Zero also happens to be a legal weight for every N, so an out-of-range
    // core_weight could not corrupt a transform. That is a coincidence, and
    // nothing here relies on it.
    //-----------------------------------------------------------------------

    assign sw_weight   = sw_r_ok   ? weight_mem[sw_raddr]   : {VAL_W{1'b0}};
    assign sw_offset   = sw_r_ok   ? offset_mem[sw_raddr]   : {VAL_W{1'b0}};
    assign core_weight = core_r_ok ? weight_mem[core_raddr] : {VAL_W{1'b0}};

    genvar i;

    generate
        for (i = 0; i < MAX_C; i = i + 1) begin : g_flat
            assign weights_flat[VAL_W*i +: VAL_W] = weight_mem[i];
            assign offsets_flat[VAL_W*i +: VAL_W] = offset_mem[i];
        end
    endgenerate

    //-----------------------------------------------------------------------
    // Writes
    //-----------------------------------------------------------------------

    integer k;

    always @(posedge clk) begin
        if (rst) begin
            for (k = 0; k < MAX_C; k = k + 1) begin
                weight_mem[k] <= {VAL_W{1'b0}};
                offset_mem[k] <= {VAL_W{1'b0}};
            end
            wr_ptr    <= {IDX_W{1'b0}};
            loading_q <= 1'b0;
        end else begin

            //---------------------------------------------------------------
            // Ownership. load_start wins over load_done, which matters only
            // for an illegal input; the two are never asserted together.
            //---------------------------------------------------------------
            if (load_start) begin
                loading_q <= 1'b1;
            end else if (load_done) begin
                loading_q <= 1'b0;
            end

            //---------------------------------------------------------------
            // The weight window. Software's write is accepted only while
            // software owns the context.
            //---------------------------------------------------------------
            if (sw_we && !loading_q && sw_w_ok) begin
                weight_mem[sw_waddr] <= sw_wdata;
            end

            //---------------------------------------------------------------
            // The offset window. Cleared whole at load_start, then filled in
            // stream order, one location per accepted offset.
            //---------------------------------------------------------------
            if (load_start) begin
                for (k = 0; k < MAX_C; k = k + 1) begin
                    offset_mem[k] <= {VAL_W{1'b0}};
                end
                wr_ptr <= {IDX_W{1'b0}};
            end else if (offset_valid && loading_q && wr_ok) begin
                offset_mem[wr_addr] <= offset_in;
                wr_ptr              <= wr_ptr + {{(IDX_W-1){1'b0}}, 1'b1};
            end
        end
    end

    //-----------------------------------------------------------------------
    // Preconditions
    //
    // Every one of these is a contract the bus wrapper is required to keep,
    // and the register map already denies each corresponding access at the
    // bus with E1 or E4. Reaching this module means the wrapper is broken, so
    // the module drops the write and says so, loudly, in simulation.
    //
    // Note what is absent: nothing here checks a weight against N, or an
    // index against C. This module has neither, and could not.
    //-----------------------------------------------------------------------

`ifndef SYNTHESIS
    initial begin
        if (MAX_C < 1) begin
            $display("bcmc_context: ERROR MAX_C = %0d (MAX_C >= 1 required)",
                     MAX_C);
            $stop;
        end
        if ((MAX_C_W >> IDX_W) != 32'd0) begin
            $display("bcmc_context: ERROR MAX_C = %0d does not fit in IDX_W = %0d",
                     MAX_C, IDX_W);
            $stop;
        end
    end

    always @(posedge clk) begin
        if (!rst) begin
            if (sw_we && loading_q) begin
                $display("bcmc_context: ERROR weight write while loading (E4)");
                $stop;
            end
            if (sw_we && !sw_w_ok) begin
                $display("bcmc_context: ERROR weight write index %0d >= MAX_C = %0d (E1)",
                         sw_windex, MAX_C);
                $stop;
            end
            if (offset_valid && !loading_q) begin
                $display("bcmc_context: ERROR offset_valid while not loading");
                $stop;
            end
            if (offset_valid && loading_q && !wr_ok && !load_start) begin
                $display("bcmc_context: ERROR more than MAX_C = %0d offsets streamed",
                         MAX_C);
                $stop;
            end
            if (load_start && loading_q && !load_done) begin
                $display("bcmc_context: ERROR load_start while already loading");
                $stop;
            end
            if (load_start && load_done) begin
                $display("bcmc_context: ERROR load_start and load_done together");
                $stop;
            end
        end
    end
`endif

endmodule

`default_nettype wire

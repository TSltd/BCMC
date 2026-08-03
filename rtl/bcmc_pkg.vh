//===========================================================================
// bcmc_pkg.vh -- Shared definitions for the BCMC IP blocks
//
// Verilog-2005 has no package construct, so the shared defaults live here as
// macros. Including this file is optional: every module carries the same
// values as its own parameter defaults and can be overridden at instantiation.
//
// Part of the BCMC project. See docs/Hardware_Architecture.md.
//===========================================================================

`ifndef BCMC_PKG_VH
`define BCMC_PKG_VH

//---------------------------------------------------------------------------
// Datapath widths
//---------------------------------------------------------------------------
//
// BCMC_VAL_W bounds N, and therefore also every weight and every offset,
// since the specification requires
//
//     0 <= w_i <= N     and     0 <= offset_i < N.
//
// With the default of 16 bits, N may be up to 65535.
//
// BCMC_IDX_W bounds C, the number of rows.
//
// Note that internal prefix arithmetic needs BCMC_VAL_W+1 bits, because
// offset + weight < 2N. It never needs more: that is exactly why the
// reduction modulo N is a comparison and a subtraction rather than a
// division.

`define BCMC_VAL_W 16
`define BCMC_IDX_W 16

`endif // BCMC_PKG_VH

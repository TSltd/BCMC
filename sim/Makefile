#===========================================================================
# sim/Makefile -- Icarus Verilog path
#
# The primary simulation flow is Verilator, driven by CMake:
#
#     mkdir -p build && cd build && cmake .. && make && ctest
#
# This Makefile is the second opinion: plain Verilog testbenches, a different
# simulator, the same vector files, the same reference model. If the two
# simulators ever disagree, that is worth knowing.
#
#     make          # run every vector file of every testbench
#     make core     # just bcmc_core
#     make cell     # just bcmc_cell
#     make row      # just bcmc_row     (the row projection of the matrix)
#     make column   # just bcmc_column  (the column projection)
#     make soak     # the optional big files from gen_vectors.py --big
#     make waves    # run one small file per testbench and dump waves/*.vcd
#     make gtkwave  # ... and open the core trace
#     make clean
#===========================================================================

IVERILOG ?= iverilog
VVP      ?= vvp
GTKWAVE  ?= gtkwave

RTLDIR   := ../rtl

# build/ belongs to CMake; the Icarus flow keeps its own directory so that the
# two never tread on each other.
BUILDDIR := build-icarus

# One entry per testbench: top module, its testbench file, the RTL it needs,
# and the vector files it must pass. Adding a module means adding a stanza,
# not editing the rules.
TOPS       := tb_core tb_cell tb_row tb_column

# bcmc_row and bcmc_column are replication of bcmc_cell, and their testbenches
# instantiate a SECOND, separate cell to prove it, so the cell is in both lists.
RTL_tb_core   := $(RTLDIR)/bcmc_core.v
RTL_tb_cell   := $(RTLDIR)/bcmc_cell.v
RTL_tb_row    := $(RTLDIR)/bcmc_row.v $(RTLDIR)/bcmc_cell.v
RTL_tb_column := $(RTLDIR)/bcmc_column.v $(RTLDIR)/bcmc_cell.v

VEC_tb_core  := vectors/core_edge.txt \
                vectors/core_random.txt \
                vectors/core_biased.txt \
                vectors/core_large.txt

VEC_tb_cell  := vectors/cell_edge.txt \
                vectors/cell_exhaustive.txt \
                vectors/cell_random.txt

# Both projections read the same whole-matrix suites. They must agree on every
# bit, having assembled the same matrix from opposite directions.
VEC_MATRIX   := vectors/matrix_edge.txt \
                vectors/matrix_exhaustive.txt \
                vectors/matrix_random.txt

VEC_tb_row    := $(VEC_MATRIX)
VEC_tb_column := $(VEC_MATRIX)

SOAK_MATRIX  := vectors/matrix_exhaustive_6.txt \
                vectors/matrix_random_5k.txt

SOAK_tb_core   := vectors/core_random_10k.txt
SOAK_tb_cell   := vectors/cell_exhaustive_32.txt
SOAK_tb_row    := $(SOAK_MATRIX)
SOAK_tb_column := $(SOAK_MATRIX)

WAVE_tb_core   := vectors/core_edge.txt
WAVE_tb_cell   := vectors/cell_edge.txt
WAVE_tb_row    := vectors/matrix_edge.txt
WAVE_tb_column := vectors/matrix_edge.txt

# tb_cell holds a whole suite in memory so that it can replay it in three
# different orders. The array size is a compile time choice in Verilog-2005,
# so the soak build gets a bigger one.
DEFS_tb_cell      :=
SOAKDEFS_tb_cell  := -DMAXCASE=400000

.PHONY: all test core cell row column soak waves gtkwave vectors clean

all: test

test: core cell row column
	@echo "ICARUS: PASS"

$(BUILDDIR):
	@mkdir -p $(BUILDDIR)

waves-dir:
	@mkdir -p waves

#---------------------------------------------------------------------------
# Build and run rules, generated once per testbench
#
# -Wno-timescale: the RTL deliberately declares no `timescale. A timescale is
# a property of a simulation, not of hardware, so the testbench owns it and the
# synthesisable sources inherit it.
#
# Every vector file must print its own PASS line. grep, not just the exit
# status, because a testbench that silently ran nothing would otherwise look
# like a success.
#---------------------------------------------------------------------------

define TB_rules

$(BUILDDIR)/$(1).vvp: $(1).v $(RTL_$(1)) | $(BUILDDIR)
	$$(IVERILOG) -g2005 -Wall -Wno-timescale $(DEFS_$(1)) -s $(1) -o $$@ $(1).v $(RTL_$(1))

$(BUILDDIR)/$(1)-soak.vvp: $(1).v $(RTL_$(1)) | $(BUILDDIR)
	$$(IVERILOG) -g2005 -Wall -Wno-timescale $(SOAKDEFS_$(1)) -s $(1) -o $$@ $(1).v $(RTL_$(1))

run-$(1): $(BUILDDIR)/$(1).vvp
	@fail=0; \
	for v in $(VEC_$(1)); do \
	    if [ ! -f $$$$v ]; then \
	        echo "MISSING $$$$v  (cd ../validation && python3 gen_vectors.py)"; \
	        fail=1; continue; \
	    fi; \
	    out=$$$$($$(VVP) $$< +vectors=$$$$v 2>&1); \
	    echo "$$$$out" | grep -q '$(1): PASS' || { echo "$$$$out"; fail=1; }; \
	    echo "$$$$out" | grep '$(1): PASS'; \
	done; \
	if [ $$$$fail -ne 0 ]; then echo "ICARUS $(1): FAIL"; exit 1; fi

# Optional big files from `python3 gen_vectors.py --big`. Skipped, not failed,
# when they have not been generated.
soak-$(1): $(BUILDDIR)/$(1)-soak.vvp
	@for v in $(SOAK_$(1)); do \
	    if [ ! -f $$$$v ]; then \
	        echo "SKIP $$$$v  (cd ../validation && python3 gen_vectors.py --big)"; \
	        continue; \
	    fi; \
	    $$(VVP) $$< +vectors=$$$$v | grep '$(1): PASS' || exit 1; \
	done

waves-$(1): $(BUILDDIR)/$(1).vvp waves-dir
	$$(VVP) $$< +vectors=$(WAVE_$(1)) +vcd

endef

$(foreach t,$(TOPS),$(eval $(call TB_rules,$(t))))

core: run-tb_core

cell: run-tb_cell

row: run-tb_row

column: run-tb_column

soak: $(addprefix soak-,$(TOPS))

waves: $(addprefix waves-,$(TOPS))

gtkwave: waves-tb_core
	$(GTKWAVE) waves/tb_core.vcd &

# Regenerate the vector files from the Python reference model.
vectors:
	cd ../validation && python3 gen_vectors.py

clean:
	rm -rf $(BUILDDIR)
	rm -f $(addprefix waves/,$(addsuffix .vcd,$(TOPS)))

#===========================================================================
# sim/Makefile -- Icarus Verilog path
#
# The primary simulation flow is Verilator, driven by CMake:
#
#     mkdir -p build && cd build && cmake .. && make && ctest
#
# This Makefile is the second opinion: a plain Verilog testbench, a different
# simulator, the same vector files, the same reference model. If the two
# simulators ever disagree, that is worth knowing.
#
#     make          # run every vector file
#     make waves    # run one small file and dump waves/tb_core.vcd
#     make gtkwave  # ... and open it
#     make clean
#===========================================================================

IVERILOG ?= iverilog
VVP      ?= vvp
GTKWAVE  ?= gtkwave

# build/ belongs to CMake; the Icarus flow keeps its own directory so that the
# two never tread on each other.
RTL      := ../rtl/bcmc_core.v
TB       := tb_core.v
BUILDDIR := build-icarus
OUT      := $(BUILDDIR)/tb_core.vvp


VECTORS  := vectors/core_edge.txt \
            vectors/core_random.txt \
            vectors/core_biased.txt \
            vectors/core_large.txt

.PHONY: all test soak waves gtkwave vectors clean

all: test

$(BUILDDIR):
	@mkdir -p $(BUILDDIR)

waves-dir:
	@mkdir -p waves

# -Wno-timescale: the RTL deliberately declares no `timescale. A timescale is
# a property of a simulation, not of hardware, so the testbench owns it and the
# synthesisable sources inherit it.
$(OUT): $(TB) $(RTL) | $(BUILDDIR)
	$(IVERILOG) -g2005 -Wall -Wno-timescale -s tb_core -o $@ $(TB) $(RTL)



# Every vector file must print its own PASS line. grep, not just the exit
# status, because a testbench that silently ran nothing would otherwise look
# like a success.
test: $(OUT)
	@fail=0; \
	for v in $(VECTORS); do \
	    if [ ! -f $$v ]; then \
	        echo "MISSING $$v  (cd ../validation && python3 gen_vectors.py)"; \
	        fail=1; continue; \
	    fi; \
	    out=$$($(VVP) $(OUT) +vectors=$$v 2>&1); \
	    echo "$$out" | grep -q 'tb_core: PASS' || { echo "$$out"; fail=1; }; \
	    echo "$$out" | grep 'tb_core: PASS'; \
	done; \
	if [ $$fail -ne 0 ]; then echo "ICARUS: FAIL"; exit 1; fi; \
	echo "ICARUS: PASS"

# Optional soak file from `python3 gen_vectors.py --big`.
soak: $(OUT)
	$(VVP) $(OUT) +vectors=vectors/core_random_10k.txt

waves: $(OUT) waves-dir
	$(VVP) $(OUT) +vectors=vectors/core_edge.txt +vcd

gtkwave: waves
	$(GTKWAVE) waves/tb_core.vcd &

# Regenerate the vector files from the Python reference model.
vectors:
	cd ../validation && python3 gen_vectors.py

clean:
	rm -rf $(BUILDDIR)
	rm -f waves/tb_core.vcd


#===========================================================================
# sim/CMakeLists.txt -- Verilator simulation harness for the BCMC RTL
#
#   mkdir -p build && cd build
#   cmake ..
#   make
#   ctest --output-on-failure
#
# The vector files consumed by the tests are generated from the Python
# reference model:
#
#   cd validation && python3 gen_vectors.py
#
#===========================================================================

cmake_minimum_required(VERSION 3.20)

# C as well as C++, because sw/bcmc.c is compiled here as C99 -- the same
# translation unit a bare-metal target would build -- rather than dragged into
# C++ by the harness that drives it. A driver that only ever sees a C++
# compiler is not a C driver.
project(bcmc_sim C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# CMake 3.27 introduced CMP0144: find_package() honours the upper-case
# <PACKAGENAME>_ROOT variable.  VERILATOR_ROOT is exactly that variable, and
# honouring it is the behaviour the error message below promises, so opt in.
# Guarded so that CMake 3.20 .. 3.26 still configure without complaint.
if(POLICY CMP0144)
  cmake_policy(SET CMP0144 NEW)
endif()

find_package(verilator HINTS $ENV{VERILATOR_ROOT})

if(NOT verilator_FOUND)
  message(FATAL_ERROR
    "Verilator was not found. Install it (apt install verilator) or set "
    "VERILATOR_ROOT to a Verilator source tree.")
endif()

set(BCMC_RTL      ${CMAKE_CURRENT_SOURCE_DIR}/../rtl)
set(BCMC_SW       ${CMAKE_CURRENT_SOURCE_DIR}/../sw)
set(BCMC_EXAMPLES ${CMAKE_CURRENT_SOURCE_DIR}/../examples)
set(BCMC_VECTORS  ${CMAKE_CURRENT_SOURCE_DIR}/vectors)
set(BCMC_WAVES    ${CMAKE_CURRENT_SOURCE_DIR}/waves)

#---------------------------------------------------------------------------
# Shared test-vector reader
#---------------------------------------------------------------------------

add_library(bcmc_sim_common STATIC common/vectors.cpp common/wb_bfm.cpp)
target_include_directories(bcmc_sim_common PUBLIC common)


#---------------------------------------------------------------------------
# bcmc_core: RTL == Python
#---------------------------------------------------------------------------

add_executable(bcmc_core_test bcmc_core_test.cpp)
target_link_libraries(bcmc_core_test PRIVATE bcmc_sim_common)

verilate(bcmc_core_test
  SOURCES ${BCMC_RTL}/bcmc_core.v
  TOP_MODULE bcmc_core
  PREFIX Vbcmc_core
  TRACE
  VERILATOR_ARGS -Wall)

#---------------------------------------------------------------------------
# bcmc_cell: RTL == Python, exhaustively
#---------------------------------------------------------------------------

add_executable(bcmc_cell_test bcmc_cell_test.cpp)
target_link_libraries(bcmc_cell_test PRIVATE bcmc_sim_common)

verilate(bcmc_cell_test
  SOURCES ${BCMC_RTL}/bcmc_cell.v
  TOP_MODULE bcmc_cell
  PREFIX Vbcmc_cell
  TRACE
  VERILATOR_ARGS -Wall)

#---------------------------------------------------------------------------
# bcmc_row and bcmc_column: the two projections
#
# Each of these binaries verilates TWO tops: the projection under test, and a
# separate bcmc_cell to compare it against. That is what makes the replication
# claim testable rather than merely asserted -- see the header of
# sim/bcmc_row_test.cpp.
#
# MAX_N and MAX_C are elaboration-time bounds, so the same number must reach
# Verilator (as -G) and the C++ (as -D). One variable each, used twice.
#---------------------------------------------------------------------------

set(BCMC_MAX_N 32)   # widest row bcmc_row can hold
set(BCMC_MAX_C 32)   # most rows bcmc_column can hold
set(BCMC_VAL_W 16)   # width of N, the weights, the offsets, the column index

add_executable(bcmc_row_test bcmc_row_test.cpp)
target_link_libraries(bcmc_row_test PRIVATE bcmc_sim_common)
target_compile_definitions(bcmc_row_test PRIVATE
  BCMC_ROW_MAX_N=${BCMC_MAX_N})

verilate(bcmc_row_test
  SOURCES ${BCMC_RTL}/bcmc_row.v ${BCMC_RTL}/bcmc_cell.v
  TOP_MODULE bcmc_row
  PREFIX Vbcmc_row
  TRACE
  VERILATOR_ARGS -Wall -GVAL_W=${BCMC_VAL_W} -GMAX_N=${BCMC_MAX_N})

verilate(bcmc_row_test
  SOURCES ${BCMC_RTL}/bcmc_cell.v
  TOP_MODULE bcmc_cell
  PREFIX Vbcmc_cell
  VERILATOR_ARGS -Wall -GVAL_W=${BCMC_VAL_W})

add_executable(bcmc_column_test bcmc_column_test.cpp)
target_link_libraries(bcmc_column_test PRIVATE bcmc_sim_common)
target_compile_definitions(bcmc_column_test PRIVATE
  BCMC_COLUMN_MAX_C=${BCMC_MAX_C}
  BCMC_COLUMN_VAL_W=${BCMC_VAL_W})

verilate(bcmc_column_test
  SOURCES ${BCMC_RTL}/bcmc_column.v ${BCMC_RTL}/bcmc_cell.v
  TOP_MODULE bcmc_column
  PREFIX Vbcmc_column
  TRACE
  VERILATOR_ARGS -Wall -GVAL_W=${BCMC_VAL_W} -GIDX_W=${BCMC_VAL_W}
                 -GMAX_C=${BCMC_MAX_C})

verilate(bcmc_column_test
  SOURCES ${BCMC_RTL}/bcmc_cell.v
  TOP_MODULE bcmc_cell
  PREFIX Vbcmc_cell
  VERILATOR_ARGS -Wall -GVAL_W=${BCMC_VAL_W})

#---------------------------------------------------------------------------
# bcmc_context: storage, arbitration, and composition with the Core
#
# This binary verilates THREE tops:
#
#   Vbcmc_context      the module under test
#   Vbcmc_core         a real Core, so that the offsets the context stores are
#                      the ones reference.py computed rather than ones the
#                      harness made up
#   Vbcmc_context_nc   the same module with -DSYNTHESIS, so its simulation
#                      assertions are compiled out
#
# The third needs explaining. The arbitration suite drives inputs the register
# map denies at the bus -- a weight write while the Core owns the context, an
# out-of-range index -- so against the ordinary elaboration the assertions
# would stop the run, correctly, before the guard behind them could be
# observed. The assertions are the wrapper's contract; the guards are the
# defence in depth. Checking the defence means silencing the alarm.
#---------------------------------------------------------------------------

add_executable(bcmc_context_test bcmc_context_test.cpp)
target_link_libraries(bcmc_context_test PRIVATE bcmc_sim_common)
target_compile_definitions(bcmc_context_test PRIVATE
  BCMC_CONTEXT_MAX_C=${BCMC_MAX_C}
  BCMC_CONTEXT_VAL_W=${BCMC_VAL_W})

verilate(bcmc_context_test
  SOURCES ${BCMC_RTL}/bcmc_context.v
  TOP_MODULE bcmc_context
  PREFIX Vbcmc_context
  TRACE
  VERILATOR_ARGS -Wall -GVAL_W=${BCMC_VAL_W} -GIDX_W=${BCMC_VAL_W}
                 -GMAX_C=${BCMC_MAX_C})

verilate(bcmc_context_test
  SOURCES ${BCMC_RTL}/bcmc_context.v
  TOP_MODULE bcmc_context
  PREFIX Vbcmc_context_nc
  VERILATOR_ARGS -Wall -DSYNTHESIS -GVAL_W=${BCMC_VAL_W} -GIDX_W=${BCMC_VAL_W}
                 -GMAX_C=${BCMC_MAX_C})

verilate(bcmc_context_test
  SOURCES ${BCMC_RTL}/bcmc_core.v
  TOP_MODULE bcmc_core
  PREFIX Vbcmc_core
  VERILATOR_ARGS -Wall -GVAL_W=${BCMC_VAL_W} -GIDX_W=${BCMC_VAL_W})

#---------------------------------------------------------------------------
# bcmc_wb: the register map, on real wires
#
# The whole design is under test here, because the wrapper instantiates it:
# bcmc_wb -> bcmc_context -> bcmc_core, and bcmc_wb -> bcmc_column ->
# bcmc_cell. Only one top is verilated, and that is the point -- there is
# nothing left to compare it against inside the fabric. The comparison is with
# validation/bcmc_periph.py, and it arrives as a recorded conversation.
#
# The geometry is NOT the shared BCMC_MAX_C above. A recording contains
# addresses that exist only for one MAX_C (F2 lives at WEIGHT_BASE + 4*MAX_C),
# so these numbers must match the ones at the top of
# validation/gen_wb_vectors.py. Nothing enforces that at build time, and
# nothing needs to: every recording begins by reading CAPS, which encodes all
# three, so a mismatch fails on the third op of the first file.
#---------------------------------------------------------------------------

set(BCMC_WB_MAX_C 64)
set(BCMC_WB_VAL_W 16)
set(BCMC_WB_IDX_W 16)

add_executable(bcmc_wb_test bcmc_wb_test.cpp)
target_link_libraries(bcmc_wb_test PRIVATE bcmc_sim_common)

verilate(bcmc_wb_test
  SOURCES ${BCMC_RTL}/bcmc_wb.v
  TOP_MODULE bcmc_wb
  PREFIX Vbcmc_wb
  TRACE
  INCLUDE_DIRS ${BCMC_RTL}
  VERILATOR_ARGS -Wall -y ${BCMC_RTL} -GVAL_W=${BCMC_WB_VAL_W}
                 -GIDX_W=${BCMC_WB_IDX_W} -GMAX_C=${BCMC_WB_MAX_C})

#---------------------------------------------------------------------------
# sw/bcmc.c: the driver, against the peripheral
#
# The only target here that compiles software rather than hardware, and the
# only one whose sources leave sim/. ../sw/bcmc.c is built as C99 -- see
# project(bcmc_sim C CXX) above -- and linked to a C++ harness that hands it
# two function pointers reaching the verilated bcmc_wb. Nothing about the
# peripheral is modelled twice.
#
# The geometry is the wrapper's, not the shared BCMC_MAX_C, because the
# wrapper is what is being driven. It reaches Verilator as -G and the harness
# as -D from the same three variables, and bcmc_probe() then rediscovers it
# from CAPS at run time -- so the first thing the test checks is that the
# discovery and the elaboration agree.
#
# There is no Icarus counterpart to this target, and there should not be: the
# thing under test is a C translation unit, and Verilog simulators do not
# compile C. Every other module in rtl/ is checked by two independent
# simulators; sw/bcmc.c is checked by two independent compilers instead
# (see docs/Verification.md 3f).
#---------------------------------------------------------------------------

add_executable(bcmc_driver_test bcmc_driver_test.cpp ${BCMC_SW}/bcmc.c)
target_link_libraries(bcmc_driver_test PRIVATE bcmc_sim_common)
target_include_directories(bcmc_driver_test PRIVATE ${BCMC_SW})
target_compile_definitions(bcmc_driver_test PRIVATE
  BCMC_DRIVER_MAX_C=${BCMC_WB_MAX_C}
  BCMC_DRIVER_VAL_W=${BCMC_WB_VAL_W}
  BCMC_DRIVER_IDX_W=${BCMC_WB_IDX_W})

# The driver is held to the warnings a bare-metal build would use. -Wconversion
# matters most: the register map is a set of narrow fields in 32-bit words, and
# a silent narrowing is exactly the bug this file must not have.
target_compile_options(bcmc_driver_test PRIVATE
  $<$<COMPILE_LANGUAGE:C>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow;-Wstrict-prototypes;-Wmissing-prototypes>)

verilate(bcmc_driver_test
  SOURCES ${BCMC_RTL}/bcmc_wb.v
  TOP_MODULE bcmc_wb
  PREFIX Vbcmc_wb
  TRACE
  INCLUDE_DIRS ${BCMC_RTL}
  VERILATOR_ARGS -Wall -y ${BCMC_RTL} -GVAL_W=${BCMC_WB_VAL_W}
                 -GIDX_W=${BCMC_WB_IDX_W} -GMAX_C=${BCMC_WB_MAX_C})

#---------------------------------------------------------------------------
# sw/bcmc_observer.c: traversal, over the driver, over the peripheral
#
# Three layers of real code and no model anywhere: the observer calls the
# driver, the driver drives the verilated wrapper. That is the only arrangement
# in which "an observer contributes order and nothing else" is a statement
# about this implementation rather than about a diagram.
#
# It links bcmc.c as well, because an observer that supplied its own bus access
# would not be an observer -- docs/Observers.md says it may not reach past its
# interface, and the interface is sw/bcmc.h.
#
# Like the driver target there is no Icarus counterpart, for the same reason:
# the thing under test is a C translation unit. scripts/run_sim.sh compiles it
# under every C compiler on the machine instead.
#---------------------------------------------------------------------------

add_executable(bcmc_observer_test bcmc_observer_test.cpp
               ${BCMC_SW}/bcmc.c ${BCMC_SW}/bcmc_observer.c)
target_link_libraries(bcmc_observer_test PRIVATE bcmc_sim_common)
target_include_directories(bcmc_observer_test PRIVATE ${BCMC_SW})
target_compile_definitions(bcmc_observer_test PRIVATE
  BCMC_DRIVER_MAX_C=${BCMC_WB_MAX_C}
  BCMC_DRIVER_VAL_W=${BCMC_WB_VAL_W}
  BCMC_DRIVER_IDX_W=${BCMC_WB_IDX_W})

target_compile_options(bcmc_observer_test PRIVATE
  $<$<COMPILE_LANGUAGE:C>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow;-Wstrict-prototypes;-Wmissing-prototypes>)

verilate(bcmc_observer_test
  SOURCES ${BCMC_RTL}/bcmc_wb.v
  TOP_MODULE bcmc_wb
  PREFIX Vbcmc_wb
  TRACE
  INCLUDE_DIRS ${BCMC_RTL}
  VERILATOR_ARGS -Wall -y ${BCMC_RTL} -GVAL_W=${BCMC_WB_VAL_W}
                 -GIDX_W=${BCMC_WB_IDX_W} -GMAX_C=${BCMC_WB_MAX_C})

#---------------------------------------------------------------------------
# The examples
#
# Three applications, each built here so that it runs against the verilated
# bcmc_wb rather than against a model of it. The stack is exactly the one
# docs/Observers.md describes and nothing is stubbed out:
#
#     examples/<app>/<app>.c        the application: purpose
#     examples/common/example_*.c   the shared command line and the traversal
#     sw/bcmc_observer.c            the observer: order
#     sw/bcmc.c                     the driver: transactions
#     sim/example_host.cpp          the host seam, over Vbcmc_wb
#     rtl/bcmc_wb.v                 the peripheral
#
# The applications are C and know nothing about Verilator; example_host.cpp is
# the only C++ in the link, and its board-side twin is
# examples/common/example_host_mmio.c. Which traversal an application gets is a
# command-line argument, so the same binary exercises the whole
# Application x Traversal product -- that is what the ctest entries below walk.
#
# The strict-C options are the same set the driver and observer targets use: an
# example that only compiled cleanly with warnings off would be a poor advert
# for a driver that compiles cleanly everywhere.
#---------------------------------------------------------------------------

set(BCMC_EXAMPLE_APPS matrix_dump gpio_scheduler heater_controller)

foreach(app ${BCMC_EXAMPLE_APPS})
  add_executable(example_${app}
                 ${BCMC_EXAMPLES}/${app}/${app}.c
                 ${BCMC_EXAMPLES}/common/example_config.c
                 ${BCMC_EXAMPLES}/common/example_traversal.c
                 example_host.cpp
                 ${BCMC_SW}/bcmc.c
                 ${BCMC_SW}/bcmc_observer.c)

  target_link_libraries(example_${app} PRIVATE bcmc_sim_common)
  target_include_directories(example_${app} PRIVATE
                             ${BCMC_SW} ${BCMC_EXAMPLES}/common)

  target_compile_options(example_${app} PRIVATE
    $<$<COMPILE_LANGUAGE:C>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow;-Wstrict-prototypes;-Wmissing-prototypes>)

  verilate(example_${app}
    SOURCES ${BCMC_RTL}/bcmc_wb.v
    TOP_MODULE bcmc_wb
    PREFIX Vbcmc_wb
    TRACE
    INCLUDE_DIRS ${BCMC_RTL}
    VERILATOR_ARGS -Wall -y ${BCMC_RTL} -GVAL_W=${BCMC_WB_VAL_W}
                   -GIDX_W=${BCMC_WB_IDX_W} -GMAX_C=${BCMC_WB_MAX_C})
endforeach()

#---------------------------------------------------------------------------
# Tests
#
# Each vector file is a separate ctest entry so that a failure names the
# family of inputs that broke.
#---------------------------------------------------------------------------


enable_testing()

set(BCMC_CORE_VECTORS   core_edge core_random core_biased core_large)
set(BCMC_CELL_VECTORS   cell_edge cell_exhaustive cell_random)
set(BCMC_MATRIX_VECTORS matrix_edge matrix_exhaustive matrix_random)

foreach(vec ${BCMC_CORE_VECTORS})
  if(EXISTS ${BCMC_VECTORS}/${vec}.txt)
    add_test(NAME ${vec}
             COMMAND bcmc_core_test ${BCMC_VECTORS}/${vec}.txt)
  else()
    message(WARNING
      "missing ${BCMC_VECTORS}/${vec}.txt -- run: "
      "cd validation && python3 gen_vectors.py")
  endif()
endforeach()

foreach(vec ${BCMC_CELL_VECTORS})
  if(EXISTS ${BCMC_VECTORS}/${vec}.txt)
    add_test(NAME ${vec}
             COMMAND bcmc_cell_test ${BCMC_VECTORS}/${vec}.txt)
  else()
    message(WARNING
      "missing ${BCMC_VECTORS}/${vec}.txt -- run: "
      "cd validation && python3 gen_vectors.py")
  endif()
endforeach()

# The two projections read the same matrix suites: the same bits, seen once
# along the rows and once down the columns. The context reads them a third
# time, for the (weights, offsets) pair rather than for the bits.
foreach(vec ${BCMC_MATRIX_VECTORS})
  if(EXISTS ${BCMC_VECTORS}/${vec}.txt)
    add_test(NAME row_${vec}
             COMMAND bcmc_row_test ${BCMC_VECTORS}/${vec}.txt)
    add_test(NAME column_${vec}
             COMMAND bcmc_column_test ${BCMC_VECTORS}/${vec}.txt)
    add_test(NAME context_${vec}
             COMMAND bcmc_context_test ${BCMC_VECTORS}/${vec}.txt)
  else()
    message(WARNING
      "missing ${BCMC_VECTORS}/${vec}.txt -- run: "
      "cd validation && python3 gen_vectors.py")
  endif()
endforeach()

# The strongest statement available about any module in this project: every
# legal (N, weight, offset, column) tuple for N <= 40, needing no vector file.
add_test(NAME cell_sweep_40
         COMMAND bcmc_cell_test --sweep 40)
set_tests_properties(cell_sweep_40 PROPERTIES TIMEOUT 600)

# The bus conversations. Separate entries for the same reason as above: a
# failure should name which conversation broke, not merely that one did.
foreach(vec wb_sequences wb_busy wb_random)
  if(EXISTS ${BCMC_VECTORS}/${vec}.txt)
    add_test(NAME ${vec}
             COMMAND bcmc_wb_test ${BCMC_VECTORS}/${vec}.txt)
  else()
    message(WARNING
      "missing ${BCMC_VECTORS}/${vec}.txt -- run: "
      "cd validation && python3 gen_wb_vectors.py")
  endif()
endforeach()

# The driver, over the same matrices the two projections use -- every case, not
# a sample. Reading a whole matrix through a 32-bit bus costs hundreds of
# accesses per case, which sounded expensive and is not: the three families
# together are about a quarter of a million bus cycles and under a second.
# There is no reason to check a driver on less than all of it.
foreach(vec ${BCMC_MATRIX_VECTORS})
  if(EXISTS ${BCMC_VECTORS}/${vec}.txt)
    add_test(NAME driver_${vec}
             COMMAND bcmc_driver_test ${BCMC_VECTORS}/${vec}.txt)
    set_tests_properties(driver_${vec} PROPERTIES TIMEOUT 900)
  else()
    message(WARNING
      "missing ${BCMC_VECTORS}/${vec}.txt -- run: "
      "cd validation && python3 gen_vectors.py")
  endif()
endforeach()

# The observers, over the same matrices again -- a fourth reading of the same
# bits, which is the point: an observer that agreed with reference.py on its
# own vectors and nowhere else would have proved nothing. Each case is walked
# three times (sequential, permuted, and the permuted pass rewound), so the
# timeout is the driver's with room to spare.
set(BCMC_OBSERVER_PRNG  ${BCMC_VECTORS}/observer_prng.txt)
set(BCMC_OBSERVER_ORDER ${BCMC_VECTORS}/observer_order.txt)

if(EXISTS ${BCMC_OBSERVER_PRNG} AND EXISTS ${BCMC_OBSERVER_ORDER})
  foreach(vec ${BCMC_MATRIX_VECTORS})
    if(EXISTS ${BCMC_VECTORS}/${vec}.txt)
      add_test(NAME observer_${vec}
               COMMAND bcmc_observer_test
                       --prng ${BCMC_OBSERVER_PRNG}
                       --order ${BCMC_OBSERVER_ORDER}
                       ${BCMC_VECTORS}/${vec}.txt)
      set_tests_properties(observer_${vec} PROPERTIES TIMEOUT 1800)
    endif()
  endforeach()
else()
  message(WARNING
    "missing ${BCMC_OBSERVER_PRNG} or ${BCMC_OBSERVER_ORDER} -- run: "
    "cd validation && python3 gen_observer_vectors.py")
endif()


# Optional soak files, produced by `python3 gen_vectors.py --big`.
if(EXISTS ${BCMC_VECTORS}/core_random_10k.txt)
  add_test(NAME core_random_10k
           COMMAND bcmc_core_test ${BCMC_VECTORS}/core_random_10k.txt)
  set_tests_properties(core_random_10k PROPERTIES TIMEOUT 1800)
endif()

if(EXISTS ${BCMC_VECTORS}/cell_exhaustive_32.txt)
  add_test(NAME cell_exhaustive_32
           COMMAND bcmc_cell_test ${BCMC_VECTORS}/cell_exhaustive_32.txt)
  set_tests_properties(cell_exhaustive_32 PROPERTIES TIMEOUT 1800)
endif()

foreach(vec matrix_exhaustive_6 matrix_random_5k)
  if(EXISTS ${BCMC_VECTORS}/${vec}.txt)
    add_test(NAME row_${vec}
             COMMAND bcmc_row_test ${BCMC_VECTORS}/${vec}.txt)
    add_test(NAME column_${vec}
             COMMAND bcmc_column_test ${BCMC_VECTORS}/${vec}.txt)
    set_tests_properties(row_${vec} column_${vec} PROPERTIES TIMEOUT 3600)
  endif()
endforeach()

# Short traced runs, so that there is always a waveform to look at.
add_test(NAME core_trace
         COMMAND bcmc_core_test --limit 6
                 --vcd ${BCMC_WAVES}/bcmc_core.vcd
                 ${BCMC_VECTORS}/core_edge.txt)

add_test(NAME cell_trace
         COMMAND bcmc_cell_test --limit 24
                 --vcd ${BCMC_WAVES}/bcmc_cell.vcd
                 ${BCMC_VECTORS}/cell_edge.txt)

add_test(NAME row_trace
         COMMAND bcmc_row_test --limit 8
                 --vcd ${BCMC_WAVES}/bcmc_row.vcd
                 ${BCMC_VECTORS}/matrix_edge.txt)

add_test(NAME column_trace
         COMMAND bcmc_column_test --limit 8
                 --vcd ${BCMC_WAVES}/bcmc_column.vcd
                 ${BCMC_VECTORS}/matrix_edge.txt)

add_test(NAME context_trace
         COMMAND bcmc_context_test --limit 4
                 --vcd ${BCMC_WAVES}/bcmc_context.vcd
                 ${BCMC_VECTORS}/matrix_edge.txt)

# One case is enough here: the waveform is for looking at a driver's bus
# traffic, and the second case looks exactly like the first.
add_test(NAME driver_trace
         COMMAND bcmc_driver_test --limit 1
                 --vcd ${BCMC_WAVES}/bcmc_driver.vcd
                 ${BCMC_VECTORS}/matrix_edge.txt)

# A traced pass. Worth having separately from the driver's: this waveform shows
# COLUMN reads arriving in an order the hardware has no opinion about, which is
# the clearest picture of what an observer is.
if(EXISTS ${BCMC_OBSERVER_PRNG} AND EXISTS ${BCMC_OBSERVER_ORDER})
  add_test(NAME observer_trace
           COMMAND bcmc_observer_test --limit 1
                   --vcd ${BCMC_WAVES}/bcmc_observer.vcd
                   --prng ${BCMC_OBSERVER_PRNG}
                   --order ${BCMC_OBSERVER_ORDER}
                   ${BCMC_VECTORS}/matrix_edge.txt)
endif()

# The Application x Traversal product, walked exhaustively. Every application
# is run under every traversal, and each self-check inside an application --
# "exact", "ok" -- prints MISMATCH instead when it fails, so the absence of that
# word is the verdict. These are cheap: a few thousand bus cycles each.
#
# What they do NOT check is that two traversals agree with each other; a single
# process cannot, because it only ran one of them. That comparison is
# scripts/run_examples.sh, which diffs the --summary outputs.
foreach(app ${BCMC_EXAMPLE_APPS})
  foreach(traversal sequential permuted)
    add_test(NAME example_${app}_${traversal}
             COMMAND example_${app} --traversal ${traversal} --rounds 2)
    set_tests_properties(example_${app}_${traversal} PROPERTIES
                         FAIL_REGULAR_EXPRESSION "MISMATCH"
                         TIMEOUT 300)
  endforeach()
endforeach()

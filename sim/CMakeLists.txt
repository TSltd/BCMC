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
project(bcmc_sim CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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

set(BCMC_RTL     ${CMAKE_CURRENT_SOURCE_DIR}/../rtl)
set(BCMC_VECTORS ${CMAKE_CURRENT_SOURCE_DIR}/vectors)
set(BCMC_WAVES   ${CMAKE_CURRENT_SOURCE_DIR}/waves)

#---------------------------------------------------------------------------
# Shared test-vector reader
#---------------------------------------------------------------------------

add_library(bcmc_sim_common STATIC common/vectors.cpp)
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
# along the rows and once down the columns.
foreach(vec ${BCMC_MATRIX_VECTORS})
  if(EXISTS ${BCMC_VECTORS}/${vec}.txt)
    add_test(NAME row_${vec}
             COMMAND bcmc_row_test ${BCMC_VECTORS}/${vec}.txt)
    add_test(NAME column_${vec}
             COMMAND bcmc_column_test ${BCMC_VECTORS}/${vec}.txt)
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

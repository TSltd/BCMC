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
# Tests
#
# Each vector file is a separate ctest entry so that a failure names the
# family of inputs that broke.
#---------------------------------------------------------------------------

enable_testing()

set(BCMC_CORE_VECTORS core_edge core_random core_biased core_large)

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

# Optional soak file, produced by `python3 gen_vectors.py --big`.
if(EXISTS ${BCMC_VECTORS}/core_random_10k.txt)
  add_test(NAME core_random_10k
           COMMAND bcmc_core_test ${BCMC_VECTORS}/core_random_10k.txt)
  set_tests_properties(core_random_10k PROPERTIES TIMEOUT 1800)
endif()

# A short traced run, so that there is always a waveform to look at.
add_test(NAME core_trace
         COMMAND bcmc_core_test --limit 6
                 --vcd ${BCMC_WAVES}/bcmc_core.vcd
                 ${BCMC_VECTORS}/core_edge.txt)

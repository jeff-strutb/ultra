# One library per module; each links only what it needs. `reality_machine` ties them
# together (bus decode and the System), and `reality` is the CLI.

function(reality_library name)
  add_library(${name} STATIC ${ARGN})
  target_include_directories(${name} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(${name} PRIVATE reality_warnings)
endfunction()

reality_library(reality_core
  core/scheduler.cpp
  core/trace.cpp)

reality_library(reality_memory
  memory/rdram.cpp
  memory/ri.cpp
  memory/base_rdram_arbiter.cpp)
target_link_libraries(reality_memory PUBLIC reality_core)

reality_library(reality_rsp rsp/sp_interface.cpp)
target_link_libraries(reality_rsp PUBLIC reality_core reality_memory)

reality_library(reality_rdp rdp/dp_interface.cpp)
target_link_libraries(reality_rdp PUBLIC reality_core)

reality_library(reality_io
  io/mi.cpp
  io/pi.cpp
  io/si.cpp
  io/pif.cpp
  io/pif_boot_rom.cpp
  io/cic.cpp
  io/cart.cpp
  io/vi.cpp
  io/ai.cpp)
target_link_libraries(reality_io PUBLIC reality_core reality_memory)

reality_library(reality_cpu
  cpu/vr4300.cpp
  cpu/execute.cpp
  cpu/cop1.cpp
  cpu/disasm.cpp)
target_link_libraries(reality_cpu PUBLIC reality_core)
# The FPU model controls the host rounding mode and reads host exception flags; the compiler
# must not constant-fold or reorder floating-point operations across those calls.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  set_source_files_properties(cpu/cop1.cpp PROPERTIES COMPILE_OPTIONS "-frounding-math;-ffp-exception-behavior=strict")
else()
  set_source_files_properties(cpu/cop1.cpp PROPERTIES COMPILE_OPTIONS "-frounding-math;-fsignaling-nans")
endif()

reality_library(reality_machine
  core/bus.cpp
  core/system.cpp)
target_link_libraries(reality_machine PUBLIC reality_cpu reality_io reality_rsp reality_rdp reality_memory reality_core)

add_executable(reality app/main.cpp)
target_link_libraries(reality PRIVATE reality_machine fmt::fmt reality_warnings)

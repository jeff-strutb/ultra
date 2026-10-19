# One library per module; each links only what it needs, and the dependency graph has no cycles
# (GNU ld resolves static libraries in link order). `ultra_machine` ties them together (the
# System), and `ultra` is the CLI.

function(ultra_library name)
  add_library(${name} STATIC ${ARGN})
  target_include_directories(${name} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(${name} PRIVATE ultra_warnings)
endfunction()

ultra_library(ultra_core
  core/scheduler.cpp
  core/trace.cpp)

ultra_library(ultra_memory
  memory/rdram.cpp
  memory/ri.cpp
  memory/base_rdram_arbiter.cpp)
target_link_libraries(ultra_memory PUBLIC ultra_core)

ultra_library(ultra_rsp rsp/sp_interface.cpp)
target_link_libraries(ultra_rsp PUBLIC ultra_io ultra_memory ultra_core)

ultra_library(ultra_rdp rdp/dp_interface.cpp)
target_link_libraries(ultra_rdp PUBLIC ultra_core)

ultra_library(ultra_io
  io/mi.cpp
  io/pi.cpp
  io/si.cpp
  io/pif.cpp
  io/pif_boot_rom.cpp
  io/cic.cpp
  io/cart.cpp
  io/vi.cpp
  io/ai.cpp)
target_link_libraries(ultra_io PUBLIC ultra_core ultra_memory)

# CPU-side bus decode to the RCP devices; the CPU issues its transactions through it.
ultra_library(ultra_bus core/bus.cpp)
target_link_libraries(ultra_bus PUBLIC ultra_io ultra_rsp ultra_rdp ultra_memory ultra_core)

ultra_library(ultra_cpu
  cpu/vr4300.cpp
  cpu/execute.cpp
  cpu/cop1.cpp
  cpu/disasm.cpp)
target_link_libraries(ultra_cpu PUBLIC ultra_bus ultra_core)
# The FPU model controls the host rounding mode and reads host exception flags; the compiler
# must not constant-fold or reorder floating-point operations across those calls.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  set_source_files_properties(cpu/cop1.cpp PROPERTIES COMPILE_OPTIONS "-frounding-math;-ffp-exception-behavior=strict")
else()
  set_source_files_properties(cpu/cop1.cpp PROPERTIES COMPILE_OPTIONS "-frounding-math;-fsignaling-nans")
endif()

ultra_library(ultra_machine core/system.cpp)
target_link_libraries(ultra_machine PUBLIC ultra_cpu ultra_bus ultra_io ultra_rsp ultra_rdp ultra_memory
                      ultra_core)

add_executable(ultra app/main.cpp)
target_link_libraries(ultra PRIVATE ultra_machine fmt::fmt ultra_warnings)

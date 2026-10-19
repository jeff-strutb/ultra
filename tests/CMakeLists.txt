add_subdirectory(unit)

add_executable(ultra_harness harness/harness.cpp harness/png.cpp)
target_include_directories(ultra_harness PRIVATE harness)
target_link_libraries(ultra_harness PRIVATE ultra_machine ultra_warnings)

# The ROM gate: every manifest entry must come out exactly as recorded (see docs/timing/ledger.md).
# The krom ROM images are untracked (tests/roms/README.md). A checkout without them fails the gate
# unless ULTRA_ROMS_OPTIONAL is ON, which reports their entries as skipped instead; CI uses it.
option(ULTRA_ROMS_OPTIONAL "Skip ROM-gate entries whose files are missing instead of failing" OFF)
set(_rom_gate_flags "")
if(ULTRA_ROMS_OPTIONAL)
  set(_rom_gate_flags --allow-missing)
endif()
add_test(NAME rom-manifest
         COMMAND ultra_harness manifest ${CMAKE_CURRENT_SOURCE_DIR}/roms/manifest.txt
                 --rom-dir ${CMAKE_CURRENT_SOURCE_DIR}/roms ${_rom_gate_flags})
# The gate opens no trace, so it starts no second thread; the tsan test preset leaves it out
# (it does not finish within this timeout under ThreadSanitizer).
set_tests_properties(rom-manifest PROPERTIES TIMEOUT 1200 LABELS rom)

# Every expected failure is explained in the ledger, and every ledger explanation is still needed.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME ledger-exact COMMAND Python3::Interpreter ${CMAKE_SOURCE_DIR}/tools/check_ledger.py)

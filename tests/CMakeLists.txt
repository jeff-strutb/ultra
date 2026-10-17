add_subdirectory(unit)

add_executable(ultra_harness harness/harness.cpp harness/png.cpp)
target_include_directories(ultra_harness PRIVATE harness)
target_link_libraries(ultra_harness PRIVATE ultra_machine ultra_warnings)

# The ROM gate: every manifest entry must come out exactly as recorded (see docs/timing/ledger.md).
add_test(NAME rom-manifest
         COMMAND ultra_harness manifest ${CMAKE_CURRENT_SOURCE_DIR}/roms/manifest.txt
                 --rom-dir ${CMAKE_CURRENT_SOURCE_DIR}/roms)
set_tests_properties(rom-manifest PROPERTIES TIMEOUT 1200 LABELS rom)

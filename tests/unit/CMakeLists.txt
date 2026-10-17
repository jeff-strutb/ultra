add_executable(reality_unit_tests
  test_core.cpp
  test_cpu.cpp
  test_machine.cpp
  test_scheduler.cpp)
target_link_libraries(reality_unit_tests PRIVATE reality_machine Catch2::Catch2WithMain reality_warnings)
# Machine-level tests boot vendored ROMs (tests/roms/README.md).
target_compile_definitions(reality_unit_tests PRIVATE REALITY_TEST_ROMS="${CMAKE_SOURCE_DIR}/tests/roms")
include(Catch)
catch_discover_tests(reality_unit_tests)

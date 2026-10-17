add_executable(ultra_unit_tests
  test_core.cpp
  test_cpu.cpp
  test_machine.cpp
  test_scheduler.cpp)
target_link_libraries(ultra_unit_tests PRIVATE ultra_machine Catch2::Catch2WithMain ultra_warnings)
# Machine-level tests boot vendored ROMs (tests/roms/README.md).
target_compile_definitions(ultra_unit_tests PRIVATE ULTRA_TEST_ROMS="${CMAKE_SOURCE_DIR}/tests/roms")
include(Catch)
catch_discover_tests(ultra_unit_tests)

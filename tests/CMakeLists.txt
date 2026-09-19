# Backpressure Fabric - test suite.
#
# No test declares a timeout. Tests are run plainly and are allowed to complete
# naturally; a non-terminating test is a defect to diagnose.

set(BPFAB_TEST_SOURCES
  main.cpp
  framework.cpp
  test_core.cpp
  test_model.cpp
  test_engine.cpp
  test_authority.cpp
  test_multisource.cpp
  test_damping.cpp
  test_stale.cpp
  test_store.cpp
  test_ipc.cpp
  test_multiprocess.cpp
  test_concurrency.cpp
  test_property.cpp
  test_adversarial.cpp
  test_bench.cpp
  test_hardening.cpp
)

add_executable(bpfab_tests ${BPFAB_TEST_SOURCES})
target_link_libraries(bpfab_tests PRIVATE backpressure_fabric)
bpfab_apply_warnings(bpfab_tests)
target_compile_options(bpfab_tests PRIVATE ${BPFAB_SANITIZER_FLAGS})
target_link_options(bpfab_tests PRIVATE ${BPFAB_SANITIZER_LINK_FLAGS})
target_include_directories(bpfab_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(bpfab_tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)

add_test(NAME bpfab_tests COMMAND bpfab_tests)
FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.15.3
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(Catch2)

add_executable(lyflog_tests unit_tests.cpp)
target_link_libraries(lyflog_tests PRIVATE lyflog Catch2::Catch2WithMain)

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)
catch_discover_tests(lyflog_tests
  TEST_PREFIX "unit."
  PROPERTIES TIMEOUT 30
)

option(LYFLOG_BUILD_BENCHMARKS "Build lyflog performance benchmarks" ON)
if(LYFLOG_BUILD_BENCHMARKS)
  add_executable(lyflog_benchmark benchmark.cpp)
  target_link_libraries(lyflog_benchmark PRIVATE lyflog)
endif()

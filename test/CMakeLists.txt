add_executable(unit_tests
  test_log_config.cpp
  test_logger_level.cpp
  test_log_formatter.cpp
  test_log_queue.cpp
  test_file_sink_rotate.cpp
  test_http_sink.cpp
)

target_include_directories(unit_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(unit_tests PRIVATE GTest::gtest_main)

add_test(
  NAME unit_tests
  COMMAND unit_tests
)

# CMakeLists.txt
cmake_minimum_required(VERSION 3.20) # cmake的最低版本要求
project(code_template_cmake CXX) # 项目名称 CXX表示C++项目
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

set(CMAKE_CXX_STANDARD_REQUIRED ON) # C++标准要求
set(CMAKE_CXX_STANDARD 20) # 设置C++标准版本

# # 启用 Address Sanitizer
# option(ENABLE_ASAN "Enable Address Sanitizer" OFF)

# if(ENABLE_ASAN)
# if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang|GNU")
# add_compile_options(-fsanitize=address -fno-omit-frame-pointer -g)
# add_link_options(-fsanitize=address)
# endif()
# endif()

# 使用 O3 优化
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3")

# 在配置阶段执行命令，将配置文件复制到build文件夹下
execute_process(COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/config.toml ${CMAKE_BINARY_DIR}/config.toml)

include(FetchContent)
FetchContent_Declare(
  gflags
  URL https://github.com/gflags/gflags/archive/refs/tags/v2.2.2.zip
)
set(GFLAGS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(GFLAGS_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(gflags)

# 添加可执行文件
add_executable(main main.cpp)

add_executable(http_sink_example example_http_sink.cpp)

# 添加包含目录
target_include_directories(main PRIVATE ${PROJECT_SOURCE_DIR}/include) # 添加头文件
target_link_libraries(main PRIVATE gflags::gflags)

target_include_directories(http_sink_example PRIVATE ${PROJECT_SOURCE_DIR}/include) # 添加头文件

option(BUILD_BENCHMARKS "Build benchmarks" ON)

if(BUILD_BENCHMARKS)
  FetchContent_Declare(
    benchmark
    URL https://github.com/google/benchmark/archive/refs/tags/v1.8.3.zip
  )
  set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(benchmark)

  add_subdirectory(benchmark)
endif()

include(CTest)

if(BUILD_TESTING)
  FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
  )
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  add_subdirectory(test)
endif()

# CMakeLists.txt
cmake_minimum_required(VERSION 3.20) # cmake的最低版本要求
project(lyflog CXX) # 项目名称 CXX表示C++项目
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

include(FetchContent)

FetchContent_Declare(
  fmt
  GIT_REPOSITORY https://github.com/fmtlib/fmt.git
  GIT_TAG 12.1.0
  GIT_SHALLOW TRUE
)
set(FMT_TEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(fmt)

FetchContent_Declare(
  concurrentqueue
  GIT_REPOSITORY https://github.com/cameron314/concurrentqueue.git
  GIT_TAG v1.0.4
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(concurrentqueue)

add_library(lyflog INTERFACE)
target_include_directories(lyflog INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${concurrentqueue_SOURCE_DIR}
)
target_link_libraries(lyflog INTERFACE fmt::fmt)

# 添加可执行文件
add_executable(example example.cpp)
target_link_libraries(example PRIVATE lyflog)

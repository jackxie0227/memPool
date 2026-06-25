# 设置 CMake 最小版本要求
cmake_minimum_required(VERSION 3.10)

# 设置项目名称
project(ConcurrentMemoryPool)

# 设置 C++ 标准为 C++17
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# 追加编译选项：开启 O2 优化和警告
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -Wall")

# 链接系统的线程库 pthread
find_package(Threads REQUIRED)

set(MEMPOOL_SOURCES
    CentralCache.cpp
    PageCache.cpp
    ThreadCache.cpp
    ConcurrentAlloc.cpp
    MallocReplacement.cpp
)

add_library(mempool SHARED ${MEMPOOL_SOURCES})
target_link_libraries(mempool PUBLIC Threads::Threads)
target_compile_features(mempool PUBLIC cxx_std_17)

add_executable(mempool_benchmark benchmark.cpp)
target_link_libraries(mempool_benchmark PRIVATE mempool)

add_executable(system_benchmark benchmark.cpp)
target_link_libraries(system_benchmark PRIVATE Threads::Threads)

add_executable(gperftools_benchmark benchmark.cpp)
target_link_libraries(gperftools_benchmark PRIVATE tcmalloc Threads::Threads)

add_executable(standard_alloc_test standard_alloc_test.cpp)
target_link_libraries(standard_alloc_test PRIVATE mempool)

add_executable(standard_alloc_plain standard_alloc_test.cpp)

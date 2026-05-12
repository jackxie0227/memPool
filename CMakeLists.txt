# 设置 CMake 最小版本要求
cmake_minimum_required(VERSION 3.10)

# 设置项目名称
project(ConcurrentMemoryPool)

# 设置 C++ 标准为 C++17
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# 追加编译选项：开启 O2 优化和警告
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -Wall")

# 查找当前目录下所有的源文件并存入 SOURCES 变量
file(GLOB SOURCES "*.cpp")

# 指定生成可执行文件 mempool
add_executable(mempool ${SOURCES})

# 链接系统的线程库 pthread
find_package(Threads REQUIRED)
target_link_libraries(mempool Threads::Threads)
cmake_minimum_required(VERSION 3.10)

project(rcu_qsbr_demo
        VERSION 1.0
        LANGUAGES C)

# C11 is required for <stdatomic.h> (atomics) and _Atomic.
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# POSIX threads (pthreads), the only external dependency. No DPDK.
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# The RCU library: the QS variable + deferred-delete FIFO.
add_library(rcu_qsbr STATIC src/rcu_qsbr.c)
target_include_directories(rcu_qsbr PUBLIC include)
target_compile_options(rcu_qsbr PRIVATE -Wall -Wextra)

# The demo executable.
add_executable(rcu_demo src/main.c)
target_link_libraries(rcu_demo PRIVATE rcu_qsbr Threads::Threads)
target_compile_options(rcu_demo PRIVATE -Wall -Wextra)

# `ctest` runs the demo as a smoke test.
enable_testing()
add_test(NAME rcu_demo_runs COMMAND rcu_demo)

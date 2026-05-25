cmake_minimum_required(VERSION 3.10)

project(rcu_qsbr_demo
        VERSION 1.0
        LANGUAGES C)

# C11 is required for <stdatomic.h> (atomics) and _Atomic.
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

# POSIX threads (pthreads), the only external dependency on Unix.
# On Windows/MSVC the implementation uses the native Win32 thread API,
# so pthreads is not needed there.
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# Warning flags differ between GCC/Clang and MSVC.
if(MSVC)
    set(RCU_WARN_FLAGS /W4)
    # MSVC needs C11 atomics explicitly enabled (VS 2022 17.5+).
    set(RCU_C_FLAGS /experimental:c11atomics)
else()
    set(RCU_WARN_FLAGS -Wall -Wextra)
    set(RCU_C_FLAGS "")
endif()

# The RCU library: the QS variable + deferred-delete FIFO.
add_library(rcu_qsbr STATIC src/rcu_qsbr.c)
target_include_directories(rcu_qsbr PUBLIC include)
target_compile_options(rcu_qsbr PRIVATE ${RCU_WARN_FLAGS} ${RCU_C_FLAGS})

# The demo executable.
add_executable(rcu_demo src/main.c)
target_link_libraries(rcu_demo PRIVATE rcu_qsbr Threads::Threads)
target_compile_options(rcu_demo PRIVATE ${RCU_WARN_FLAGS} ${RCU_C_FLAGS})

# `ctest` runs the demo as a smoke test.
enable_testing()
add_test(NAME rcu_demo_runs COMMAND rcu_demo)

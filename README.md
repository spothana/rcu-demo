# RCU QSBR demo (no DPDK dependency)

A self-contained C implementation of **Quiescent State Based Reclamation**
(QSBR) RCU, emulating the DPDK `rte_rcu_qsbr` library. It depends only on
C11 `<stdatomic.h>` and POSIX threads — no DPDK.

## What it shows

A `config` object is shared lock-free between reader threads and one
writer. The writer publishes new versions and frees old ones, but only
once every reader has stopped referencing them. There is no lock on the
read path and no use-after-free.

| Concept | API in this project | DPDK equivalent |
|---|---|---|
| Size / init the QS variable | `rcu_qsbr_get_memsize`, `rcu_qsbr_init` | `rte_rcu_qsbr_*` |
| Reader register / online | `rcu_qsbr_thread_register/online` | `rte_rcu_qsbr_thread_*` |
| Reader reports quiescent state | `rcu_qsbr_quiescent` | `rte_rcu_qsbr_quiescent` |
| Reader offline for blocking call | `rcu_qsbr_thread_offline` | `rte_rcu_qsbr_thread_offline` |
| Writer: token + check | `rcu_qsbr_start`, `rcu_qsbr_check` | `rte_rcu_qsbr_start/check` |
| Writer: blocking shortcut | `rcu_qsbr_synchronize` | `rte_rcu_qsbr_synchronize` |
| Deferred-delete FIFO | `rcu_qsbr_dq_*` | `rte_rcu_qsbr_dq_*` |

## Build & run

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/rcu_demo
```

Or run it as a test: `ctest --test-dir build`.

### Windows (Visual Studio / MSVC)

Builds out of the box — no pthreads package needed. `rcu_port.h`
maps threading and sleep onto the native Win32 API on `_WIN32`, and
onto pthreads everywhere else. The C11 `<stdatomic.h>` code is shared.

```cmd
cmake -S . -B build
cmake --build build --config Release
build\Release\rcu_demo.exe
```

MSVC needs Visual Studio 2022 17.5 or newer for C11 atomics; the
CMake file passes `/experimental:c11atomics` automatically.

## Check for use-after-free

GCC / Clang only:

```sh
cmake -S . -B build-asan -DCMAKE_C_FLAGS="-fsanitize=address,undefined -g"
cmake --build build-asan
./build-asan/rcu_demo
```

## Layout

```
include/rcu_qsbr.h   public API
include/rcu_port.h   thread/sleep shim (pthreads or Win32)
src/rcu_qsbr.c       QS variable + deferred-delete FIFO
src/main.c           reader/writer demo
CMakeLists.txt       build (C11, pthreads or Win32)
```

## How it works

Each reader owns a cache-line-padded counter slot. While online, the
reader publishes the variable's global token into its slot at every
quiescent point. `rcu_qsbr_start()` bumps the global token to `T`;
`rcu_qsbr_check(T)` succeeds once every registered, online reader's slot
counter has reached `T` — at which point no reader can still hold a
reference taken before `start()`, so the deleted memory is safe to free.
An offline reader's slot is treated as permanently quiescent and skipped.

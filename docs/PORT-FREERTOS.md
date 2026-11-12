# Relais on FreeRTOS — port notes (mission M32, idea 73)

**Status on the machine this was written on: NOT COMPILED.** No FreeRTOS kernel
checkout and no `arm-none-eabi-gcc` here. The conditional check says so instead
of failing:

```
make -C ports/freertos check     # audit + honest verdict, always exits 0
```

What that check proves without the kernel:

| check | result here |
|---|---|
| every file of the port < 500 lines | OK (3 source files) |
| no allocation / file / clock / raw socket call in the port | OK |
| the build compiles `core/src/*.c`, not a copy of it | OK |
| `ports/common/relais_port.h` compiles with host gcc `-Werror` | OK |
| `cmake` configure + build | SKIPPED, kernel and cross toolchain missing |

The kernel is deliberately **not vendored**: a port that carries a copy of the
RTOS is a fork. `CMakeLists.txt` fails with an explicit message unless
`FREERTOS_KERNEL_PATH` points at a checkout.

## What is here

```
ports/freertos/
  CMakeLists.txt        builds core/src/*.c + host/transport/udp.c + kernel + app
  FreeRTOSConfig.h      static allocation only, 8 priorities, no time slicing
  src/main.c            the three tasks: net, core, ctl
  src/assert.c          what configASSERT does: stop, do not limp on
  check.sh, Makefile    the conditional build check above
```

## Build for real

```bash
git clone https://github.com/FreeRTOS/FreeRTOS-Kernel
cmake -B build -S ports/freertos \
      -DFREERTOS_KERNEL_PATH=$PWD/FreeRTOS-Kernel \
      -DFREERTOS_PORT=GCC_ARM_CM4F \
      -DCMAKE_TOOLCHAIN_FILE=<your-arm-none-eabi.cmake> \
      -DRELAIS_ACTUATOR_ENDPOINT=192.168.1.10:7200 \
      -DRELAIS_PEER_ENDPOINT=192.168.1.12:7202 \
      -DRELAIS_LOCAL_ENDPOINT=:7201
cmake --build build -j 4
```

A hosted build (POSIX/GCC port, `FREERTOS_PORT=GCC_POSIX`) is the fastest way to
smoke-test the logic on a PC, provided a POSIX socket stack is linked in.

## Architecture

Same split as the QNX image and as the Zephyr port — one owner of the socket, one
owner of the protocol, one owner of the law:

| task | FreeRTOS priority | QNX equivalent | stack (words) |
|---|---|---|---|
| `relais-ctl` | `configMAX_PRIORITIES - 1` = 7 | 60 | 512 |
| `relais-core` | `configMAX_PRIORITIES - 2` = 6 | 61 | 1024 |
| `relais-net` | `configMAX_PRIORITIES - 3` = 5 | 62 | 512 |

**Priority mapping rule:** QNX ranks *lower is stronger*, FreeRTOS ranks *higher
is stronger*, so one rule converts them and preserves the ordering:

```
freertos_prio(qnx) = configMAX_PRIORITIES - 1 - (qnx - 60)
```

Three kernel settings are protocol settings, not taste:

* `configSUPPORT_STATIC_ALLOCATION = 1`, `configSUPPORT_DYNAMIC_ALLOCATION = 0`,
  `configTOTAL_HEAP_SIZE = 0`. Every queue and task buffer is a static object, so
  the "no allocation" rule that `core/` respects holds for the whole image.
* `configMAX_PRIORITIES = 8`, which leaves the three Relais tasks above any
  application task.
* `configUSE_TIME_SLICING = 0`: peers of one protocol, not tenants of a CPU. A
  tick is never cut in the middle by a round robin.

The core talks to the law through `relais_law` (CMD-09): `law_step()` sends a
request on a depth-1 static queue and blocks on the reply queue. One client, so
the pair is a lock-step rendez-vous — no mutex, no priority inversion, and the
ctl task at priority 7 preempts the core task at 6 to answer immediately, which
is the FreeRTOS image of QNX message passing with priority inheritance.

## What must be adapted for a real target

1. **TCP/IP stack.** `host/transport/udp.c` is compiled unchanged and speaks the
   POSIX socket API, which **lwIP** provides (`socket`, `bind`, `sendto`,
   `recvfrom`, `setsockopt(SO_RCVTIMEO)`, `MSG_DONTWAIT`). On **FreeRTOS+TCP** the
   API is named differently, and the mapping is one-to-one:

   | transport call | POSIX / lwIP | FreeRTOS+TCP |
   |---|---|---|
   | create | `socket(AF_INET, SOCK_DGRAM, 0)` | `FreeRTOS_socket(FREERTOS_AF_INET, FREERTOS_SOCK_DGRAM, FREERTOS_IPPROTO_UDP)` |
   | bind | `bind(fd, …)` | `FreeRTOS_bind(sock, &addr, sizeof addr)` |
   | send | `sendto(fd, buf, len, 0, dst, …)` | `FreeRTOS_sendto(sock, buf, len, 0, dst, …)` |
   | receive | `recvfrom(fd, buf, cap, flags, &src, &srclen)` | `FreeRTOS_recvfrom(sock, buf, cap, flags, &src, &srclen)` |
   | timeout | `setsockopt(SO_RCVTIMEO)` | `FreeRTOS_setsockopt(sock, 0, FREERTOS_SO_RCVTIMEO, &ticks, …)` |
   | close | `close(fd)` | `FreeRTOS_closesocket(sock)` |

   That is a *third* implementation of `relais_transport` (after UDP/POSIX and
   CAN), roughly 80 lines, and **zero change in `core/`**. It is not written here:
   this machine cannot compile FreeRTOS+TCP, and shipping unverifiable code as if
   it were a port would be a lie.
2. **Interrupt priorities.** `FreeRTOSConfig.h` carries the standard Cortex-M4
   block (`configPRIO_BITS`, `configMAX_SYSCALL_INTERRUPT_PRIORITY`). Another core
   needs its own equivalent, and the Ethernet ISR must sit *below*
   `configMAX_SYSCALL_INTERRUPT_PRIORITY` or it may not call the queue API.
3. **Stacks.** 512/1024/512 words are a starting point. `relais_msg` is ~400
   bytes and the net task holds two of them; measure with
   `uxTaskGetStackHighWaterMark()` (enabled) before trusting a number.
4. **Startup.** `main()` opens the transport, creates the four static queues and
   the three static tasks, then calls `vTaskStartScheduler()`. A real product
   needs the network *up* before the transport is opened (`NetworkUp` event /
   `xNetworkBuffers`), otherwise `relais_tr_init()` fails and `main` returns 1.
5. **Key.** `relais_key_clear()` is level S (CRC-32C). Level C needs a provisioned
   key from secure storage, not a compile-time string.

## What is *not* claimed

No image was built, no board ran, no timing was measured, and the FreeRTOS+TCP
variant is not written. What is claimed is structural and checkable here: the
shared core compiles into a FreeRTOS application without modification, the port
allocates nothing, and the three-way split of the QNX image survives the change of
kernel with an explicit, documented priority mapping.

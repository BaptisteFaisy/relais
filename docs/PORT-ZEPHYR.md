# Relais on Zephyr — port notes (mission M32, idea 73)

**Status on the machine this was written on: NOT COMPILED.** There is no Zephyr
SDK here (`west`, `ZEPHYR_BASE`, `ninja`, `dtc` and the SDK toolchain are all
absent). Everything below is delivered as source plus build files, and the
conditional check says so instead of failing:

```
make -C ports/zephyr check          # audit + honest verdict, always exits 0
make -C ports/zephyr check BOARD=qemu_x86
```

What that check *does* prove without the SDK:

| check | result here |
|---|---|
| every file of the port < 500 lines | OK |
| no allocation / file / clock / raw socket call in the port | OK |
| the build compiles `core/src/*.c`, not a copy of it | OK |
| `ports/common/relais_port.h` compiles with host gcc `-Werror` | OK |
| `west build` | SKIPPED, toolchain missing |

## What is here

```
ports/zephyr/
  CMakeLists.txt              compiles the SHARED core/src/*.c and host/transport/udp.c
  Kconfig                     the board wiring only (endpoints, port, A or B, log level)
  prj.conf                    networking, no heap, no time slicing, cooperative priorities
  app.overlay                 deliberately empty of hardware
  boards/qemu_x86.overlay     the emulated e1000 NIC
  boards/native_posix.overlay TAP interface towards the host stack
  src/main.c                  the three threads: net, core, ctl
  check.sh, Makefile          the conditional build check above
```

The protocol parameters (T, p, N, epsilon, delta, …) are **not** in Kconfig: they
live in `core/src/params.c` and are identical on PC, QNX and Zephyr. Only the
wiring of a board is configurable.

## Build for real

```bash
pip install west
west init -l zephyrproject && west update          # or use an existing workspace
west build -b native_posix ports/zephyr --pristine  # or -b qemu_x86
west build -t run                                   # qemu_x86: runs under QEMU
```

To exercise it against the existing PC bench, run the image on `native_posix`
with the TAP driver (`prj.conf` enables `CONFIG_ETH_NATIVE_TAP`), point
`CONFIG_RELAIS_ACTUATOR` at the host side of the TAP, and start the bench:

```bash
sudo ./build/zephyr/zephyr.exe -tap_dev_name tap0
python3 host/bench/relais_bench.py --scenario scenarios/slalom-nominal.json
```

## Architecture

Same split as the QNX image (`target/`), with threads instead of processes:

| thread | Zephyr priority | QNX equivalent | job | requirement |
|---|---|---|---|---|
| `relais-net` | `-1` | 62 | owns the socket, validates every frame, forwards decoded messages | SEC-05, DET-04 |
| `relais-core` | `-2` | 61 | the protocol state machine, role, replica | MAN-*, REP-*, CMD-02 |
| `relais-ctl` | `-3` | 60 | the control law as a service: three functions | CMD-09, CMD-06 |

**Priority mapping rule:** QNX SCHED_FIFO ranks *lower is stronger*; Zephyr
cooperative priorities are negative and *more negative is stronger*. One rule
converts them and preserves the ordering:

```
zephyr_prio(qnx) = -(63 - qnx)        62 -> -1,  61 -> -2,  60 -> -3
```

All three are cooperative, so no tick can be cut in the middle by a preemptible
thread, and `CONFIG_TIMESLICING=n` removes round-robin between equal priorities.
`CONFIG_NUM_COOP_PRIORITIES=8` leaves room for five more cooperative levels.

The core reaches the control law through its own extension point, `relais_law`
(CMD-09): `law_step()` puts a request on a depth-1 message queue and blocks on
the reply queue. One client, so the pair of queues is a lock-step rendez-vous and
needs no mutex — the FreeRTOS port does the same thing with `xQueueSend`.

## What must be adapted for a real board

1. **Network driver.** `prj.conf` enables the native TAP driver and the generic
   Ethernet L2. A real board needs its own MAC/PHY driver enabled, plus a static
   address or DHCP (`CONFIG_NET_CONFIG_MY_IPV4_ADDR`), and its board overlay must
   declare the controller (the `qemu_x86` overlay here assumes `&eth0`).
2. **BSD sockets vs `net_context`.** The port compiles `host/transport/udp.c`
   unchanged, because Zephyr's POSIX socket layer (`CONFIG_NET_SOCKETS`,
   `CONFIG_NET_SOCKETS_POSIX_NAMES`) provides `socket`, `bind`, `sendto`,
   `recvfrom`, `setsockopt(SO_RCVTIMEO)` and `MSG_DONTWAIT`. On a build that
   refuses the POSIX layer, the alternative is a second transport implementation
   over `net_context_get`/`net_context_sendto`/`net_context_recvfrom`: that is
   ~80 lines *behind `relais_transport`*, and **no change in `core/`** — which is
   the property the transport interface exists to guarantee.
3. **Time.** Nothing in the port calls a clock for protocol decisions: the tempo
   comes from the actuator's STATUT (CMD-02) and the only timeouts are
   `SO_RCVTIMEO` and `K_MSEC(period * 3)` for MAN-09 silence. `CONFIG_POSIX_CLOCK`
   is enabled only because the socket layer wants it.
4. **Stacks.** 2048 / 4096 / 2048 bytes are a starting point, not a measurement:
   `relais_msg` is ~400 bytes and the net thread holds two of them on its stack.
   Measure with `CONFIG_THREAD_ANALYZER=y` and `uxTaskGetStackHighWaterMark`-style
   reporting before trusting a number.
5. **Key.** `main()` calls `relais_key_clear()` (level S, CRC-32C). Level C needs
   `relais_key_from_hex()` with a provisioned key — on a real board, from secure
   storage, not from a Kconfig string.

## What is *not* claimed

No image was built, no board ran, no timing was measured. The claims this port
makes are structural: the core compiles as-is into a Zephyr application, the
protocol glue is one shared header, and the split of responsibilities of the QNX
image survives the change of kernel. `make -C ports/zephyr check` prints that
verdict itself.

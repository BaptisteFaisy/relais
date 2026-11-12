# `ports/` — the core, elsewhere (mission M32)

The central claim of this project is that `core/` is a portable C11 library
compiled several times over. This directory tests that claim against other
kernels, another language and other compilers.

| port | what it proves | verdict on this machine |
|---|---|---|
| [`zephyr/`](zephyr/) | the core as a Zephyr application, three cooperative threads (net/core/ctl) at the image of QNX 62/61/60 | **NOT COMPILED** — no Zephyr SDK; audit green, `check` exits 0 |
| [`freertos/`](freertos/) | the core as a FreeRTOS application, three static tasks, no allocation anywhere | **NOT COMPILED** — no kernel checkout, no `arm-none-eabi-gcc`; audit green, `check` exits 0 |
| [`rust/`](rust/) | the arbiter reimplemented in Rust, `no_std`, no `unsafe`, confronted with C tick by tick | **BUILT AND TESTED** — 12 unit tests, clippy clean, 240 025 ticks identical to C |
| [`compilers/`](compilers/) | the core under other compilers and other language standards | **TESTED** — 99 object compilations, 0 warning, 5 tools reported missing |
| [`common/`](common/) | the glue shared by the RTOS ports, and the audit they all run | compiled with host gcc `-Werror` |

One command for all of them:

```bash
make ports          # zephyr + freertos + rust + compilers, each one honest
```

## The rule every port follows

1. **The core is compiled, never copied.** Each build file points at
   `../../core/src/*.c`. `ports/common/audit.sh` fails a port that carries its own
   copy or that re-implements a piece of the protocol.
2. **No allocation, no clock, no file, no socket in the port's own code.** A port
   talks to the network through `core/include/relais/transport.h` and to time
   through its kernel. The audit greps for it.
3. **A missing toolchain is not a red build.** Every `check` target reports what is
   absent, runs whatever *can* be checked, and exits 0. A port that pretends to
   build is worse than a port that says it cannot.
4. **Every claim in the docs is reproducible by a command given in the docs.**

## Where the transport work lives

The transport interface is `core/include/relais/transport.h`; its implementations
and their tests are under `host/transport/`. See
[`../transports/README.md`](../transports/README.md) and
[`../docs/PORT-TRANSPORTS.md`](../docs/PORT-TRANSPORTS.md).

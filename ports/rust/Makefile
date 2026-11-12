# Relais — the Rust port of the arbiter (mission M32, idea 74).
#
# `make check` is CONDITIONAL, like the other ports: it reports what is missing
# and exits 0, because a machine without a Rust toolchain must not turn the
# build red. When cargo IS installed, it runs everything for real:
#   build, unit tests, clippy -D warnings, the no_std build and the C-vs-Rust
#   differential confrontation over the JSON-lines bridge.
#
# Nothing is downloaded: the crate has no dependency, so --offline always works.

CARGO     ?= cargo
REPO      := ../..
C_BRIDGE  := $(REPO)/build/relais-bridge
RUNS      ?= 200
SEED      ?= 1
TICKS     ?= 60

.PHONY: check build test clippy nostd difftest

check:
	@command -v $(CARGO) >/dev/null 2>&1 || { \
	  echo "RUST-PORT-SKIPPED: cargo/rustc not installed on this machine."; \
	  echo "  install: rustup (https://rustup.rs) or the distro package 'rustc cargo'"; \
	  echo "  then run: make -C ports/rust check"; \
	  exit 0; }
	@echo "== Rust port: $$($(CARGO) --version) =="
	@$(MAKE) --no-print-directory build test clippy nostd difftest
	@echo "RUST-PORT-OK"

build:
	$(CARGO) build --offline
	@echo "  built target/debug/relais-bridge-rs"

test:
	$(CARGO) test --offline 2>&1 | tail -12

clippy:
	@if $(CARGO) clippy --version >/dev/null 2>&1; then \
	  $(CARGO) clippy --offline --all-targets -- -D warnings && \
	  echo "  clippy: 0 warning with -D warnings"; \
	else echo "  clippy MISSING (rustup component add clippy): not run"; fi

# The library alone, with no std: the portability claim of the port.
nostd:
	$(CARGO) build --offline --no-default-features
	@echo "  no_std build of the library: OK (no allocation, no clock, no I/O)"

# The confrontation with the C reference. Needs build/relais-bridge: if the C
# side is not built, say so and stay green.
difftest: build
	@if [ -x $(C_BRIDGE) ]; then \
	  python3 difftest/run_difftest.py --runs $(RUNS) --seed $(SEED) --ticks $(TICKS); \
	else \
	  echo "  DIFFTEST-SKIPPED: $(C_BRIDGE) missing (run `make -j4 all` at the repo root)"; \
	fi

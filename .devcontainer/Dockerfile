# Relais — reproducible development container (REP-01, REP-04).
#
# Purpose: let an stranger press "Run in Codespaces" (or `docker build .` then
# `docker run`) and have, without reading the whole repository, the exact set of
# tools that replay every published proof: gcc, make, python3, qemu-system-x86_64
# for the VM demonstration, and cpio + busybox-static to assemble the bootable
# Linux guest described in docs/REPRODUCE.md §"Bootable Linux guest".
#
# It installs nothing beyond standard distribution packages and never runs
# anything with administrator rights inside the container.

FROM mcr.microsoft.com/devcontainers/base:ubuntu-24.04

# -- the proof toolchain -----------------------------------------------------
# gcc/make            : build core/, unit tests, explorer, simulator (levels 0-2)
# python3             : the mutant bench and the deterministic demo (stdlib only)
# qemu-system-x86     : provides qemu-system-x86_64, the VM demonstration layer
# cpio + busybox-static : build the minimal bootable Linux guest used to show a
#                       QMP freeze/thaw without the QNX SDP (see REPRODUCE.md)
RUN apt-get update && export DEBIAN_FRONTEND=noninteractive \
    && apt-get install -y --no-install-recommends \
        build-essential \
        python3 \
        qemu-system-x86 \
        qemu-utils \
        cpio \
        busybox-static \
    && rm -rf /var/lib/apt/lists/*
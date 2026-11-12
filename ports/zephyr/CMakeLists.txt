# Relais on Zephyr (mission M32, idea 73).
#
# Build:   west build -b native_posix ports/zephyr        (see docs/PORT-ZEPHYR.md)
#
# The whole point of this file: core/src/*.c is compiled here exactly as it is on
# the PC and on QNX. Nothing is copied, nothing is generated, nothing is patched.
# The transport implementation host/transport/udp.c is compiled too, because
# Zephyr's POSIX socket layer provides the same calls — that file was NOT
# written for Zephyr.

cmake_minimum_required(VERSION 3.20)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(relais_zephyr C)

get_filename_component(RELAIS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

# The core, compiled as a fourth time (after QNX, the PC simulator, the explorer).
file(GLOB RELAIS_CORE_SRC ${RELAIS_ROOT}/core/src/*.c)

target_include_directories(app PRIVATE
  src
  ${RELAIS_ROOT}/core/include
  ${RELAIS_ROOT}/host/transport
  ${RELAIS_ROOT}/ports/common)

target_sources(app PRIVATE
  src/main.c
  ${RELAIS_ROOT}/host/transport/udp.c
  ${RELAIS_CORE_SRC})

# The core is strict C11 and must stay warning free on this compiler too.
target_compile_options(app PRIVATE -std=c11 -Wall -Wextra -Wshadow -Wconversion
                       -Wno-sign-conversion -fno-strict-aliasing)

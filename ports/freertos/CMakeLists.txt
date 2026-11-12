# Relais on FreeRTOS (mission M32, idea 73).
#
# The kernel is NOT vendored here: a port that carries a copy of the RTOS is a
# fork, not a port. Point FREERTOS_KERNEL_PATH at a checkout and configure:
#
#   cmake -B build -DFREERTOS_KERNEL_PATH=$HOME/FreeRTOS-Kernel \
#         -DFREERTOS_PORT=GCC_ARM_CM4F \
#         -DCMAKE_TOOLCHAIN_FILE=arm-none-eabi.cmake
#   cmake --build build
#
# The core and the UDP transport are the shared ones: this build compiles
# ../../core/src/*.c and ../../host/transport/udp.c, exactly like the PC build,
# the QNX build and the Zephyr port do.

cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED FREERTOS_KERNEL_PATH AND DEFINED ENV{FREERTOS_KERNEL_PATH})
  set(FREERTOS_KERNEL_PATH $ENV{FREERTOS_KERNEL_PATH})
endif()
if(NOT DEFINED FREERTOS_KERNEL_PATH)
  message(FATAL_ERROR
    "FREERTOS_KERNEL_PATH is not set. The kernel is deliberately not vendored: "
    "clone https://github.com/FreeRTOS/FreeRTOS-Kernel and pass -DFREERTOS_KERNEL_PATH=<path>. "
    "See docs/PORT-FREERTOS.md.")
endif()
set(FREERTOS_PORT "GCC_ARM_CM4F" CACHE STRING "FreeRTOS port directory under portable/")

project(relais_freertos C)
get_filename_component(RELAIS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

file(GLOB RELAIS_CORE_SRC ${RELAIS_ROOT}/core/src/*.c)

add_library(relais_core STATIC ${RELAIS_CORE_SRC})
target_include_directories(relais_core PUBLIC ${RELAIS_ROOT}/core/include)
target_compile_options(relais_core PRIVATE -std=c11 -Wall -Wextra -Werror -Wshadow
                       -Wconversion -Wno-sign-conversion -fno-strict-aliasing)

# The UDP transport of host/transport/, unchanged. It speaks the POSIX socket
# API, which lwIP provides; on FreeRTOS+TCP see docs/PORT-FREERTOS.md for the
# mapping table (that is what the transport interface exists for).
add_library(relais_transport STATIC ${RELAIS_ROOT}/host/transport/udp.c)
target_include_directories(relais_transport PUBLIC
  ${RELAIS_ROOT}/host/transport ${RELAIS_ROOT}/core/include)

add_library(freertos_kernel STATIC
  ${FREERTOS_KERNEL_PATH}/tasks.c
  ${FREERTOS_KERNEL_PATH}/queue.c
  ${FREERTOS_KERNEL_PATH}/list.c
  ${FREERTOS_KERNEL_PATH}/timers.c
  ${FREERTOS_KERNEL_PATH}/portable/MemMang/heap_0.c
  ${FREERTOS_KERNEL_PATH}/portable/${FREERTOS_PORT}/port.c)
target_include_directories(freertos_kernel PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${FREERTOS_KERNEL_PATH}/include
  ${FREERTOS_KERNEL_PATH}/portable/${FREERTOS_PORT})

add_executable(relais_freertos src/main.c src/assert.c)
target_include_directories(relais_freertos PRIVATE
  src ${RELAIS_ROOT}/core/include ${RELAIS_ROOT}/host/transport ${RELAIS_ROOT}/ports/common)
target_compile_definitions(relais_freertos PRIVATE
  RELAIS_ACTUATOR_ENDPOINT="${RELAIS_ACTUATOR_ENDPOINT}"
  RELAIS_PEER_ENDPOINT="${RELAIS_PEER_ENDPOINT}"
  RELAIS_LOCAL_ENDPOINT="${RELAIS_LOCAL_ENDPOINT}")
target_link_libraries(relais_freertos PRIVATE relais_core relais_transport freertos_kernel)
target_compile_options(relais_freertos PRIVATE -std=c11 -Wall -Wextra -Werror -Wshadow
                       -Wconversion -Wno-sign-conversion -fno-strict-aliasing)

set(RELAIS_ACTUATOR_ENDPOINT "192.168.1.10:7200" CACHE STRING "actuator host:port")
set(RELAIS_PEER_ENDPOINT "192.168.1.12:7202" CACHE STRING "peer computer host:port")
set(RELAIS_LOCAL_ENDPOINT ":7201" CACHE STRING "local port to bind")

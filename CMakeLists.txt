cmake_minimum_required(VERSION 3.20)
project(sunbright CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# The sole shipping binary. Two render paths inside:
#   Path A (GC-faithful): reference/sms decomp -> Aurora <dolphin/gx.h> -> WebGPU/Dawn (default).
#   Path B (PC-native):   reference/sms decomp -> SDL3-GPU direct (deferred until GC parity closes).
# Select at build time with -DSB_RENDER=gc|pc.
# Aurora (encounter/aurora) — GX-faithful source-level compat layer built on
# SDL3 + WebGPU/Dawn. Consumed by sms-boot/render_gc/ as the Path A backend.
# Enables its GX compat layer + CARD (memory-card) shim; DVD stays off (we
# provide our own DVD seam in sms-boot/common/).
set(AURORA_ENABLE_GX   ON  CACHE BOOL "" FORCE)
set(AURORA_ENABLE_CARD ON  CACHE BOOL "" FORCE)
set(AURORA_ENABLE_DVD  OFF CACHE BOOL "" FORCE)
add_subdirectory(extern/aurora EXCLUDE_FROM_ALL)

set(SMS_BUILD_BOOT ON CACHE BOOL "" FORCE)
add_subdirectory(sms-boot)

cmake_minimum_required(VERSION 3.20)
project(sunbright CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# The sole shipping binary: sms-boot — reference/sms decomp compiled native,
# with Aurora (encounter/aurora, SDL3 + WebGPU/Dawn) providing the whole GC
# platform surface: GX render, DVD (synchronous, nod-backed), CARD, PAD/SI,
# VI, MTX, and host audio out (aurora::audio). One runtime, one thread — see
# sms-boot/CMakeLists.txt.
set(AURORA_ENABLE_GX   ON  CACHE BOOL "" FORCE)
set(AURORA_ENABLE_CARD ON  CACHE BOOL "" FORCE)
set(AURORA_ENABLE_DVD  ON  CACHE BOOL "" FORCE)
add_subdirectory(extern/aurora EXCLUDE_FROM_ALL)

set(SMS_BUILD_BOOT ON CACHE BOOL "" FORCE)
add_subdirectory(sms-boot)

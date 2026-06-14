# =============================================================================
# Super Mario Sunshine — native PC port: build scaffold (FIRST BRICK).
#
# Source-ports the doldecomp/sms decompilation at ../reference/sms to a native
# x86-64 static library (libsmsport_core.a) using a compat-shim layer. This is
# a PARALLEL, INDEPENDENT build from the sunbright recompiler — it does not
# touch ../runtime, ../CMakeLists.txt, or ../build, and it does not modify the
# pristine vendored decomp under ../reference/sms.
#
# Build:
#   cmake -S port -B port/build
#   cmake --build port/build -j$(nproc)
# Output:  port/build/libsmsport_core.a
# =============================================================================
cmake_minimum_required(VERSION 3.16)
project(smsport CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)   # GNU extensions (the decomp relies on a few)

# Repo root = parent of this port/ dir.
get_filename_component(SMS_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(SMS_DECOMP "${SMS_REPO_ROOT}/reference/sms")
set(SMS_COMPAT "${CMAKE_CURRENT_SOURCE_DIR}/compat")

# ---------------------------------------------------------------------------
# Include path ORDER MATTERS (see port/README.md "Shadow-header strategy"):
#   1. port/compat/include  — shadow headers WIN resolution (corrected
#      dolphin/types.h with int-based u32/s32, the J3d->J3D case-fold shim,
#      shadow <stdio.h>/<stdarg.h>, the MSL math.h shadow).
#   2. reference/sms/include — the pristine decomp headers.
# The MetroWerks MSL libc tree (reference/sms/include/PowerPC_EABI_Support/Msl)
# is deliberately NOT added: it shadows the host libc and will not compile.
# Its few needed macros (DEG_TO_RAD, M_PI-as-float, ...) come from the
# force-included msl_shim.h and the MSL math.h shadow.
# ---------------------------------------------------------------------------
set(SMS_INCLUDES
  "${SMS_COMPAT}/include"
  "${SMS_DECOMP}/include")

# Force-include the compat shims into every TU (before any decomp source):
#   intrinsics.h — host implementations of CodeWarrior PPC builtins
#                  (__frsqrte/__fres bit-accurate, __fabs/__cntlzw/__fsel/...).
#   msl_shim.h   — MSL <math.h> macros on top of the real system libc.
# Use "SHELL:" so CMake does NOT de-duplicate the two identical -include
# options (without it, the second -include is dropped and its path leaks in as
# a stray source -> "cannot specify -o with -c with multiple files").
set(SMS_FORCE_INCLUDE
  "SHELL:-include ${SMS_COMPAT}/intrinsics.h"
  "SHELL:-include ${SMS_COMPAT}/msl_shim.h")

# The portable-core source set. Curated by port/try_compile.sh: every file here
# compiles cleanly under -std=c++17 with the shims and NO -fpermissive, so the
# 64-bit-port correctness checks (e.g. pointer-truncation diagnostics) stay on.
# Regenerate the candidate sweep with:  port/try_compile.sh <files...>
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/core_sources.txt" SMS_CORE_REL)
set(SMS_CORE_SOURCES "")
foreach(rel ${SMS_CORE_REL})
  if(NOT rel STREQUAL "")
    list(APPEND SMS_CORE_SOURCES "${SMS_REPO_ROOT}/${rel}")
  endif()
endforeach()

add_library(smsport_core STATIC ${SMS_CORE_SOURCES})
target_include_directories(smsport_core PRIVATE ${SMS_INCLUDES})
target_compile_options(smsport_core PRIVATE
  ${SMS_FORCE_INCLUDE}
  -fno-strict-aliasing       # the decomp type-puns through unions/casts
  -Wno-multichar)            # GC fourcc 'RTI ' style literals

# Keep the warning floor low but DO NOT silence pointer-truncation: that's the
# real 64-bit-port signal. (Files that trip it are deferred, not in core.)
target_compile_options(smsport_core PRIVATE -w)

message(STATUS "smsport_core: ${SMS_CORE_REL} (see core_sources.txt)")

# ---------------------------------------------------------------------------
# Endian-safe asset readers (port/assets/). Standalone: NO dependency on the
# decomp core or its shims — these are clean native readers written from the
# format spec, not ports of the struct-overlay decomp code. Portable to
# x86-64 and arm64 (host-byte-order independent).
# ---------------------------------------------------------------------------
add_library(smsport_assets STATIC assets/rarc.cpp)
target_include_directories(smsport_assets PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/assets")
target_compile_options(smsport_assets PRIVATE -Wall -Wextra)

enable_testing()
add_executable(rarc_test assets/rarc_test.cpp)
target_link_libraries(rarc_test PRIVATE smsport_assets)
# Run from the repo root so the default real-archive scratch path resolves
# (the test PASSES without it via the synthetic fixture + Yaz0 round-trip).
add_test(NAME rarc_test
         COMMAND rarc_test
         WORKING_DIRECTORY "${SMS_REPO_ROOT}")

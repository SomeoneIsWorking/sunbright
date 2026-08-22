# Standalone build for the resurrected static recompiler (2026-07-21).
#
# Restored from git 9283f44^ after the NO-RECOMP directive was removed. Differences
# from the original target:
#   - host_stubs.cpp is DROPPED: it only existed to satisfy Dolphin's Host interface
#     when linking Dolphin DiscIO. We feed the recompiler an already-extracted DOL
#     (scratch/bin/sms.dol), so no disc reading and no Dolphin dependency.
#   - disc_loader.cpp is excluded for the same reason (DOL-only path).
# cpu_state.h / intrinsics.h live in tools/runtime/ so the sources' existing
# "../runtime/..." includes resolve unchanged. Provisional placement — they belong
# with the recomp runtime once that is stood up.
cmake_minimum_required(VERSION 3.20)
project(sunbright_recomp CXX)
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "sunbright-recomp requires clang++; configure with -DCMAKE_CXX_COMPILER=clang++")
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

file(GLOB RECOMP_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
# disc_loader.cpp is self-contained (no Dolphin headers) - keep it

find_package(fmt QUIET)
add_executable(sunbright-recomp ${RECOMP_SOURCES})

# Unit tests. The target was lost when the recompiler was resurrected — recomp_test.cpp still said
# "built as sunbright-recomp-test, run via ctest" while nothing built it, so the suite silently did
# not exist. A test that cannot run is not a test, and the emitter is exactly where a silent
# wrong-answer bug hides (ps_sum1 read the wrong operand for as long as the file existed).
file(GLOB RECOMP_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp")
set(RECOMP_LIB_SOURCES ${RECOMP_SOURCES})
list(FILTER RECOMP_LIB_SOURCES EXCLUDE REGEX "main\\.cpp$")   # the tests bring their own main()
add_executable(sunbright-recomp-test ${RECOMP_TEST_SOURCES} ${RECOMP_LIB_SOURCES})
target_include_directories(sunbright-recomp-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
if(fmt_FOUND)
  target_link_libraries(sunbright-recomp-test PRIVATE fmt::fmt)
else()
  target_compile_definitions(sunbright-recomp-test PRIVATE FMT_HEADER_ONLY=1)
endif()
enable_testing()
add_test(NAME recomp_test COMMAND sunbright-recomp-test)
target_include_directories(sunbright-recomp PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
if(fmt_FOUND)
  target_link_libraries(sunbright-recomp PRIVATE fmt::fmt)
else()
  target_compile_definitions(sunbright-recomp PRIVATE FMT_HEADER_ONLY=1)
endif()

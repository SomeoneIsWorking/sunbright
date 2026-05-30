cmake_minimum_required(VERSION 3.20)
project(sunbright CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Options ───────────────────────────────────────────────────────────────────
option(SUNBRIGHT_USE_DOLPHIN_DISCIO  "Link recompiler against Dolphin DiscIO" ON)
option(SUNBRIGHT_USE_DOLPHIN_CORE    "Link runtime against Dolphin Core"      ON)

# ── Dolphin submodule ─────────────────────────────────────────────────────────
set(DOLPHIN_DIR "${CMAKE_SOURCE_DIR}/externals/dolphin")

if(NOT EXISTS "${DOLPHIN_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "Dolphin submodule not found.\n"
        "Run: git submodule update --init --recursive externals/dolphin"
    )
endif()

# Suppress Dolphin's own executables/tests — we only need its libraries
set(DOLPHIN_BUILD_TESTS      OFF CACHE BOOL "" FORCE)
set(ENABLE_QT                OFF CACHE BOOL "" FORCE)
set(ENABLE_WXWIDGETS         OFF CACHE BOOL "" FORCE)
set(ENABLE_SDL               OFF CACHE BOOL "" FORCE)
set(ENABLE_EVDEV             OFF CACHE BOOL "" FORCE)
set(ENABLE_BLUEZ             OFF CACHE BOOL "" FORCE)
set(ENABLE_PULSEAUDIO        OFF CACHE BOOL "" FORCE)
set(ENABLE_ALSA              OFF CACHE BOOL "" FORCE)
set(USE_UPNP                 OFF CACHE BOOL "" FORCE)
set(USE_MGBA                 OFF CACHE BOOL "" FORCE)
set(USE_DISCORD_PRESENCE     OFF CACHE BOOL "" FORCE)
set(ENABLE_AUTOUPDATE        OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTS             OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS        OFF CACHE BOOL "" FORCE)

# When Dolphin is a subdirectory, ${CMAKE_SOURCE_DIR} in its Vulkan backend
# resolves to our root, not Dolphin's. Add the correct VMA path globally.
include_directories(BEFORE
    "${DOLPHIN_DIR}/Externals/VulkanMemoryAllocator/include"
    "${DOLPHIN_DIR}/Externals/Vulkan-Headers/include"
)

add_subdirectory("${DOLPHIN_DIR}" dolphin EXCLUDE_FROM_ALL)

# ── Recompiler tool ───────────────────────────────────────────────────────────
file(GLOB_RECURSE RECOMP_SOURCES tools/recompiler/*.cpp)
add_executable(sunbright-recomp ${RECOMP_SOURCES})

target_include_directories(sunbright-recomp PRIVATE
    tools/recompiler
    runtime
    "${DOLPHIN_DIR}/Source/Core"
    "${CMAKE_BINARY_DIR}/dolphin/Source/Core"  # for generated headers
)

target_compile_definitions(sunbright-recomp PRIVATE
    $<$<BOOL:${SUNBRIGHT_USE_DOLPHIN_DISCIO}>:HAVE_DOLPHIN_DISCIO=1>
)

if(SUNBRIGHT_USE_DOLPHIN_DISCIO)
    target_link_libraries(sunbright-recomp PRIVATE discio core)
endif()

target_compile_options(sunbright-recomp PRIVATE
    -Wall -Wextra -Wno-unused-parameter
)

# ── Runtime shared library ────────────────────────────────────────────────────
# The runtime is a shared lib so it can be loaded into Dolphin or run standalone.
file(GLOB RUNTIME_SOURCES
    runtime/memory_bridge.cpp
    runtime/dolphin_hook.cpp
    runtime/os_hle.cpp
)

add_library(sunbright-runtime SHARED ${RUNTIME_SOURCES})

target_include_directories(sunbright-runtime PRIVATE
    runtime
    "${DOLPHIN_DIR}/Source/Core"
    "${CMAKE_BINARY_DIR}/dolphin/Source/Core"
)

target_compile_definitions(sunbright-runtime PRIVATE
    $<$<BOOL:${SUNBRIGHT_USE_DOLPHIN_CORE}>:HAVE_DOLPHIN_CORE=1>
    $<$<BOOL:${SUNBRIGHT_USE_DOLPHIN_DISCIO}>:HAVE_DOLPHIN_MEMMAP=1>
)

if(SUNBRIGHT_USE_DOLPHIN_CORE)
    target_link_libraries(sunbright-runtime PRIVATE core discio)
endif()
target_link_libraries(sunbright-runtime PRIVATE dl)

# ── Generated code library ────────────────────────────────────────────────────
# Built separately after /recompile generates the source files.
# Usage: cmake --build build --target sunbright-generated
if(EXISTS "${CMAKE_SOURCE_DIR}/generated/functions.cpp")
    add_library(sunbright-generated SHARED
        generated/functions.cpp
        generated/jump_table.cpp
    )
    target_include_directories(sunbright-generated PRIVATE runtime)
    target_compile_options(sunbright-generated PRIVATE
        -O3 -ffast-math  # generated code benefits from aggressive optimization
    )
    target_link_libraries(sunbright-generated PRIVATE sunbright-runtime)
endif()

# ── Install ───────────────────────────────────────────────────────────────────
install(TARGETS sunbright-recomp DESTINATION bin)
install(TARGETS sunbright-runtime DESTINATION lib)

message(STATUS "Sunbright build configured")
message(STATUS "  Dolphin DiscIO: ${SUNBRIGHT_USE_DOLPHIN_DISCIO}")
message(STATUS "  Dolphin Core:   ${SUNBRIGHT_USE_DOLPHIN_CORE}")

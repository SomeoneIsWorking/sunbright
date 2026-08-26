include_guard(GLOBAL)

# Debug is the project's normal development configuration, including when playing the game.
# Assertions and symbols must not require running the 2.8-million-line generated guest at -O0.
# Keep the policy at directory scope so Aurora, the native runtimes, and the generated guest all
# receive the same optimization contract without every target growing its own copy.
function(sunbright_enable_fast_debug_builds)
    if (NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
        set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
    endif ()

    add_compile_options(
        "$<$<AND:$<CONFIG:Debug>,$<COMPILE_LANG_AND_ID:C,AppleClang,Clang,GNU>>:-O2>"
        "$<$<AND:$<CONFIG:Debug>,$<COMPILE_LANG_AND_ID:CXX,AppleClang,Clang,GNU>>:-O2>"
        "$<$<AND:$<CONFIG:Debug>,$<COMPILE_LANG_AND_ID:OBJC,AppleClang,Clang>>:-O2>"
        "$<$<AND:$<CONFIG:Debug>,$<COMPILE_LANG_AND_ID:C,MSVC>>:/O2>"
        "$<$<AND:$<CONFIG:Debug>,$<COMPILE_LANG_AND_ID:CXX,MSVC>>:/O2>"
    )

    message(STATUS "Sunbright build policy: Debug keeps assertions/symbols and compiles at -O2 (/O2 on MSVC)")
endfunction()

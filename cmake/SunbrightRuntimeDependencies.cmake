include_guard(GLOBAL)

# Resolve runtime dependencies from the link graph, never from the build machine's PATH.
# CMake 3.21 supplies TARGET_RUNTIME_DLLS, including transitive imported SHARED targets.
function(sunbright_deploy_runtime_dependencies target required_library)
    if(NOT WIN32)
        return()
    endif()
    set(tool "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../tools/runtime_dependencies.py")
    set(manifest "${CMAKE_CURRENT_BINARY_DIR}/runtime/$<CONFIG>/${target}.txt")
    file(GENERATE OUTPUT "${manifest}"
         CONTENT "$<JOIN:$<TARGET_RUNTIME_DLLS:${target}>,\n>\n")
    get_target_property(library_type ${required_library} TYPE)
    set(required_arguments)
    if(library_type STREQUAL "SHARED_LIBRARY")
        list(APPEND required_arguments --required "$<TARGET_FILE:${required_library}>")
    elseif(NOT library_type STREQUAL "STATIC_LIBRARY")
        message(FATAL_ERROR "${required_library}: runtime deployment needs a SHARED or STATIC target")
    endif()
    set(arguments --manifest "${manifest}" --destination "$<TARGET_FILE_DIR:${target}>"
                  ${required_arguments})
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${Python3_EXECUTABLE}" "${tool}" ${arguments}
        VERBATIM)
    # An always-run, content-preserving step also repairs removed DLLs on an incremental build.
    add_custom_target(${target}_runtime ALL
        COMMAND "${Python3_EXECUTABLE}" "${tool}" ${arguments}
        DEPENDS ${target}
        VERBATIM)
    add_test(NAME ${target}_runtime
             COMMAND "${Python3_EXECUTABLE}" "${tool}" ${arguments} --check)
    set_tests_properties(${target}_runtime PROPERTIES TIMEOUT 30)
endfunction()

# Copies the runtime dependencies of an executable next to it.
#
# Invoked at build time via `cmake -P` because the executable only exists
# after linking. `file(GET_RUNTIME_DEPENDENCIES)` resolves the transitive
# dependencies of imported shared libraries (OpenCV pulls in zlib and the
# image codecs, protobuf pulls in abseil), which a plain copy of
# `$<TARGET_RUNTIME_DLLS>` does not cover.

if(NOT DEFINED TARGET_FILE OR NOT DEFINED DEST_DIR OR NOT DEFINED STAGE_DIR)
    message(FATAL_ERROR
        "CopyRuntimeDependencies: TARGET_FILE, DEST_DIR and STAGE_DIR are required")
endif()

if(POLICY CMP0207)
    cmake_policy(SET CMP0207 NEW)
endif()

# `file(GET_RUNTIME_DEPENDENCIES)` always searches the executable's own
# directory first. Copies left next to the binary by a previous build (or by
# a parallel target sharing the same output directory) therefore resolve
# twice, and CMake aborts with "Multiple conflicting paths found". Resolve
# against a throw-away copy of the executable in a per-target staging
# directory instead: it contains no DLLs, so every dependency resolves
# against exactly one entry of RUNTIME_DIRS.
get_filename_component(_tmcmt_target_name "${TARGET_FILE}" NAME)
set(_tmcmt_stage "${STAGE_DIR}/${_tmcmt_target_name}")
file(REMOVE_RECURSE "${_tmcmt_stage}")
file(MAKE_DIRECTORY "${_tmcmt_stage}")
file(COPY "${TARGET_FILE}" DESTINATION "${_tmcmt_stage}")

file(GET_RUNTIME_DEPENDENCIES
    EXECUTABLES "${_tmcmt_stage}/${_tmcmt_target_name}"
    RESOLVED_DEPENDENCIES_VAR _tmcmt_resolved
    UNRESOLVED_DEPENDENCIES_VAR _tmcmt_unresolved
    DIRECTORIES ${RUNTIME_DIRS}
    PRE_EXCLUDE_REGEXES
        "api-ms-.*"
        "ext-ms-.*"
        "AzureAttest.*"
        "Hvsi.*"
        "PdmUtilities.*"
        "wpaxholder.*"
    POST_EXCLUDE_REGEXES
        ".*[Ss]ystem32.*")

foreach(_tmcmt_dep IN LISTS _tmcmt_resolved)
    file(COPY "${_tmcmt_dep}" DESTINATION "${DEST_DIR}")
endforeach()
file(REMOVE_RECURSE "${_tmcmt_stage}")

if(_tmcmt_unresolved)
    # Windows 11 lists a few delayed system dependencies that are never
    # shipped with applications. Ignore those, but keep reporting anything
    # that might be a genuinely missing third-party DLL.
    set(_tmcmt_missing "")
    foreach(_tmcmt_dep IN LISTS _tmcmt_unresolved)
        if(_tmcmt_dep MATCHES "^(AzureAttest|Hvsi|PdmUtilities|wpaxholder)")
            continue()
        endif()
        list(APPEND _tmcmt_missing "${_tmcmt_dep}")
    endforeach()
    if(_tmcmt_missing)
        message(WARNING
            "TrafficMCMT: unresolved runtime dependencies for ${TARGET_FILE}: "
            "${_tmcmt_missing}")
    endif()
endif()

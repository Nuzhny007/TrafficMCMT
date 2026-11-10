# Dependency resolution for TrafficMCMT.
#
# Installed packages are preferred (find_package). If a package is missing,
# header-only / small dependencies are fetched with FetchContent. Optional
# heavy dependencies (FAISS, muSSP, ZeroMQ, Protobuf) are switched off with a
# warning so that the project always has a buildable configuration.

include_guard(GLOBAL)

include(FetchContent)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# spdlog
# ---------------------------------------------------------------------------
find_package(spdlog CONFIG QUIET)
if(NOT spdlog_FOUND)
    message(STATUS "TrafficMCMT: spdlog not found, fetching v1.14.1")
    set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
    set(SPDLOG_BUILD_TESTS   OFF CACHE BOOL "" FORCE)
    set(SPDLOG_BUILD_BENCH   OFF CACHE BOOL "" FORCE)
    set(SPDLOG_INSTALL       OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(spdlog
        GIT_REPOSITORY https://github.com/gabime/spdlog.git
        GIT_TAG        v1.14.1
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(spdlog)
endif()

# ---------------------------------------------------------------------------
# yaml-cpp
# ---------------------------------------------------------------------------
find_package(yaml-cpp CONFIG QUIET)
if(NOT yaml-cpp_FOUND)
    message(STATUS "TrafficMCMT: yaml-cpp not found, fetching 0.8.0")
    set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    set(YAML_CPP_INSTALL     OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(yaml-cpp
        GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
        GIT_TAG        0.8.0
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(yaml-cpp)
endif()

if(TARGET yaml-cpp::yaml-cpp)
    set(TMCMT_YAMLCPP_TARGET yaml-cpp::yaml-cpp)
elseif(TARGET yaml-cpp)
    set(TMCMT_YAMLCPP_TARGET yaml-cpp)
else()
    message(FATAL_ERROR "TrafficMCMT: yaml-cpp target not found after find_package/FetchContent")
endif()

# ---------------------------------------------------------------------------
# Eigen 3 (ReID index; header-only)
# ---------------------------------------------------------------------------
# Eigen is the only numeric dependency of the ReID module. Prefer an installed
# package (vcpkg / distro) and fall back to FetchContent so that the CPU-only
# CI runners build without extra setup.
find_package(Eigen3 3.3 CONFIG QUIET)
if(NOT Eigen3_FOUND)
    message(STATUS "TrafficMCMT: Eigen3 not found, fetching 3.4.0")
    set(EIGEN_BUILD_DOC       OFF CACHE BOOL "" FORCE)
    set(EIGEN_BUILD_PKGCONFIG OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTING         OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(eigen
        GIT_REPOSITORY https://gitlab.com/libeigen/eigen.git
        GIT_TAG        3.4.0
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(eigen)
endif()

if(NOT TARGET Eigen3::Eigen)
    message(FATAL_ERROR "TrafficMCMT: Eigen3::Eigen target not found after find_package/FetchContent")
endif()

# ---------------------------------------------------------------------------
# GoogleTest (tests only)
# ---------------------------------------------------------------------------
if(TRAFFICMCMT_BUILD_TESTS)
    find_package(GTest CONFIG QUIET)
    if(NOT GTest_FOUND)
        message(STATUS "TrafficMCMT: GTest not found, fetching v1.15.2")
        set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
        set(BUILD_GMOCK   ON  CACHE BOOL "" FORCE)
        FetchContent_Declare(googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            GIT_TAG        v1.15.2
            GIT_SHALLOW    TRUE)
        FetchContent_MakeAvailable(googletest)
    endif()
endif()

# ---------------------------------------------------------------------------
# OpenCV (Edge helpers and the MTT exporter only; never linked into Master)
# ---------------------------------------------------------------------------
if(TRAFFICMCMT_WITH_EDGE OR TRAFFICMCMT_WITH_MTT)
    find_package(OpenCV CONFIG QUIET)
    if(NOT OpenCV_FOUND)
        message(WARNING
            "TrafficMCMT: OpenCV not found. Set OpenCV_DIR to a directory that "
            "contains OpenCVConfig.cmake. Disabling TRAFFICMCMT_WITH_EDGE and "
            "TRAFFICMCMT_WITH_MTT.")
        set(TRAFFICMCMT_WITH_EDGE OFF CACHE BOOL "" FORCE)
        set(TRAFFICMCMT_WITH_MTT  OFF CACHE BOOL "" FORCE)
    else()
        message(STATUS "TrafficMCMT: OpenCV ${OpenCV_VERSION} (${OpenCV_DIR})")
    endif()
endif()

# ---------------------------------------------------------------------------
# Multitarget-tracker bridge (edge exporter only)
# ---------------------------------------------------------------------------
if(TRAFFICMCMT_WITH_MTT)
    find_package(MTTracking CONFIG QUIET)
    if(NOT MTTracking_FOUND)
        message(WARNING
            "TrafficMCMT: MTTracking not found. "
            "Set MTTracking_DIR to <install>/lib/MTTracking/cmake. "
            "Disabling TRAFFICMCMT_WITH_MTT (edge bridge is not built).")
        set(TRAFFICMCMT_WITH_MTT OFF CACHE BOOL "" FORCE)
    else()
        message(STATUS "TrafficMCMT: MTTracking ${MTTracking_VERSION} (${MTTracking_INCLUDE_DIR})")

        # A local MTTracking install usually exports only one configuration.
        # Map the missing ones onto the configuration that was actually
        # installed so that both Debug and Release builds of the edge exporter
        # can link against the same static libraries.
        set(_mtt_config_dir "${MTTracking_DIR}")
        set(_mtt_has_debug FALSE)
        set(_mtt_has_release FALSE)
        if(EXISTS "${_mtt_config_dir}/MTTrackingTargets-debug.cmake")
            set(_mtt_has_debug TRUE)
        endif()
        if(EXISTS "${_mtt_config_dir}/MTTrackingTargets-release.cmake")
            set(_mtt_has_release TRUE)
        endif()

        foreach(_mtt_target MTTracking::inih MTTracking::mtracking MTTracking::mdetection
                            MTTracking::yolo_lib MTTracking::yolo_rt_lib)
            if(TARGET ${_mtt_target})
                if(_mtt_has_debug AND NOT _mtt_has_release)
                    set_property(TARGET ${_mtt_target} PROPERTY MAP_IMPORTED_CONFIG_RELEASE Debug)
                    set_property(TARGET ${_mtt_target} PROPERTY MAP_IMPORTED_CONFIG_RELWITHDEBINFO Debug)
                    set_property(TARGET ${_mtt_target} PROPERTY MAP_IMPORTED_CONFIG_MINSIZEREL Debug)
                elseif(_mtt_has_release AND NOT _mtt_has_debug)
                    set_property(TARGET ${_mtt_target} PROPERTY MAP_IMPORTED_CONFIG_DEBUG Release)
                endif()
            endif()
        endforeach()
        unset(_mtt_config_dir)
        unset(_mtt_has_debug)
        unset(_mtt_has_release)
    endif()
endif()

# ---------------------------------------------------------------------------
# Protobuf (stage 2)
# ---------------------------------------------------------------------------
if(TRAFFICMCMT_WITH_PROTOBUF)
    find_package(Protobuf CONFIG QUIET)
    if(NOT Protobuf_FOUND)
        message(WARNING "TrafficMCMT: Protobuf not found -> TRAFFICMCMT_WITH_PROTOBUF=OFF")
        set(TRAFFICMCMT_WITH_PROTOBUF OFF CACHE BOOL "" FORCE)
    elseif(NOT TARGET protobuf::libprotobuf OR NOT TARGET protobuf::protoc)
        message(WARNING
            "TrafficMCMT: Protobuf found but protobuf::libprotobuf / protobuf::protoc "
            "targets are missing -> TRAFFICMCMT_WITH_PROTOBUF=OFF")
        set(TRAFFICMCMT_WITH_PROTOBUF OFF CACHE BOOL "" FORCE)
    else()
        message(STATUS "TrafficMCMT: Protobuf ${Protobuf_VERSION} (${Protobuf_DIR})")
    endif()
endif()

# ---------------------------------------------------------------------------
# Optional heavy dependencies (stages 3, 5, 6)
# ---------------------------------------------------------------------------

if(TRAFFICMCMT_WITH_ZMQ)
    if(NOT TRAFFICMCMT_WITH_PROTOBUF)
        message(WARNING
            "TrafficMCMT: ZeroMQ transport needs the protobuf protocol "
            "-> TRAFFICMCMT_WITH_ZMQ=OFF")
        set(TRAFFICMCMT_WITH_ZMQ OFF CACHE BOOL "" FORCE)
    else()
        # cppzmq is header-only and pulls in libzmq. The transport uses the
        # plain C API (`zmq.h`) so either target is acceptable.
        find_package(cppzmq CONFIG QUIET)
        if(TARGET cppzmq::cppzmq)
            set(TMCMT_ZMQ_TARGET cppzmq::cppzmq)
        elseif(TARGET cppzmq)
            set(TMCMT_ZMQ_TARGET cppzmq)
        elseif(TARGET cppzmq-static)
            set(TMCMT_ZMQ_TARGET cppzmq-static)
        else()
            find_package(ZeroMQ CONFIG QUIET)
            if(TARGET libzmq)
                set(TMCMT_ZMQ_TARGET libzmq)
            elseif(TARGET libzmq-static)
                set(TMCMT_ZMQ_TARGET libzmq-static)
            else()
                message(WARNING
                    "TrafficMCMT: ZeroMQ/cppzmq not found "
                    "-> TRAFFICMCMT_WITH_ZMQ=OFF")
                set(TRAFFICMCMT_WITH_ZMQ OFF CACHE BOOL "" FORCE)
            endif()
        endif()
        if(TRAFFICMCMT_WITH_ZMQ)
            message(STATUS "TrafficMCMT: ZeroMQ via ${TMCMT_ZMQ_TARGET}")
        endif()
    endif()
endif()

if(TRAFFICMCMT_WITH_FAISS)
    find_package(faiss CONFIG QUIET)
    if(NOT faiss_FOUND)
        message(WARNING "TrafficMCMT: FAISS not found -> TRAFFICMCMT_WITH_FAISS=OFF")
        set(TRAFFICMCMT_WITH_FAISS OFF CACHE BOOL "" FORCE)
    endif()
endif()

if(TRAFFICMCMT_WITH_MUSSP)
    # A local source checkout (for example the Nuzhny007/muSSP fork) is the
    # primary way to enable muSSP: it avoids vendoring third-party sources and
    # works on machines without an installed package. An installed package is
    # still supported as a fallback.
    set(TMCMT_MUSSP_LOCAL FALSE)
    if(DEFINED TRAFFICMCMT_MUSSP_SOURCE_DIR AND
       EXISTS "${TRAFFICMCMT_MUSSP_SOURCE_DIR}/muSSP/Graph.cpp")
        set(TMCMT_MUSSP_LOCAL TRUE)
        message(STATUS "TrafficMCMT: muSSP sources from ${TRAFFICMCMT_MUSSP_SOURCE_DIR}")
    else()
        find_package(mussp CONFIG QUIET)
        if(NOT mussp_FOUND)
            message(WARNING
                "TrafficMCMT: muSSP not found. Set TRAFFICMCMT_MUSSP_SOURCE_DIR "
                "to a checkout containing muSSP/Graph.cpp, or install the mussp "
                "package. Disabling TRAFFICMCMT_WITH_MUSSP.")
            set(TRAFFICMCMT_WITH_MUSSP OFF CACHE BOOL "" FORCE)
        endif()
    endif()
endif()

message(STATUS "TrafficMCMT options: "
    "EDGE=${TRAFFICMCMT_WITH_EDGE} "
    "MTT=${TRAFFICMCMT_WITH_MTT} "
    "ZMQ=${TRAFFICMCMT_WITH_ZMQ} "
    "PROTOBUF=${TRAFFICMCMT_WITH_PROTOBUF} "
    "FAISS=${TRAFFICMCMT_WITH_FAISS} "
    "MUSSP=${TRAFFICMCMT_WITH_MUSSP} "
    "TESTS=${TRAFFICMCMT_BUILD_TESTS}")

# ---------------------------------------------------------------------------
# Runtime dependencies
# ---------------------------------------------------------------------------
# On Windows, executables link against DLLs (OpenCV, protobuf, MTT). Copying
# them next to the executable keeps `ctest` and manual runs working without
# touching PATH. The copy runs as a build-time CMake script because the
# transitive dependencies of imported DLLs (zlib, image codecs, abseil) are
# only known once the executable has been linked.
set(TMCMT_COPY_RUNTIME_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/CopyRuntimeDependencies.cmake")

set(TRAFFICMCMT_RUNTIME_DIRS "")
foreach(_tmcmt_prefix_var Protobuf_DIR OpenCV_DIR cppzmq_DIR ZeroMQ_DIR)
    if(DEFINED ${_tmcmt_prefix_var})
        get_filename_component(_tmcmt_prefix "${${_tmcmt_prefix_var}}/../.." ABSOLUTE)
        if(EXISTS "${_tmcmt_prefix}/bin")
            list(APPEND TRAFFICMCMT_RUNTIME_DIRS "${_tmcmt_prefix}/bin")
        endif()
        unset(_tmcmt_prefix)
    endif()
endforeach()
list(REMOVE_DUPLICATES TRAFFICMCMT_RUNTIME_DIRS)
unset(_tmcmt_prefix_var)

function(tmcmt_copy_runtime_dependencies target)
    if(NOT WIN32)
        return()
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            -D "TARGET_FILE=$<TARGET_FILE:${target}>"
            -D "DEST_DIR=$<TARGET_FILE_DIR:${target}>"
            -D "STAGE_DIR=${CMAKE_CURRENT_BINARY_DIR}/tmcmt_runtime_stage"
            -D "RUNTIME_DIRS=${TRAFFICMCMT_RUNTIME_DIRS}"
            -P "${TMCMT_COPY_RUNTIME_SCRIPT}"
        VERBATIM)
endfunction()

cmake_minimum_required(VERSION 3.21)

project(TrafficMCMT
    VERSION 0.1.0
    DESCRIPTION "Multi-camera multi-target tracking master node (late fusion)"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(TRAFFICMCMT_BUILD_TESTS    "Build unit and integration tests" ON)
option(TRAFFICMCMT_WITH_MTT       "Build Multitarget-tracker bridge (edge exporter)" ON)
option(TRAFFICMCMT_WITH_EDGE      "Build Edge helpers (ReID, tracklet chunking)" ON)
option(TRAFFICMCMT_WITH_ZMQ       "ZeroMQ transport (stage 6)" OFF)
option(TRAFFICMCMT_WITH_PROTOBUF  "Protobuf tracklet protocol (stage 2)" ON)
option(TRAFFICMCMT_WITH_FAISS     "FAISS vector index (stage 3)" OFF)
option(TRAFFICMCMT_WITH_MUSSP     "muSSP min-cost flow solver (stage 5)" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(GNUInstallDirs)
include(Dependencies)
include(CompilerWarnings)

# ---------------------------------------------------------------------------
# Core library: domain types, configuration, logging.
# No OpenCV, no Multitarget-tracker, no CUDA, no protobuf.
# ---------------------------------------------------------------------------
add_library(trafficmcmt_core STATIC
    src/core/Config.cpp
    src/core/Log.cpp
    src/core/Result.cpp
    src/core/TrackletId.cpp)
add_library(TrafficMCMT::core ALIAS trafficmcmt_core)

target_include_directories(trafficmcmt_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_compile_definitions(trafficmcmt_core PUBLIC
    TMCMT_VERSION="${PROJECT_VERSION}")

target_link_libraries(trafficmcmt_core
    PUBLIC
        spdlog::spdlog
        ${TMCMT_YAMLCPP_TARGET}
        Threads::Threads)

tmcmt_set_warnings(trafficmcmt_core)

# ---------------------------------------------------------------------------
# Protocol: generated protobuf messages (stage 2).
# ---------------------------------------------------------------------------
set(TMCMT_INSTALL_TARGETS trafficmcmt_core)

if(TRAFFICMCMT_WITH_PROTOBUF)
    set(TMCMT_PROTO_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/gen")

    add_library(trafficmcmt_proto STATIC
        proto/tracklet.proto
        proto/calibration.proto)
    add_library(TrafficMCMT::proto ALIAS trafficmcmt_proto)

    target_include_directories(trafficmcmt_proto PUBLIC
        $<BUILD_INTERFACE:${TMCMT_PROTO_GEN_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    target_link_libraries(trafficmcmt_proto PUBLIC protobuf::libprotobuf)

    protobuf_generate(TARGET trafficmcmt_proto
        LANGUAGE cpp
        PROTOC_OUT_DIR "${TMCMT_PROTO_GEN_DIR}"
        IMPORT_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/proto")

    tmcmt_set_warnings(trafficmcmt_proto)

    # -----------------------------------------------------------------------
    # I/O: codec, file source, sinks, normalizer, in-memory store.
    # -----------------------------------------------------------------------
    add_library(trafficmcmt_io STATIC
        src/io/TrackletCodec.cpp
        src/io/FileTrackletSource.cpp
        src/io/TrackletSinks.cpp
        src/io/TrackletNormalizer.cpp
        src/io/InMemoryTrackletStore.cpp)
    add_library(TrafficMCMT::io ALIAS trafficmcmt_io)

    target_link_libraries(trafficmcmt_io
        PUBLIC
            trafficmcmt_core
            trafficmcmt_proto)
    tmcmt_set_warnings(trafficmcmt_io)

    list(APPEND TMCMT_INSTALL_TARGETS trafficmcmt_proto trafficmcmt_io)
endif()

# ---------------------------------------------------------------------------
# Edge helpers: ReID extraction and tracklet chunking (OpenCV only).
# ---------------------------------------------------------------------------
if(TRAFFICMCMT_WITH_EDGE AND OpenCV_FOUND)
    add_library(trafficmcmt_edge STATIC
        src/edge/ColorHistogramReIdExtractor.cpp
        src/edge/DnnReIdExtractor.cpp
        src/edge/EdgeTrackExporter.cpp)
    add_library(TrafficMCMT::edge ALIAS trafficmcmt_edge)

    target_include_directories(trafficmcmt_edge PUBLIC ${OpenCV_INCLUDE_DIRS})
    target_link_libraries(trafficmcmt_edge
        PUBLIC
            trafficmcmt_core
            ${OpenCV_LIBS})
    tmcmt_set_warnings(trafficmcmt_edge)

    list(APPEND TMCMT_INSTALL_TARGETS trafficmcmt_edge)
endif()

# ---------------------------------------------------------------------------
# Multitarget-tracker bridge (Edge only, never linked into the Master).
# ---------------------------------------------------------------------------
if(TRAFFICMCMT_WITH_MTT AND TARGET trafficmcmt_edge AND MTTracking_FOUND)
    add_library(trafficmcmt_mtt STATIC
        src/mtt/MttTrackAdapter.cpp)
    add_library(TrafficMCMT::mtt ALIAS trafficmcmt_mtt)

    target_include_directories(trafficmcmt_mtt PUBLIC ${MTTracking_INCLUDE_DIR})
    target_compile_definitions(trafficmcmt_mtt PUBLIC HAVE_FILESYSTEM)
    target_link_libraries(trafficmcmt_mtt
        PUBLIC
            trafficmcmt_core
            trafficmcmt_edge
            MTTracking::mtracking)
    tmcmt_set_warnings(trafficmcmt_mtt)

    list(APPEND TMCMT_INSTALL_TARGETS trafficmcmt_mtt)
endif()

# ---------------------------------------------------------------------------
# Applications
# ---------------------------------------------------------------------------
add_executable(trafficmcmt_master apps/master_node/main.cpp)
target_link_libraries(trafficmcmt_master PRIVATE trafficmcmt_core)
tmcmt_set_warnings(trafficmcmt_master)

if(TARGET trafficmcmt_io)
    add_executable(trafficmcmt_tracklet_inspect apps/tracklet_inspect/main.cpp)
    target_link_libraries(trafficmcmt_tracklet_inspect PRIVATE trafficmcmt_io)
    tmcmt_set_warnings(trafficmcmt_tracklet_inspect)
    tmcmt_copy_runtime_dependencies(trafficmcmt_tracklet_inspect)
endif()

if(TARGET trafficmcmt_mtt AND TARGET trafficmcmt_io)
    add_executable(trafficmcmt_mtt_export apps/mtt_export/main.cpp)
    target_link_libraries(trafficmcmt_mtt_export
        PRIVATE
            trafficmcmt_mtt
            trafficmcmt_io)
    tmcmt_set_warnings(trafficmcmt_mtt_export)
    tmcmt_copy_runtime_dependencies(trafficmcmt_mtt_export)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(TRAFFICMCMT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Install / package config
# ---------------------------------------------------------------------------
include(CMakePackageConfigHelpers)

install(TARGETS ${TMCMT_INSTALL_TARGETS}
    EXPORT TrafficMCMTTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT TrafficMCMTTargets
    FILE TrafficMCMTTargets.cmake
    NAMESPACE TrafficMCMT::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/TrafficMCMT)

configure_package_config_file(
    cmake/TrafficMCMTConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/TrafficMCMTConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/TrafficMCMT)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/TrafficMCMTConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/TrafficMCMTConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/TrafficMCMTConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/TrafficMCMT)

install(FILES
    configs/master.yaml
    configs/logging.yaml
    DESTINATION ${CMAKE_INSTALL_DATADIR}/trafficmcmt)

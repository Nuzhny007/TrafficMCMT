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
option(TRAFFICMCMT_WITH_MAP       "Map feed: tracklets -> viewer protocol" ON)
option(TRAFFICMCMT_WITH_WS        "WebSocket server for the map feed" ON)
option(TRAFFICMCMT_WITH_FAISS     "FAISS vector index (stage 3)" OFF)
option(TRAFFICMCMT_WITH_MUSSP     "muSSP min-cost flow solver (stage 5)" OFF)
set(TRAFFICMCMT_MUSSP_SOURCE_DIR "" CACHE PATH
    "Path to a muSSP checkout (must contain muSSP/Graph.cpp)")

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
    src/core/Geo.cpp
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
# Emissions: HBEFA rates + Gaussian puff dispersion.
# Pure C++, no OpenCV, no Multitarget-tracker, no sockets.
# ---------------------------------------------------------------------------
add_library(trafficmcmt_emissions STATIC
    src/emissions/ConcentrationGrid.cpp
    src/emissions/EmissionSimulator.cpp
    src/emissions/GaussianPuffModel.cpp
    src/emissions/HbefaEmissionModel.cpp
    src/emissions/PasquillGifford.cpp)
add_library(TrafficMCMT::emissions ALIAS trafficmcmt_emissions)

target_link_libraries(trafficmcmt_emissions
    PUBLIC
        trafficmcmt_core)
tmcmt_set_warnings(trafficmcmt_emissions)

# ---------------------------------------------------------------------------
# ReID: embedding index, gallery and matcher (CPU-only, Eigen).
# Never links OpenCV, protobuf or the Multitarget-tracker.
# ---------------------------------------------------------------------------
add_library(trafficmcmt_reid STATIC
    src/reid/BruteForceIndex.cpp
    src/reid/EmbeddingIndex.cpp
    src/reid/EmbeddingNormalizer.cpp
    src/reid/GalleryManager.cpp
    src/reid/ReIdMatcher.cpp)
add_library(TrafficMCMT::reid ALIAS trafficmcmt_reid)

target_link_libraries(trafficmcmt_reid
    PUBLIC
        trafficmcmt_core
        Eigen3::Eigen)
tmcmt_set_warnings(trafficmcmt_reid)

if(TRAFFICMCMT_WITH_FAISS)
    target_sources(trafficmcmt_reid PRIVATE src/reid/FaissIndex.cpp)
    target_compile_definitions(trafficmcmt_reid PUBLIC TMCMT_WITH_FAISS=1)
    if(TARGET faiss::faiss)
        target_link_libraries(trafficmcmt_reid PRIVATE faiss::faiss)
    elseif(TARGET faiss)
        target_link_libraries(trafficmcmt_reid PRIVATE faiss)
    else()
        message(WARNING "TrafficMCMT: FAISS found but no import target -> disabling FAISS")
        set(TRAFFICMCMT_WITH_FAISS OFF CACHE BOOL "" FORCE)
    endif()
endif()

# ---------------------------------------------------------------------------
# st-ReID: camera topology, transition estimation, spatial gates and the
# fused scorer (stage 4). CPU-only; no OpenCV, protobuf or Multitarget-tracker.
# ---------------------------------------------------------------------------
add_library(trafficmcmt_st STATIC
    src/st/CameraLinkModel.cpp
    src/st/PairCandidateGenerator.cpp
    src/st/Polygon.cpp
    src/st/SpatialGate.cpp
    src/st/StReIdScorer.cpp
    src/st/TrackletEndpoint.cpp
    src/st/TransitionEstimator.cpp)
add_library(TrafficMCMT::st ALIAS trafficmcmt_st)

target_link_libraries(trafficmcmt_st
    PUBLIC
        trafficmcmt_core
        trafficmcmt_reid)
tmcmt_set_warnings(trafficmcmt_st)

# ---------------------------------------------------------------------------
# Flow: tracklet graph, pruning and global association (stage 5).
# CPU-only; muSSP is an optional private dependency.
# ---------------------------------------------------------------------------
if(TRAFFICMCMT_WITH_MUSSP)
    if(TMCMT_MUSSP_LOCAL)
        # Compile the third-party sources into an object library and embed the
        # objects into trafficmcmt_flow. A separate non-imported library would
        # leak an absolute source path into TrafficMCMTTargets.cmake, and an
        # object library keeps the project warning policy off the vendored code.
        add_library(trafficmcmt_mussp_objects OBJECT
            ${TRAFFICMCMT_MUSSP_SOURCE_DIR}/muSSP/Graph.cpp
            ${TRAFFICMCMT_MUSSP_SOURCE_DIR}/muSSP/Node.cpp
            ${TRAFFICMCMT_MUSSP_SOURCE_DIR}/muSSP/Sink.cpp)
        target_include_directories(trafficmcmt_mussp_objects PRIVATE
            ${TRAFFICMCMT_MUSSP_SOURCE_DIR}/muSSP)
        if(MSVC)
            target_compile_options(trafficmcmt_mussp_objects PRIVATE /w)
        else()
            target_compile_options(trafficmcmt_mussp_objects PRIVATE -w)
        endif()
        set(TMCMT_MUSSP_OBJECTS $<TARGET_OBJECTS:trafficmcmt_mussp_objects>)
        set(TMCMT_MUSSP_INCLUDE_DIR ${TRAFFICMCMT_MUSSP_SOURCE_DIR}/muSSP)
    elseif(TARGET mussp::mussp)
        set(TMCMT_MUSSP_TARGET mussp::mussp)
    elseif(TARGET mussp)
        set(TMCMT_MUSSP_TARGET mussp)
    else()
        message(WARNING "TrafficMCMT: muSSP target missing -> TRAFFICMCMT_WITH_MUSSP=OFF")
        set(TRAFFICMCMT_WITH_MUSSP OFF CACHE BOOL "" FORCE)
    endif()
endif()

add_library(trafficmcmt_flow STATIC
    src/flow/FlowAssociator.cpp
    src/flow/GlobalIdManager.cpp
    src/flow/GraphPruner.cpp
    src/flow/SspSolver.cpp
    src/flow/TrackletGraph.cpp)
add_library(TrafficMCMT::flow ALIAS trafficmcmt_flow)

target_link_libraries(trafficmcmt_flow
    PUBLIC
        trafficmcmt_core)

if(TRAFFICMCMT_WITH_MUSSP)
    target_sources(trafficmcmt_flow PRIVATE
        src/flow/MuSspSolver.cpp
        ${TMCMT_MUSSP_OBJECTS})
    target_compile_definitions(trafficmcmt_flow PUBLIC TMCMT_WITH_MUSSP=1)
    if(TMCMT_MUSSP_INCLUDE_DIR)
        target_include_directories(trafficmcmt_flow PRIVATE ${TMCMT_MUSSP_INCLUDE_DIR})
    else()
        target_link_libraries(trafficmcmt_flow PRIVATE ${TMCMT_MUSSP_TARGET})
    endif()
endif()
tmcmt_set_warnings(trafficmcmt_flow)

# ---------------------------------------------------------------------------
# Metrics: track-level identity metrics (stage 5).
# ---------------------------------------------------------------------------
add_library(trafficmcmt_metrics STATIC
    src/metrics/MotMetrics.cpp)
add_library(TrafficMCMT::metrics ALIAS trafficmcmt_metrics)

target_link_libraries(trafficmcmt_metrics
    PUBLIC
        trafficmcmt_core)
tmcmt_set_warnings(trafficmcmt_metrics)

# ---------------------------------------------------------------------------
# Protocol: generated protobuf messages (stage 2).
# ---------------------------------------------------------------------------
set(TMCMT_INSTALL_TARGETS
    trafficmcmt_core
    trafficmcmt_emissions
    trafficmcmt_reid
    trafficmcmt_st
    trafficmcmt_flow
    trafficmcmt_metrics)

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
# Map feed: converts Edge tracklets into the viewer WebSocket protocol (v1).
# No OpenCV, no Multitarget-tracker, no sockets.
# ---------------------------------------------------------------------------
if(TRAFFICMCMT_WITH_MAP AND TARGET trafficmcmt_io)
    add_library(trafficmcmt_map STATIC
        src/map/AccelerationEstimator.cpp
        src/map/MapConfig.cpp
        src/map/MapFeed.cpp
        src/map/MapIdRegistry.cpp
        src/map/MapProtocol.cpp
        src/map/TrackletTimeline.cpp
        src/map/TrackletFiles.cpp
        src/map/TrailBuilder.cpp
        src/map/VelocityEstimator.cpp)
    add_library(TrafficMCMT::map ALIAS trafficmcmt_map)

    target_link_libraries(trafficmcmt_map
        PUBLIC
            trafficmcmt_core
            trafficmcmt_emissions
            trafficmcmt_io)
    tmcmt_set_warnings(trafficmcmt_map)

    list(APPEND TMCMT_INSTALL_TARGETS trafficmcmt_map)
endif()

# ---------------------------------------------------------------------------
# WebSocket transport (RFC 6455). Platform sockets only, no third-party deps.
# ---------------------------------------------------------------------------
if(TRAFFICMCMT_WITH_WS)
    add_library(trafficmcmt_net STATIC
        src/net/WebSocketClient.cpp
        src/net/WebSocketServer.cpp)
    add_library(TrafficMCMT::net ALIAS trafficmcmt_net)

    target_link_libraries(trafficmcmt_net
        PUBLIC
            trafficmcmt_core)
    if(WIN32)
        target_link_libraries(trafficmcmt_net PUBLIC ws2_32)
    endif()
    tmcmt_set_warnings(trafficmcmt_net)

    list(APPEND TMCMT_INSTALL_TARGETS trafficmcmt_net)
endif()

# ---------------------------------------------------------------------------
# Edge helpers: ReID extraction and tracklet chunking (OpenCV only).
# ---------------------------------------------------------------------------
if(TRAFFICMCMT_WITH_EDGE AND OpenCV_FOUND)
    add_library(trafficmcmt_edge STATIC
        src/edge/ColorHistogramReIdExtractor.cpp
        src/edge/DnnReIdExtractor.cpp
        src/edge/EdgeTrackExporter.cpp
        src/edge/GeoBinding.cpp
        src/edge/TypeAwareReIdExtractor.cpp)
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
            trafficmcmt_io
            MTTracking::mdetection)
    tmcmt_set_warnings(trafficmcmt_mtt_export)
    tmcmt_copy_runtime_dependencies(trafficmcmt_mtt_export)
endif()

if(TARGET trafficmcmt_map)
    add_executable(trafficmcmt_map_export apps/map_export/main.cpp)
    target_link_libraries(trafficmcmt_map_export PRIVATE trafficmcmt_map)
    tmcmt_set_warnings(trafficmcmt_map_export)
    tmcmt_copy_runtime_dependencies(trafficmcmt_map_export)

    if(TARGET trafficmcmt_net)
        add_executable(trafficmcmt_map_gateway apps/map_gateway/main.cpp)
        target_link_libraries(trafficmcmt_map_gateway PRIVATE trafficmcmt_map trafficmcmt_net)
        tmcmt_set_warnings(trafficmcmt_map_gateway)
        tmcmt_copy_runtime_dependencies(trafficmcmt_map_gateway)
    endif()
endif()

# ---------------------------------------------------------------------------
# ReID applications
# ---------------------------------------------------------------------------
add_executable(trafficmcmt_reid_bench apps/reid_bench/main.cpp)
target_link_libraries(trafficmcmt_reid_bench PRIVATE trafficmcmt_reid)
tmcmt_set_warnings(trafficmcmt_reid_bench)

if(TARGET trafficmcmt_io)
    add_executable(trafficmcmt_reid_eval apps/reid_eval/main.cpp)
    target_link_libraries(trafficmcmt_reid_eval
        PRIVATE
            trafficmcmt_io
            trafficmcmt_reid)
    tmcmt_set_warnings(trafficmcmt_reid_eval)
    tmcmt_copy_runtime_dependencies(trafficmcmt_reid_eval)

    add_executable(trafficmcmt_st_estimate apps/st_estimate/main.cpp)
    target_link_libraries(trafficmcmt_st_estimate
        PRIVATE
            trafficmcmt_io
            trafficmcmt_st)
    tmcmt_set_warnings(trafficmcmt_st_estimate)
    tmcmt_copy_runtime_dependencies(trafficmcmt_st_estimate)
endif()

if(TARGET trafficmcmt_io AND TARGET trafficmcmt_st)
    add_executable(trafficmcmt_associate apps/associate/main.cpp)
    target_link_libraries(trafficmcmt_associate
        PRIVATE
            trafficmcmt_io
            trafficmcmt_st
            trafficmcmt_flow
            trafficmcmt_metrics)
    tmcmt_set_warnings(trafficmcmt_associate)
    tmcmt_copy_runtime_dependencies(trafficmcmt_associate)
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
    configs/map_feed.yaml
    configs/map_feed_aire.yaml
    configs/camera_links.yaml
    DESTINATION ${CMAKE_INSTALL_DATADIR}/trafficmcmt)

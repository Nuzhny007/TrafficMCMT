set(TMCMT_TEST_SOURCES
    unit/test_config.cpp
    unit/test_result.cpp)

if(TARGET trafficmcmt_io)
    list(APPEND TMCMT_TEST_SOURCES
        unit/test_tracklet_id.cpp
        unit/test_tracklet_codec.cpp
        unit/test_tracklet_normalizer.cpp
        unit/test_tracklet_store.cpp
        unit/test_file_source.cpp
        unit/test_geo.cpp)
endif()

if(TARGET trafficmcmt_emissions)
    list(APPEND TMCMT_TEST_SOURCES
        unit/test_emissions.cpp)
endif()

if(TARGET trafficmcmt_map)
    list(APPEND TMCMT_TEST_SOURCES
        unit/test_map.cpp)
endif()

add_executable(trafficmcmt_unit_tests ${TMCMT_TEST_SOURCES})

target_link_libraries(trafficmcmt_unit_tests
    PRIVATE
        trafficmcmt_core
        GTest::gtest_main)

if(TARGET trafficmcmt_io)
    target_link_libraries(trafficmcmt_unit_tests PRIVATE trafficmcmt_io)
    tmcmt_copy_runtime_dependencies(trafficmcmt_unit_tests)

    # Golden protocol files live next to the tests, not next to the binary.
    target_compile_definitions(trafficmcmt_unit_tests
        PRIVATE TMCMT_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
endif()

if(TARGET trafficmcmt_emissions)
    target_link_libraries(trafficmcmt_unit_tests PRIVATE trafficmcmt_emissions)
endif()

if(TARGET trafficmcmt_map)
    target_link_libraries(trafficmcmt_unit_tests PRIVATE trafficmcmt_map)
endif()

tmcmt_set_warnings(trafficmcmt_unit_tests)

include(GoogleTest)
gtest_discover_tests(trafficmcmt_unit_tests
    DISCOVERY_MODE PRE_TEST
    DISCOVERY_TIMEOUT 30)

if(TARGET trafficmcmt_edge)
    add_executable(trafficmcmt_edge_tests
        unit/test_edge_exporter.cpp
        unit/test_geo_binding.cpp
        unit/test_reid.cpp
        unit/test_type_aware_reid.cpp)

    target_link_libraries(trafficmcmt_edge_tests
        PRIVATE
            trafficmcmt_edge
            GTest::gtest_main)

    tmcmt_set_warnings(trafficmcmt_edge_tests)
    tmcmt_copy_runtime_dependencies(trafficmcmt_edge_tests)

    gtest_discover_tests(trafficmcmt_edge_tests
        DISCOVERY_MODE PRE_TEST
        DISCOVERY_TIMEOUT 30)
endif()

if(TARGET trafficmcmt_net)
    add_executable(trafficmcmt_net_tests
        unit/test_ws.cpp)

    target_link_libraries(trafficmcmt_net_tests
        PRIVATE
            trafficmcmt_net
            GTest::gtest_main)

    # `test_ws.cpp` uses the internal socket helpers for the `/health` check.
    target_include_directories(trafficmcmt_net_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/net)

    tmcmt_set_warnings(trafficmcmt_net_tests)
    tmcmt_copy_runtime_dependencies(trafficmcmt_net_tests)

    gtest_discover_tests(trafficmcmt_net_tests
        DISCOVERY_MODE PRE_TEST
        DISCOVERY_TIMEOUT 30)
endif()

set(gateway_test_targets
    test_gw_parser
    test_gw_body
    test_gw_connection
    test_gw_proxy
    test_gw_builtin
    test_gw_e2e
    test_gw_boundary
    test_gw_core
    test_gw_metrics
    test_gw_metrics_upstream
    test_gateway_v2
    test_gw_reload_offload
    test_gw_pool
    stress_gw_pool
    test_arc
    fakerisk)

foreach(target IN LISTS gateway_test_targets)
    add_executable(${target} ${target}.cpp)
    target_link_libraries(${target} bronx_core)
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../other)
endforeach()

set(ipban_test_targets
    test_ipban_proto
    test_ipban_guard
    test_ipban_judge
    test_ipban_e2e
    test_ipban_delta
    test_ipban_restart
    test_ipban_resync
    test_ipban_report
    test_ipban_gateway
    test_ipban_stress
    test_ipban_waf
    test_ipban_risk
    test_ipban_reload
    test_ipban_db
    test_ipban_persist
    test_ipban_dbstress
    test_ipban_dbpolicy
    test_ipban_ctl
    test_banhub)

foreach(target IN LISTS ipban_test_targets)
    add_executable(${target} ${target}.cpp)
    target_link_libraries(${target} bronx_core)
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../other)
endforeach()

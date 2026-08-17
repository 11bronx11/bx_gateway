set(api_gw_bin_dir "${PROJECT_SOURCE_DIR}/bin${BRONX_OUT_SUFFIX}")

add_executable(test_api_gw_process test_app_process.cpp)
target_link_libraries(test_api_gw_process bronx_core)
target_compile_definitions(test_api_gw_process PRIVATE
    API_GW_APP_BIN_DIR="${api_gw_bin_dir}")

add_executable(test_api_gw_chain test_api_gw_chain.cpp)
target_link_libraries(test_api_gw_chain bronx_core)
target_include_directories(test_api_gw_chain PRIVATE ${PROJECT_SOURCE_DIR}/test/other)
target_compile_definitions(test_api_gw_chain PRIVATE
    API_GW_APP_BIN_DIR="${api_gw_bin_dir}")

add_executable(test_api_gw_stress test_api_gw_stress.cpp)
target_link_libraries(test_api_gw_stress bronx_core)
target_include_directories(test_api_gw_stress PRIVATE ${PROJECT_SOURCE_DIR}/test/other)
target_compile_definitions(test_api_gw_stress PRIVATE
    API_GW_APP_BIN_DIR="${api_gw_bin_dir}")

add_executable(test_api_gw_lifecycle test_api_gw_lifecycle.cpp)
target_link_libraries(test_api_gw_lifecycle bronx_core)
target_include_directories(test_api_gw_lifecycle PRIVATE ${PROJECT_SOURCE_DIR}/test/other)
target_compile_definitions(test_api_gw_lifecycle PRIVATE
    API_GW_APP_BIN_DIR="${api_gw_bin_dir}")

add_executable(test_api_gw_journey test_api_gw_journey.cpp)
target_link_libraries(test_api_gw_journey bronx_core)
target_include_directories(test_api_gw_journey PRIVATE ${PROJECT_SOURCE_DIR}/test/other)
target_compile_definitions(test_api_gw_journey PRIVATE
    API_GW_APP_BIN_DIR="${api_gw_bin_dir}")

add_executable(test_api_gw_trace test_api_gw_trace.cpp)
target_link_libraries(test_api_gw_trace bronx_core)
target_include_directories(test_api_gw_trace PRIVATE ${PROJECT_SOURCE_DIR}/test/other)
target_compile_definitions(test_api_gw_trace PRIVATE
    API_GW_APP_BIN_DIR="${api_gw_bin_dir}")

foreach(test_target IN ITEMS test_api_gw_process test_api_gw_chain test_api_gw_stress
        test_api_gw_lifecycle test_api_gw_journey test_api_gw_trace)
    add_dependencies(${test_target} gw hub banctl)
endforeach()

set(bronx_test_targets
    test_log_async
    bench_async_log
    stress_async_log
    extreme_async_log
    fatal_barrier_async_log
    test_log_layer
    test_chain_integration
    test_address
    test_socket
    test_socket_smoke
    test_bytearray_strict
    test_bytearray_smoke
    test_util
    test_scheduler_basic
    test_timer
    test_iomanager_cancel
    test_tcp_server_lifecycle
    test_socket_stream_semantics
    test_chain_e2e
    test_stack_pool
    test_sync
    test_thread_caps
    test_singleton
    test_log_sampling
    test_log_json
    test_log_defaults
    test_app_env_config
    test_app_lifecycle
    test_offload_fd_leak
    test_cpu_pool
    test_crash_handler)

foreach(target IN LISTS bronx_test_targets)
    add_executable(${target} ${target}.cpp)
    target_link_libraries(${target} bronx_core)
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../other)
endforeach()

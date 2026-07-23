# 测试产物留在 build 树里，不输出到 bin/
unset(EXECUTABLE_OUTPUT_PATH)

add_subdirectory(bronx)
add_subdirectory(gateway)
add_subdirectory(ipban)
add_subdirectory(api_gw)
add_subdirectory(other)

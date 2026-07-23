cmake_minimum_required(VERSION 3.20)
project(Bronx)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 导出 compile_commands.json 供 clangd 等工具使用（FORCE 确保覆盖 cache 里的旧值）
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL "" FORCE)

# 抑制 OpenSSL 弃用警告
add_definitions(-Wno-deprecated-declarations)

# --- 构建选项 ---
# Sanitizer 开关(默认空=常规构建)。用法:
#   cmake -B build-asan -DBRONX_SANITIZE=address,undefined
#   cmake -B build-tsan -DBRONX_SANITIZE=thread
set(BRONX_SANITIZE "" CACHE STRING "Enable sanitizers, e.g. address,undefined or thread")
# 测试默认关闭，开发时 cmake -DBRONX_BUILD_TESTS=ON 打开
option(BRONX_BUILD_TESTS "Build test targets" OFF)

if(BRONX_SANITIZE)
    message(STATUS "Bronx sanitizers enabled: ${BRONX_SANITIZE}")
    add_compile_options(-fsanitize=${BRONX_SANITIZE} -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=${BRONX_SANITIZE})
    # 每种 sanitizer 独立产物目录，避免 ASan 和 TSan 互相覆盖
    string(REPLACE "," "-" BRONX_SAN_TAG "${BRONX_SANITIZE}")
    set(BRONX_OUT_SUFFIX "-${BRONX_SAN_TAG}")
else()
    set(BRONX_OUT_SUFFIX "")
endif()

set(EXECUTABLE_OUTPUT_PATH ${PROJECT_SOURCE_DIR}/bin${BRONX_OUT_SUFFIX})
set(LIBRARY_OUTPUT_PATH    ${PROJECT_SOURCE_DIR}/lib${BRONX_OUT_SUFFIX})

# --- 头文件路径 ---
include_directories(${PROJECT_SOURCE_DIR}/src/bronx/env/include)
include_directories(${PROJECT_SOURCE_DIR}/src/bronx/core/include)
include_directories(${PROJECT_SOURCE_DIR}/src/bronx/net/include)
include_directories(${PROJECT_SOURCE_DIR}/src/bronx/misc/include)
include_directories(${PROJECT_SOURCE_DIR}/src/gateway/include)
include_directories(${PROJECT_SOURCE_DIR}/src/ipban/include)
include_directories(${PROJECT_SOURCE_DIR}/tools)

# --- 外部依赖 ---
find_package(yaml-cpp REQUIRED)
include_directories(${YAML_CPP_INCLUDE_DIRS})

find_package(OpenSSL REQUIRED)
include_directories(${OPENSSL_INCLUDE_DIRS})

find_package(jsoncpp REQUIRED)

# jwt-cpp (header-only，依赖已有的 OpenSSL)
find_package(jwt-cpp QUIET)
if(NOT jwt-cpp_FOUND)
    find_path(JWT_CPP_INCLUDE_DIR jwt-cpp/jwt.h
        HINTS /usr/local/include /usr/include
              "${CMAKE_SOURCE_DIR}/third_party/jwt-cpp-0.7.0/include")
    if(JWT_CPP_INCLUDE_DIR)
        include_directories(${JWT_CPP_INCLUDE_DIR})
        message(STATUS "jwt-cpp header found at ${JWT_CPP_INCLUDE_DIR}")
    else()
        message(WARNING "jwt-cpp not found; JWT auth will not compile. Install: vcpkg install jwt-cpp")
    endif()
endif()

# --- vendored sqlite3（ipban 持久化）---
# 单独编库，不受项目 sanitizer/-Wall 拖累
set(SQLITE_DIR ${PROJECT_SOURCE_DIR}/third_party/sqlite)
add_library(sqlite3 STATIC ${SQLITE_DIR}/sqlite3.c)
target_include_directories(sqlite3 PUBLIC ${SQLITE_DIR})
target_compile_definitions(sqlite3 PRIVATE
    SQLITE_THREADSAFE=1
    SQLITE_OMIT_LOAD_EXTENSION
    SQLITE_DQS=0)
target_compile_options(sqlite3 PRIVATE -w)
include_directories(${SQLITE_DIR})

# --- 源码收集（各子目录设置 src_bronx / src_gateway / src_ipban）---
add_subdirectory(src)

# tools/ 下带实现的文件
set(src_tools ${PROJECT_SOURCE_DIR}/tools/pool.cpp)

# --- 核心静态库 ---
add_library(bronx_core STATIC ${src_bronx} ${src_gateway} ${src_ipban} ${src_tools})
target_link_libraries(bronx_core pthread yaml-cpp ${OPENSSL_LIBRARIES} JsonCpp::JsonCpp sqlite3)

# --- 运行产物：gw / hub / banctl ---
add_subdirectory(api_gw)

# --- 测试（可选，默认不编译）---
if(BRONX_BUILD_TESTS)
    add_subdirectory(test)
endif()

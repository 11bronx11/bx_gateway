# 获取当前源文件所在目录的绝对路径
get_filename_component(SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}" ABSOLUTE)
aux_source_directory(${SRC_DIR} SRC_LIST)

# gateway 自带的 HTTP 解析器副本(Ragel 生成),不再依赖 http 层 mongrel
aux_source_directory(${SRC_DIR}/include/gw_parser GW_PARSER_SRC)
list(APPEND SRC_LIST ${GW_PARSER_SRC})

# 中间件实现与头文件放一起(middlewares/),同 gw_parser 逐目录收
aux_source_directory(${SRC_DIR}/include/middlewares MW_SRC)
list(APPEND SRC_LIST ${MW_SRC})

# 使用 CACHE INTERNAL 确保变量在父级 CMake 中可见
set(src_gateway ${SRC_LIST} CACHE INTERNAL "gateway module sources")

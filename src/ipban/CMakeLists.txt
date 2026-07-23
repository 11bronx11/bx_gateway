# 收 ipban 目录下所有实现文件, 打进 bronx_core
get_filename_component(SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}" ABSOLUTE)
aux_source_directory(${SRC_DIR} SRC_LIST)

set(src_ipban ${SRC_LIST} CACHE INTERNAL "ipban module sources")

# 分四层收集源文件(aux_source_directory 不递归,逐目录收)
aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR}/env/src  SRC_ENV)
aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR}/core/src SRC_CORE)
aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR}/net/src  SRC_NET)
aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR}/misc/src SRC_MISC)

# 使用 CACHE INTERNAL 确保变量在父级 CMake 中可见
set(src_bronx ${SRC_ENV} ${SRC_CORE} ${SRC_NET} ${SRC_MISC} CACHE INTERNAL "bronx core sources")

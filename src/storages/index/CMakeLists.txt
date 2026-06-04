file(GLOB_RECURSE NEUG_STORAGES_INDEX_SRC "${CMAKE_CURRENT_SOURCE_DIR}/*.cc")
add_library(neug_storages_index OBJECT ${NEUG_STORAGES_INDEX_SRC})
add_dependencies(neug_storages_index neug_proto)

set(SQLITE_FTS_EXTENSION_OBJECT_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/sqlite_fts_function.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/sqlite_fts_index.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/sqlite_fts_index_scan.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/sqlite_fts_sqlite.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/sqlite_fts_extension.cc")

include(${CMAKE_SOURCE_DIR}/cmake/BuildSQLiteAsThirdParty.cmake)

build_extension_lib("sqlite_fts")

target_include_directories(neug_sqlite_fts_extension PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(neug_sqlite_fts_extension PRIVATE
    neug
    neug_sqlite3
    ${Protobuf_LIBRARIES})
add_dependencies(neug_sqlite_fts_extension neug_sqlite3_amalgamation)

add_extension_test(
    NAME sqlite_fts
    TEST_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/tests/sqlite_fts_extension_test.cc")

if(BUILD_TEST)
    target_link_libraries(sqlite_fts_extension_test PRIVATE neug_sqlite3)
    add_dependencies(sqlite_fts_extension_test neug_sqlite3_amalgamation)
    add_dependencies(sqlite_fts_extension_test neug_sqlite_fts_extension)
endif()

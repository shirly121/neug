find_package(Git REQUIRED)
include(${CMAKE_SOURCE_DIR}/cmake/BuildSQLiteAsThirdParty.cmake)

set(ZVEC_SOURCE_DIR
    "${CMAKE_SOURCE_DIR}/third_party/zvec"
    CACHE PATH
    "Path where the ZVec source tree is cloned")
set(ZVEC_GIT_REPOSITORY
    "https://github.com/alibaba/zvec.git"
    CACHE STRING
    "Git repository used to fetch ZVec")
set(ZVEC_GIT_TAG
    "main"
    CACHE STRING
    "Git branch, tag, or commit used to fetch ZVec")

if(NOT EXISTS "${ZVEC_SOURCE_DIR}/CMakeLists.txt")
    if(EXISTS "${ZVEC_SOURCE_DIR}")
        file(GLOB _zvec_partial_contents "${ZVEC_SOURCE_DIR}/*")
        if(_zvec_partial_contents OR EXISTS "${ZVEC_SOURCE_DIR}/.git")
            message(FATAL_ERROR
                "ZVec source directory exists but is incomplete: ${ZVEC_SOURCE_DIR}. "
                "Remove it and configure again.")
        endif()
    endif()

    get_filename_component(_zvec_parent_dir "${ZVEC_SOURCE_DIR}" DIRECTORY)
    file(MAKE_DIRECTORY "${_zvec_parent_dir}")
    message(STATUS
        "ZVec source not found; cloning ${ZVEC_GIT_REPOSITORY} "
        "(${ZVEC_GIT_TAG}) into ${ZVEC_SOURCE_DIR}")
    execute_process(
        COMMAND
            "${GIT_EXECUTABLE}" clone
            --recursive
            --branch "${ZVEC_GIT_TAG}"
            "${ZVEC_GIT_REPOSITORY}"
            "${ZVEC_SOURCE_DIR}"
        RESULT_VARIABLE _zvec_clone_result
        OUTPUT_VARIABLE _zvec_clone_output
        ERROR_VARIABLE _zvec_clone_error)
    if(NOT _zvec_clone_result EQUAL 0)
        file(REMOVE_RECURSE "${ZVEC_SOURCE_DIR}")
        message(FATAL_ERROR
            "Failed to clone ZVec into ${ZVEC_SOURCE_DIR}.\n"
            "${_zvec_clone_output}\n${_zvec_clone_error}")
    endif()
endif()

if(NOT EXISTS "${ZVEC_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "ZVEC_SOURCE_DIR does not contain a ZVec source tree: "
        "${ZVEC_SOURCE_DIR}")
endif()

if(EXISTS "${ZVEC_SOURCE_DIR}/.git")
    execute_process(
        COMMAND
            "${GIT_EXECUTABLE}" submodule update --init --recursive
        WORKING_DIRECTORY "${ZVEC_SOURCE_DIR}"
        RESULT_VARIABLE _zvec_submodule_result
        OUTPUT_VARIABLE _zvec_submodule_output
        ERROR_VARIABLE _zvec_submodule_error)
    if(NOT _zvec_submodule_result EQUAL 0)
        message(FATAL_ERROR
            "Failed to initialize ZVec submodules in ${ZVEC_SOURCE_DIR}.\n"
            "${_zvec_submodule_output}\n${_zvec_submodule_error}")
    endif()
endif()

include(ExternalProject)

set(_zvec_binary_dir "${CMAKE_BINARY_DIR}/third_party/zvec-build")
set(_zvec_roaring_include_dir
    "${ZVEC_SOURCE_DIR}/thirdparty/CRoaring/CRoaring-2.0.4")
set(_zvec_roaring_library
    "${_zvec_binary_dir}/external/usr/local/lib/libroaring.a")
if(APPLE)
    execute_process(
        COMMAND sysctl -n hw.physicalcpu
        OUTPUT_VARIABLE _zvec_parallel_jobs
        OUTPUT_STRIP_TRAILING_WHITESPACE)
else()
    cmake_host_system_information(
        RESULT _zvec_parallel_jobs QUERY NUMBER_OF_PHYSICAL_CORES)
endif()
if(NOT _zvec_parallel_jobs OR _zvec_parallel_jobs LESS 1)
    set(_zvec_parallel_jobs 1)
endif()
set(_zvec_core_library "${_zvec_binary_dir}/lib/libzvec_core.a")
set(_zvec_ailego_library "${_zvec_binary_dir}/lib/libzvec_ailego.a")
set(_zvec_turbo_library "${_zvec_binary_dir}/lib/libzvec_turbo.a")

# ZVec currently creates several common third-party targets unconditionally.
# Build it in an isolated CMake project to avoid collisions with NeuG's gtest,
# gflags, glog, protobuf, yaml-cpp, and utf8proc targets.
set(_zvec_cmake_args
    "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
    "-DCMAKE_POLICY_VERSION_MINIMUM=3.5"
    "-DBUILD_ZVEC_CORE_SHARED=OFF"
    "-DBUILD_ZVEC_SHARED=OFF"
    "-DBUILD_ZVEC_AILEGO_SHARED=OFF"
    "-DBUILD_PYTHON_BINDINGS=OFF"
    "-DBUILD_C_BINDINGS=OFF"
    "-DAUTO_DETECT_ARCH=OFF"
    "-DBUILD_TOOLS=OFF")
if(APPLE AND CMAKE_OSX_ARCHITECTURES)
    list(APPEND _zvec_cmake_args
        "-DCMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}"
        "-DCMAKE_SYSTEM_PROCESSOR=${CMAKE_OSX_ARCHITECTURES}")
endif()

ExternalProject_Add(zvec_external
    SOURCE_DIR "${ZVEC_SOURCE_DIR}"
    BINARY_DIR "${_zvec_binary_dir}"
    INSTALL_COMMAND ""
    BUILD_COMMAND
        "${CMAKE_COMMAND}" --build <BINARY_DIR>
        --target zvec_core zvec_ailego zvec_turbo roaring
        --parallel ${_zvec_parallel_jobs}
      CMAKE_ARGS ${_zvec_cmake_args})

add_library(zvec_core_static STATIC IMPORTED GLOBAL)
set_target_properties(zvec_core_static PROPERTIES
    IMPORTED_LOCATION "${_zvec_core_library}"
    INTERFACE_INCLUDE_DIRECTORIES "${ZVEC_SOURCE_DIR}/src/include")
add_dependencies(zvec_core_static zvec_external)

add_library(zvec_ailego_static STATIC IMPORTED GLOBAL)
set_target_properties(zvec_ailego_static PROPERTIES
    IMPORTED_LOCATION "${_zvec_ailego_library}")
add_dependencies(zvec_ailego_static zvec_external)

add_library(zvec_turbo_static STATIC IMPORTED GLOBAL)
set_target_properties(zvec_turbo_static PROPERTIES
    IMPORTED_LOCATION "${_zvec_turbo_library}")
add_dependencies(zvec_turbo_static zvec_external)

add_library(zvec_roaring STATIC IMPORTED GLOBAL)
set_target_properties(zvec_roaring PROPERTIES
    IMPORTED_LOCATION "${_zvec_roaring_library}"
    INTERFACE_INCLUDE_DIRECTORIES "${_zvec_roaring_include_dir}")
add_dependencies(zvec_roaring zvec_external)

set(HYBRID_SEARCH_EXTENSION_OBJECT_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/hnsw_index.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/hnsw_index_scan.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/vector_distance_function.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/sqlite_fts_function.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/sqlite_fts_index.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/sqlite_fts_index_scan.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/sqlite_fts_sqlite.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/hybrid_search_extension.cc")
build_extension_lib("hybrid_search")

target_include_directories(neug_hybrid_search_extension PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${ZVEC_SOURCE_DIR}/src/include)
target_link_libraries(neug_hybrid_search_extension PRIVATE
    neug
    zvec_core_static
    zvec_ailego_static
    zvec_turbo_static
    zvec_roaring
    neug_sqlite3
    ${Protobuf_LIBRARIES})
# ZVec registers metrics and index implementations from static initializers.
# Force all registration translation units into the extension; a normal static
# archive scan would discard objects that have no directly referenced symbol.
if(APPLE)
    target_link_options(neug_hybrid_search_extension PRIVATE
        "LINKER:-force_load,$<TARGET_FILE:zvec_core_static>"
        "LINKER:-force_load,$<TARGET_FILE:zvec_ailego_static>"
        "LINKER:-force_load,$<TARGET_FILE:zvec_turbo_static>")
elseif(UNIX)
    target_link_libraries(neug_hybrid_search_extension PRIVATE
        -Wl,--whole-archive
        zvec_core_static
        zvec_ailego_static
        zvec_turbo_static
        -Wl,--no-whole-archive)
endif()
add_dependencies(neug_hybrid_search_extension
    zvec_external
    neug_sqlite3_amalgamation)
add_extension_test(
    NAME hybrid_search
    TEST_SRCS
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/hnsw_index_test.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/sqlite_fts_extension_test.cc"
    EXTRA_INCLUDE_DIRS "${ZVEC_SOURCE_DIR}/src/include"
    EXTRA_LINK_LIBS
        zvec_core_static
        zvec_ailego_static
        zvec_turbo_static
        zvec_roaring
        neug_sqlite3)

if(BUILD_TEST)
    add_dependencies(hybrid_search_extension_test
        neug_sqlite3_amalgamation
        neug_hybrid_search_extension)
endif()

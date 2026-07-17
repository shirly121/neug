find_package(Git REQUIRED)

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
        COMMAND sysctl -n hw.ncpu
        OUTPUT_VARIABLE _zvec_parallel_jobs
        OUTPUT_STRIP_TRAILING_WHITESPACE)
else()
    cmake_host_system_information(
        RESULT _zvec_parallel_jobs QUERY NUMBER_OF_PHYSICAL_CORES)
endif()
if(NOT _zvec_parallel_jobs OR _zvec_parallel_jobs LESS 1)
    set(_zvec_parallel_jobs 1)
endif()
if(APPLE)
    set(_zvec_core_library "${_zvec_binary_dir}/lib/libzvec_core.dylib")
elseif(WIN32)
    set(_zvec_core_library "${_zvec_binary_dir}/src/zvec_core.dll")
else()
    set(_zvec_core_library "${_zvec_binary_dir}/lib/libzvec_core.so")
endif()

# ZVec currently creates several common third-party targets unconditionally.
# Build it in an isolated CMake project to avoid collisions with NeuG's gtest,
# gflags, glog, protobuf, yaml-cpp, and utf8proc targets.
ExternalProject_Add(zvec_external
    SOURCE_DIR "${ZVEC_SOURCE_DIR}"
    BINARY_DIR "${_zvec_binary_dir}"
    INSTALL_COMMAND ""
    BUILD_COMMAND
        "${CMAKE_COMMAND}" --build <BINARY_DIR>
        --target zvec_core_shared roaring
        --parallel ${_zvec_parallel_jobs}
      CMAKE_ARGS
        "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
        "-DCMAKE_POLICY_VERSION_MINIMUM=3.5"
        "-DBUILD_ZVEC_CORE_SHARED=ON"
        "-DBUILD_ZVEC_SHARED=OFF"
        "-DBUILD_ZVEC_AILEGO_SHARED=OFF"
        "-DBUILD_PYTHON_BINDINGS=OFF"
        "-DBUILD_C_BINDINGS=OFF"
        "-DBUILD_TOOLS=OFF")

add_library(zvec_core_shared SHARED IMPORTED GLOBAL)
set_target_properties(zvec_core_shared PROPERTIES
    IMPORTED_LOCATION "${_zvec_core_library}"
    INTERFACE_INCLUDE_DIRECTORIES "${ZVEC_SOURCE_DIR}/src/include")
add_dependencies(zvec_core_shared zvec_external)

add_library(zvec_roaring STATIC IMPORTED GLOBAL)
set_target_properties(zvec_roaring PROPERTIES
    IMPORTED_LOCATION "${_zvec_roaring_library}"
    INTERFACE_INCLUDE_DIRECTORIES "${_zvec_roaring_include_dir}")
add_dependencies(zvec_roaring zvec_external)

set(ZVEC_EXTENSION_OBJECT_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/hnsw_index.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/hnsw_index_scan.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/vector_distance_function.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/zvec_extension.cc")
build_extension_lib("zvec")

target_include_directories(neug_zvec_extension PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${ZVEC_SOURCE_DIR}/src/include)
target_link_libraries(neug_zvec_extension PRIVATE
    neug
    zvec_core_shared
    zvec_roaring
    ${Protobuf_LIBRARIES})
add_dependencies(neug_zvec_extension zvec_external)

add_extension_test(
    NAME zvec
    TEST_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/tests/hnsw_index_test.cc"
    EXTRA_INCLUDE_DIRS "${ZVEC_SOURCE_DIR}/src/include"
    EXTRA_LINK_LIBS zvec_core_shared zvec_roaring)

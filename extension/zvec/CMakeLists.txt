# ZVec Extension - Vector index support (HNSW) backed by ZVec library
#
# Builds zvec from source via add_subdirectory. The zvec repo is cloned to
# third_party/zvec at cmake-configure time if not already present.

# ===========================================================================
# 1. Obtain zvec source
# ===========================================================================
set(ZVEC_SRC "${CMAKE_SOURCE_DIR}/third_party/zvec")
if(NOT EXISTS "${ZVEC_SRC}/CMakeLists.txt")
    message(STATUS "Cloning zvec to ${ZVEC_SRC} ...")
    execute_process(
        COMMAND git clone --depth 1 https://github.com/alibaba/zvec.git "${ZVEC_SRC}"
        RESULT_VARIABLE _zvec_clone_result
    )
    if(NOT _zvec_clone_result EQUAL 0)
        message(FATAL_ERROR "Failed to clone zvec repository.")
    endif()
    # Initialize only the header-only submodules we need. Other submodules
    # (gflags, glog, protobuf, arrow, rocksdb, etc.) are already provided by
    # NeuG and must NOT be pulled in.
    message(STATUS "Initializing zvec header-only submodules...")
    execute_process(
        COMMAND git submodule update --init --depth 1
                thirdparty/sparsehash/sparsehash-2.0.4
                thirdparty/magic_enum/magic_enum-0.9.7
                thirdparty/RaBitQ-Library/RaBitQ-Library-0.1
        WORKING_DIRECTORY "${ZVEC_SRC}"
        RESULT_VARIABLE _zvec_submodule_result
    )
    if(NOT _zvec_submodule_result EQUAL 0)
        message(FATAL_ERROR "Failed to initialize zvec submodules.")
    endif()
endif()

# ===========================================================================
# 2. Patch zvec source to remove Arrow/Parquet dependency from ailego
# ===========================================================================
# zvec_ailego's buffer layer (parquet_hash_table.cc, block_eviction_queue.cc)
# depends on Arrow parquet, but the HNSW index path never uses it. We patch
# the cloned source at cmake-configure time to:
#   a) Remove parquet_hash_table.cc (Arrow parquet API, version-incompatible)
#   b) Patch block_eviction_queue.cc to remove ParquetBufferPool references
#   c) Patch ailego/CMakeLists.txt to remove Arrow from LIBS
#
# This is the same pattern NeuG uses for Arrow's ThirdpartyToolchain.cmake.

# Sentinel: only patch once
set(_zvec_patch_sentinel "${ZVEC_SRC}/.neug_patched")
if(NOT EXISTS "${_zvec_patch_sentinel}")
    message(STATUS "Patching zvec source to remove Arrow dependency from ailego...")

    # (a) Delete parquet_hash_table.cc — uses Arrow parquet API that is
    #     version-incompatible with NeuG's Arrow 18.
    file(REMOVE "${ZVEC_SRC}/src/ailego/buffer/parquet_hash_table.cc")

    # (b) Patch block_eviction_queue.cc — remove #include of parquet_hash_table.h
    #     and replace ParquetBufferPool references with no-ops. These code paths
    #     only fire when page_table==nullptr (parquet buffer pool case), which
    #     the HNSW index never triggers.
    set(_beq_file "${ZVEC_SRC}/src/ailego/buffer/block_eviction_queue.cc")
    file(READ "${_beq_file}" _beq_content)
    string(REPLACE
        "#include <zvec/ailego/buffer/parquet_hash_table.h>"
        "// Removed: #include <zvec/ailego/buffer/parquet_hash_table.h>"
        _beq_content "${_beq_content}")
    string(REPLACE
        "ParquetBufferPool::get_instance().is_dead_node(item)"
        "false /* ParquetBufferPool removed */"
        _beq_content "${_beq_content}")
    string(REPLACE
        "ParquetBufferPool::get_instance().evict(item.parquet_buffer_block.first);"
        "/* ParquetBufferPool::evict removed */"
        _beq_content "${_beq_content}")
    file(WRITE "${_beq_file}" "${_beq_content}")

    # (c) Patch ailego/CMakeLists.txt — remove Arrow link dependencies
    set(_ailego_cml "${ZVEC_SRC}/src/ailego/CMakeLists.txt")
    file(READ "${_ailego_cml}" _ailego_content)
    string(REPLACE "Arrow::arrow_static" "" _ailego_content "${_ailego_content}")
    string(REPLACE "Arrow::parquet_static" "" _ailego_content "${_ailego_content}")
    file(WRITE "${_ailego_cml}" "${_ailego_content}")

    # Write sentinel
    file(WRITE "${_zvec_patch_sentinel}" "Patched by NeuG zvec extension CMakeLists.txt\n")
    message(STATUS "ZVec ailego patched successfully.")
endif()

# ===========================================================================
# 3. Configure zvec build environment
# ===========================================================================
# PROJECT_ROOT_DIR is used by zvec's cmake infrastructure to locate its cmake/
# and thirdparty/ directories.
set(PROJECT_ROOT_DIR "${ZVEC_SRC}")

# ZVec is C++17; bazel.cmake reads CC_CXX_STANDARD to set CXX_STANDARD on
# each target it creates, so zvec targets compile as C++17 regardless of
# NeuG's project-wide C++20 setting.
set(CC_CXX_STANDARD 17)

# Disable zvec's auto architecture detection. It checks CMAKE_SYSTEM_PROCESSOR
# (host arch) which may differ from the target arch when cross-compiling
# (e.g., x86_64 host → arm64 target via CMAKE_OSX_ARCHITECTURES). The
# auto-detected x86 march flags would then be applied to arm64 code, causing
# build failures. Users can enable specific architectures explicitly if needed.
set(AUTO_DETECT_ARCH OFF CACHE BOOL "" FORCE)

# Include zvec cmake infrastructure (defines cc_library, cc_directory, etc.)
include(${PROJECT_ROOT_DIR}/cmake/bazel.cmake)
include(${PROJECT_ROOT_DIR}/cmake/option.cmake)

# Replicate the include_directories from zvec's top-level CMakeLists.txt so
# that the src/ subdirectories can find each other's headers.
include_directories(${ZVEC_SRC}/src/include)
include_directories(${ZVEC_SRC}/src)

# ===========================================================================
# 4. Create header-only stub targets that zvec depends on
# ===========================================================================
# These are normally built by zvec's thirdparty/ CMakeLists, which we skip to
# avoid pulling in duplicate gflags/glog/protobuf/arrow/googletest builds.

# --- sparsehash ---
set(_SPARSE_SRC "${ZVEC_SRC}/thirdparty/sparsehash/sparseconfig.h")
set(_SPARSE_DST "${ZVEC_SRC}/thirdparty/sparsehash/sparsehash-2.0.4/src/sparsehash/internal/sparseconfig.h")
if(NOT EXISTS "${_SPARSE_DST}")
    get_filename_component(_sparse_dst_dir "${_SPARSE_DST}" DIRECTORY)
    file(COPY "${_SPARSE_SRC}" DESTINATION "${_sparse_dst_dir}")
endif()
if(NOT TARGET sparsehash)
    add_library(sparsehash INTERFACE)
    target_include_directories(sparsehash INTERFACE
        "${ZVEC_SRC}/thirdparty/sparsehash/sparsehash-2.0.4/src/")
endif()

# --- magic_enum ---
if(NOT TARGET magic_enum)
    add_library(magic_enum INTERFACE)
    target_include_directories(magic_enum INTERFACE
        "${ZVEC_SRC}/thirdparty/magic_enum/magic_enum-0.9.7/include/")
endif()

# --- rabitqlib ---
if(NOT TARGET rabitqlib)
    add_library(rabitqlib INTERFACE)
    target_include_directories(rabitqlib INTERFACE
        "${ZVEC_SRC}/thirdparty/RaBitQ-Library/RaBitQ-Library-0.1/include")
endif()

# ===========================================================================
# 5. RaBitQ support detection (mirrors zvec's top-level CMakeLists logic)
# ===========================================================================
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64" AND NOT ANDROID)
    include(CheckCCompilerFlag)
    check_c_compiler_flag("-mavx2" COMPILER_SUPPORTS_AVX2)
    check_c_compiler_flag("-mavx512f -mavx512bw -mavx512vl" COMPILER_SUPPORTS_AVX512)
    if(COMPILER_SUPPORTS_AVX2 OR COMPILER_SUPPORTS_AVX512)
        set(RABITQ_SUPPORTED ON)
        add_definitions(-DRABITQ_SUPPORTED=1)
        if(RABITQ_ENABLE_AVX512 AND COMPILER_SUPPORTS_AVX512)
            add_definitions(-DRABITQ_COMPILED_AVX512=1)
            set(RABITQ_ARCH_FLAG "${MATH_MARCH_FLAG_AVX512}")
        else()
            set(RABITQ_ARCH_FLAG "${MATH_MARCH_FLAG_AVX2}")
        endif()
    else()
        set(RABITQ_SUPPORTED OFF)
        add_definitions(-DRABITQ_SUPPORTED=0)
    endif()
else()
    set(RABITQ_SUPPORTED OFF)
    add_definitions(-DRABITQ_SUPPORTED=0)
endif()

# ===========================================================================
# 6. x86 march flags (needed by ailego/turbo for SIMD dispatch)
# ===========================================================================
if(NOT ANDROID AND AUTO_DETECT_ARCH AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|i686|i386|x64")
    setup_compiler_march_for_x86(
        MATH_MARCH_FLAG_SSE MATH_MARCH_FLAG_AVX2
        MATH_MARCH_FLAG_AVX512 MATH_MARCH_FLAG_AVX512FP16)
endif()

# ===========================================================================
# 7. Add zvec source subdirectories (in dependency order)
# ===========================================================================
# We add only the library targets we need. src/db and src/binding are not
# required for the HNSW index.
add_subdirectory(${ZVEC_SRC}/src/ailego ${CMAKE_CURRENT_BINARY_DIR}/zvec/ailego)
add_subdirectory(${ZVEC_SRC}/src/turbo  ${CMAKE_CURRENT_BINARY_DIR}/zvec/turbo)
add_subdirectory(${ZVEC_SRC}/src/core   ${CMAKE_CURRENT_BINARY_DIR}/zvec/core)

# ===========================================================================
# 8. Collect extension sources & build the extension shared library
# ===========================================================================
file(GLOB ZVEC_EXTENSION_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
set(ZVEC_EXTENSION_OBJECT_FILES ${ZVEC_EXTENSION_SOURCES})

build_extension_lib("zvec")

# --- Include directories for the extension wrapper code ---
target_include_directories(neug_zvec_extension PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR})

# ZVec public headers (SYSTEM to suppress warnings from third-party code)
target_include_directories(neug_zvec_extension PRIVATE SYSTEM
    ${ZVEC_SRC}/src/include)

# ===========================================================================
# 9. Link against zvec targets
# ===========================================================================
# zvec_core contains registration code that is referenced indirectly, so it
# must be force-loaded. zvec_ailego and zvec_turbo are linked normally.
if(APPLE)
    # Apple ld uses -force_load per archive.
    set(_ZVEC_LINK_LIBS
        -Wl,-force_load,$<TARGET_FILE:zvec_core>
        zvec_ailego
        zvec_turbo
    )
else()
    set(_ZVEC_LINK_LIBS
        -Wl,--whole-archive
        zvec_core
        -Wl,--no-whole-archive
        zvec_ailego
        zvec_turbo
    )
endif()

target_link_libraries(neug_zvec_extension PRIVATE
    ${_ZVEC_LINK_LIBS}
    neug
)

# ===========================================================================
# 10. Tests
# ===========================================================================
if(BUILD_TEST)
    add_extension_test(NAME zvec
        TEST_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/tests/zvec_test.cpp
        EXTRA_INCLUDE_DIRS ${ZVEC_SRC}/src/include
        EXTRA_LINK_LIBS ${_ZVEC_LINK_LIBS}
    )
endif()

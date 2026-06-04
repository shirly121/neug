# ZVec Extension - Vector index support (HNSW) backed by ZVec library
#
# Required external dependency:
#   ZVec static libraries from /Users/zhouxiaoli/Downloads/zvec/build/lib/
#   ZVec headers from /Users/zhouxiaoli/Downloads/zvec/src/include/

# --- Configuration ---
# Path to pre-built ZVec libraries
set(ZVEC_LIB_DIR "/Users/zhouxiaoli/Downloads/zvec/build/lib"
    CACHE PATH "Path to ZVec pre-built static libraries")

# Path to ZVec headers
set(ZVEC_INCLUDE_DIR "/Users/zhouxiaoli/Downloads/zvec/src/include"
    CACHE PATH "Path to ZVec header files")

# Verify ZVec paths exist
if(NOT EXISTS "${ZVEC_LIB_DIR}")
    message(FATAL_ERROR "ZVec library directory not found: ${ZVEC_LIB_DIR}\n"
                        "Please set ZVEC_LIB_DIR to the correct path.")
endif()
if(NOT EXISTS "${ZVEC_INCLUDE_DIR}")
    message(FATAL_ERROR "ZVec include directory not found: ${ZVEC_INCLUDE_DIR}\n"
                        "Please set ZVEC_INCLUDE_DIR to the correct path.")
endif()

# --- Collect source files ---
file(GLOB ZVEC_EXTENSION_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
set(ZVEC_EXTENSION_OBJECT_FILES ${ZVEC_EXTENSION_SOURCES})

# --- Build extension shared library ---
build_extension_lib("zvec")

# --- Include directories ---
target_include_directories(neug_zvec_extension PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR})

# ZVec headers (SYSTEM to suppress warnings from third-party code)
target_include_directories(neug_zvec_extension PRIVATE SYSTEM
    ${ZVEC_INCLUDE_DIR})

# --- Import ZVec static libraries ---
# Order matters for static linking: dependents before dependencies.
# core_interface depends on core_knn_hnsw, core_metric, core_framework, etc.
#
# Libraries are split into two groups:
# - ZVEC_FORCE_LOAD_LIBS: Use -force_load to ensure all symbols are included
#   (needed for libs with indirect/registration-based references)
# - ZVEC_NORMAL_LIBS: Link normally (only pull in referenced symbols)
#   This avoids dragging in unnecessary dependencies (e.g., Arrow parquet
#   from libzvec_ailego.a's internal BufferManager)
set(ZVEC_FORCE_LOAD_LIBS
    ${ZVEC_LIB_DIR}/libcore_interface.a
    ${ZVEC_LIB_DIR}/libcore_knn_hnsw.a
    ${ZVEC_LIB_DIR}/libcore_metric.a
    ${ZVEC_LIB_DIR}/libzvec_turbo.a
    ${ZVEC_LIB_DIR}/libcore_framework.a
    ${ZVEC_LIB_DIR}/libcore_utility.a
)

# These libs are linked normally (demand-driven symbol resolution)
set(ZVEC_NORMAL_LIBS
    ${ZVEC_LIB_DIR}/libzvec_ailego.a
    ${ZVEC_LIB_DIR}/libzvec_core.a
)

# Verify all required libraries exist
foreach(lib ${ZVEC_FORCE_LOAD_LIBS} ${ZVEC_NORMAL_LIBS})
    if(NOT EXISTS "${lib}")
        message(FATAL_ERROR "Required ZVec library not found: ${lib}")
    endif()
endforeach()

# --- Link libraries ---
if(APPLE)
    set(_ZVEC_LINK_LIBS "")
    foreach(lib ${ZVEC_FORCE_LOAD_LIBS})
        list(APPEND _ZVEC_LINK_LIBS "-Wl,-force_load,${lib}")
    endforeach()
    # Normal libs appended without -force_load
    list(APPEND _ZVEC_LINK_LIBS ${ZVEC_NORMAL_LIBS})
else()
    set(_ZVEC_LINK_LIBS
        -Wl,--whole-archive
        ${ZVEC_FORCE_LOAD_LIBS}
        -Wl,--no-whole-archive
        ${ZVEC_NORMAL_LIBS}
    )
endif()

target_link_libraries(neug_zvec_extension PRIVATE
    ${_ZVEC_LINK_LIBS}
    neug
)

# --- Tests ---
if(BUILD_TEST)
    add_extension_test(NAME zvec
        TEST_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/tests/zvec_test.cpp
        EXTRA_INCLUDE_DIRS ${ZVEC_INCLUDE_DIR}
        EXTRA_LINK_LIBS ${_ZVEC_LINK_LIBS}
    )
endif()

cmake_minimum_required(VERSION 3.20)
project(umbra LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Warnings
add_compile_options(
    $<$<COMPILE_LANGUAGE:CXX>:-Wall>
    $<$<COMPILE_LANGUAGE:CXX>:-Wextra>
    $<$<COMPILE_LANGUAGE:CXX>:-Werror>
)

# ── Third-party dependencies ───────────────────────────────────────────────

include(FetchContent)

# Catch2 v3
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.5.0
    GIT_SHALLOW    TRUE
)

# asmjit
FetchContent_Declare(
    asmjit
    GIT_REPOSITORY https://github.com/asmjit/asmjit.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
)

# Disable asmjit tests
set(ASMJIT_TEST OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(Catch2 asmjit)

# libpg_query — built via its own Makefile
include(ExternalProject)
include(ProcessorCount)
ProcessorCount(NPROC)
if(NOT NPROC OR NPROC EQUAL 0)
    set(NPROC 1)
endif()

ExternalProject_Add(
    libpg_query_ext
    GIT_REPOSITORY https://github.com/pganalyze/libpg_query.git
    GIT_TAG        17-6.0.0
    GIT_SHALLOW    TRUE
    CONFIGURE_COMMAND ""
    BUILD_COMMAND make -j${NPROC} -C <SOURCE_DIR>
    BUILD_IN_SOURCE TRUE
    INSTALL_COMMAND ""
    BUILD_BYPRODUCTS <SOURCE_DIR>/libpg_query.a
)

ExternalProject_Get_Property(libpg_query_ext SOURCE_DIR)
set(LIBPG_QUERY_SOURCE_DIR ${SOURCE_DIR})

# Create an imported target for libpg_query
add_library(libpg_query STATIC IMPORTED GLOBAL)
set_target_properties(libpg_query PROPERTIES
    IMPORTED_LOCATION ${LIBPG_QUERY_SOURCE_DIR}/libpg_query.a
)
target_include_directories(libpg_query INTERFACE
    ${LIBPG_QUERY_SOURCE_DIR}
)
add_dependencies(libpg_query libpg_query_ext)

# ── Testing ─────────────────────────────────────────────────────────────────

enable_testing()

# ── Subdirectories ─────────────────────────────────────────────────────────

add_subdirectory(src)
add_subdirectory(test)

# UmbraDB library
add_library(umbra_lib STATIC
    common/types.cpp
)

target_include_directories(umbra_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(umbra_lib PUBLIC
    libpg_query
    asmjit
)

# Main executable
add_executable(umbra main.cpp)
target_link_libraries(umbra PRIVATE umbra_lib)

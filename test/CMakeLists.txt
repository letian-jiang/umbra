# Unit tests
add_executable(test_smoke
    unit/test_smoke.cpp
)

target_link_libraries(test_smoke PRIVATE
    umbra_lib
    Catch2::Catch2WithMain
)

add_test(NAME smoke COMMAND test_smoke)

# Verify the main executable runs and exits 0
add_test(NAME main_runs COMMAND umbra)

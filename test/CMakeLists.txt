# Unit tests
add_executable(test_smoke
    unit/test_smoke.cpp
)

target_link_libraries(test_smoke PRIVATE
    umbra_lib
    Catch2::Catch2WithMain
)

add_test(NAME smoke COMMAND test_smoke)

# Storage tests
add_executable(test_storage
    unit/test_storage.cpp
)
target_link_libraries(test_storage PRIVATE
    umbra_lib
    Catch2::Catch2WithMain
)
add_test(NAME storage COMMAND test_storage)

# IR unit tests
add_executable(test_ir
    unit/test_ir.cpp
)
target_link_libraries(test_ir PRIVATE
    umbra_lib
    Catch2::Catch2WithMain
)
add_test(NAME ir COMMAND test_ir)

# IR programs tests
add_executable(test_ir_programs
    ir/test_ir_programs.cpp
)
target_link_libraries(test_ir_programs PRIVATE
    umbra_lib
    Catch2::Catch2WithMain
)
add_test(NAME ir_programs COMMAND test_ir_programs)

# Interpreter tests
add_executable(test_interpreter
    ir/test_interpreter.cpp
)
target_link_libraries(test_interpreter PRIVATE
    umbra_lib
    Catch2::Catch2WithMain
)
add_test(NAME interpreter COMMAND test_interpreter)

# Parser tests
add_executable(test_parser
    unit/test_parser.cpp
)
target_link_libraries(test_parser PRIVATE
    umbra_lib
    Catch2::Catch2WithMain
)
add_test(NAME parser COMMAND test_parser)

# Binder tests
add_executable(test_binder
    unit/test_binder.cpp
)
target_link_libraries(test_binder PRIVATE
    umbra_lib
    Catch2::Catch2WithMain
)
add_test(NAME binder COMMAND test_binder)

# Verify the main executable runs and exits 0
add_test(NAME main_runs COMMAND umbra)

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

# Optimizer tests
add_executable(test_optimizer
    unit/test_optimizer.cpp
)
target_link_libraries(test_optimizer PRIVATE
    umbra_lib
    Catch2::Catch2WithMain
)
add_test(NAME optimizer COMMAND test_optimizer)

# Codegen API tests
add_executable(test_codegen_api
    unit/test_codegen_api.cpp
)
target_link_libraries(test_codegen_api PRIVATE
    umbra_lib
    Catch2::Catch2WithMain
)
add_test(NAME codegen_api COMMAND test_codegen_api)

# SQL Values tests
add_executable(test_sql_values
    unit/test_sql_values.cpp
)
target_link_libraries(test_sql_values PRIVATE
    umbra_lib
    Catch2::Catch2WithMain
)
add_test(NAME sql_values COMMAND test_sql_values)

# Tuples tests
add_executable(test_tuples
    unit/test_tuples.cpp
)
target_link_libraries(test_tuples PRIVATE
    umbra_lib
    Catch2::Catch2WithMain
)
add_test(NAME tuples COMMAND test_tuples)

# Hash table tests
add_executable(test_hash_table
    unit/test_hash_table.cpp
)
target_link_libraries(test_hash_table PRIVATE
    umbra_lib
    Catch2::Catch2WithMain
)
add_test(NAME hash_table COMMAND test_hash_table)

# Operator tests
add_executable(test_operators
    unit/test_operators.cpp
)
target_link_libraries(test_operators PRIVATE
    umbra_lib
    Catch2::Catch2WithMain
)
add_test(NAME operators COMMAND test_operators)

# Pipeline tests
add_executable(test_pipeline
    ir/test_pipeline.cpp
)
target_link_libraries(test_pipeline PRIVATE
    umbra_lib
    Catch2::Catch2WithMain
)
add_test(NAME pipeline COMMAND test_pipeline)

# Flying Start JIT tests
add_executable(test_flying_start
    ir/test_flying_start.cpp
)
target_link_libraries(test_flying_start PRIVATE
    umbra_lib
    Catch2::Catch2WithMain
)
add_test(NAME flying_start COMMAND test_flying_start)

# JIT Optimizations tests
add_executable(test_jit_optimizations
    ir/test_jit_optimizations.cpp
)
target_link_libraries(test_jit_optimizations PRIVATE
    umbra_lib
    Catch2::Catch2WithMain
)
add_test(NAME jit_optimizations COMMAND test_jit_optimizations)

# EXPLAIN tests
add_executable(test_explain
    unit/test_explain.cpp
)
target_link_libraries(test_explain PRIVATE
    umbra_lib
    Catch2::Catch2WithMain
)
add_test(NAME explain COMMAND test_explain)

# Shell tests
add_executable(test_shell
    unit/test_shell.cpp
)
target_link_libraries(test_shell PRIVATE
    umbra_lib
    Catch2::Catch2WithMain
)
add_test(NAME shell COMMAND test_shell)

# Formatter tests
add_executable(test_formatter
    unit/test_formatter.cpp
)
target_link_libraries(test_formatter PRIVATE
    umbra_lib
    Catch2::Catch2WithMain
)
add_test(NAME formatter COMMAND test_formatter)

# SQL: Q1 end-to-end tests
add_executable(test_q1
    sql/test_q1.cpp
)
target_link_libraries(test_q1 PRIVATE
    umbra_lib
    Catch2::Catch2WithMain
)
add_test(NAME q1 COMMAND test_q1)

# SQL: Q1 variants (INTERVAL 60/90/120, edge cases)
add_executable(test_q1_variants
    sql/test_q1_variants.cpp
)
target_link_libraries(test_q1_variants PRIVATE
    umbra_lib
    Catch2::Catch2WithMain
)
add_test(NAME q1_variants COMMAND test_q1_variants)

# Performance: Q1 benchmark
add_executable(test_benchmark
    perf/test_benchmark.cpp
)
target_link_libraries(test_benchmark PRIVATE
    umbra_lib
    Catch2::Catch2WithMain
)
add_test(NAME benchmark COMMAND test_benchmark)

# LLVM backend tests (conditional)
if(LLVM_BACKEND)
    add_executable(test_llvm_backend
        ir/test_llvm_backend.cpp
    )
    target_link_libraries(test_llvm_backend PRIVATE
        umbra_lib
        Catch2::Catch2WithMain
    )
    add_test(NAME llvm_backend COMMAND test_llvm_backend)

    add_executable(test_adaptive_analysis
        perf/test_adaptive_analysis.cpp
    )
    target_link_libraries(test_adaptive_analysis PRIVATE
        umbra_lib
        Catch2::Catch2WithMain
    )
    add_test(NAME adaptive_analysis COMMAND test_adaptive_analysis)
endif()

# Verify the main executable runs and exits 0 (pipe mode, no input)
add_test(NAME main_runs COMMAND sh -c "echo '' | $<TARGET_FILE:umbra>")

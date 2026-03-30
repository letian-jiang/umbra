# UmbraDB library
add_library(umbra_lib STATIC
    common/types.cpp
    storage/column.cpp
    storage/table.cpp
    storage/tbl_loader.cpp
    catalog/catalog.cpp
    ir/instruction.cpp
    ir/function.cpp
    ir/program.cpp
    ir/builder.cpp
    ir/printer.cpp
    backend/interpreter/interpreter.cpp
    backend/flying_start/live_span.cpp
    backend/flying_start/register_alloc.cpp
    backend/flying_start/optimizations.cpp
    backend/flying_start/translator.cpp
    parser/ast_context.cpp
    parser/parser.cpp
    parser/ast_printer.cpp
    binder/binder.cpp
    binder/bound_plan_printer.cpp
    optimizer/optimizer.cpp
    optimizer/physical_plan_printer.cpp
    codegen/api/codegen_context.cpp
    codegen/sql_values/sql_value.cpp
    codegen/tuples/tuple_layout.cpp
    codegen/structures/hash_table.cpp
    codegen/operators/table_scan.cpp
    codegen/operators/hash_aggregate.cpp
    codegen/operators/projection.cpp
    codegen/operators/sort.cpp
    codegen/operators/result_writer.cpp
    execution/result_collector.cpp
    execution/pipeline.cpp
    explain/explain.cpp
    shell/shell.cpp
    shell/formatter.cpp
)

target_include_directories(umbra_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(umbra_lib PUBLIC
    libpg_query
    asmjit
)

# ── Optional LLVM backend ──────────────────────────────────────────────────

if(LLVM_BACKEND)
    target_sources(umbra_lib PRIVATE
        backend/llvm/llvm_compiler.cpp
    )
    target_include_directories(umbra_lib SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
    target_compile_definitions(umbra_lib PUBLIC UMBRA_LLVM_BACKEND)

    separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
    target_compile_definitions(umbra_lib PRIVATE ${LLVM_DEFINITIONS_LIST})

    llvm_map_components_to_libnames(llvm_libs
        core orcjit native support passes
    )
    target_link_libraries(umbra_lib PRIVATE ${llvm_libs})
endif()

# Main executable
add_executable(umbra main.cpp)
target_link_libraries(umbra PRIVATE umbra_lib)

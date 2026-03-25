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

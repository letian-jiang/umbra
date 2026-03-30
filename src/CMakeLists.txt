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
    # ── Compile host_inline.cpp to LLVM bitcode and embed as C array ──────
    set(HOST_INLINE_SRC "${CMAKE_CURRENT_SOURCE_DIR}/backend/llvm/host_inline.cpp")
    set(HOST_INLINE_BC  "${CMAKE_CURRENT_BINARY_DIR}/host_inline.bc")
    set(HOST_INLINE_INC "${CMAKE_CURRENT_BINARY_DIR}/host_inline_bc.inc")

    add_custom_command(
        OUTPUT "${HOST_INLINE_BC}"
        COMMAND clang++ -emit-llvm -c -O2 -std=c++20
                -o "${HOST_INLINE_BC}" "${HOST_INLINE_SRC}"
        DEPENDS "${HOST_INLINE_SRC}"
        COMMENT "Compiling host inline functions to LLVM bitcode"
    )

    add_custom_command(
        OUTPUT "${HOST_INLINE_INC}"
        COMMAND xxd -i < "${HOST_INLINE_BC}" > "${HOST_INLINE_INC}"
        DEPENDS "${HOST_INLINE_BC}"
        COMMENT "Embedding host inline bitcode as C array"
    )

    add_custom_target(host_inline_bitcode DEPENDS "${HOST_INLINE_INC}")

    # ── LLVM backend sources ─────────────────────────────────────────────
    target_sources(umbra_lib PRIVATE
        backend/llvm/llvm_compiler.cpp
        backend/llvm/host_functions_c.cpp
    )
    add_dependencies(umbra_lib host_inline_bitcode)

    target_include_directories(umbra_lib SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
    target_include_directories(umbra_lib PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    target_compile_definitions(umbra_lib PUBLIC UMBRA_LLVM_BACKEND)

    separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
    target_compile_definitions(umbra_lib PRIVATE ${LLVM_DEFINITIONS_LIST})

    llvm_map_components_to_libnames(llvm_libs
        core orcjit native support passes linker bitreader
    )
    target_link_libraries(umbra_lib PRIVATE ${llvm_libs})
endif()

# Main executable
add_executable(umbra main.cpp)
target_link_libraries(umbra PRIVATE umbra_lib)

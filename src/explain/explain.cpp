#include "explain/explain.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <sstream>
#include <string>

#include "binder/binder.h"
#include "binder/bound_plan_printer.h"
#include "catalog/catalog.h"
#include "execution/pipeline.h"
#include "execution/result_collector.h"
#include "ir/printer.h"
#include "optimizer/optimizer.h"
#include "optimizer/physical_plan_printer.h"
#include "parser/ast_printer.h"
#include "parser/parser.h"
#include "storage/table.h"

namespace umbra::explain {

namespace {

/// Convert a string to uppercase.
std::string toUpper(const std::string& str) {
    std::string result = str;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    return result;
}

/// Trim leading/trailing whitespace from a string.
std::string trim(const std::string& str) {
    auto start = str.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) {
        return "";
    }
    auto end = str.find_last_not_of(" \t\n\r");
    return str.substr(start, end - start + 1);
}

/// Helper: get current time as high_resolution_clock time_point.
auto now() {
    return std::chrono::high_resolution_clock::now();
}

/// Helper: compute elapsed milliseconds.
double elapsedMs(std::chrono::high_resolution_clock::time_point start,
                 std::chrono::high_resolution_clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

}  // namespace

bool parseExplainLevel(const std::string& str, ExplainLevel& level) {
    std::string upper = toUpper(str);
    if (upper == "AST") {
        level = ExplainLevel::AST;
    } else if (upper == "LOGICAL") {
        level = ExplainLevel::LOGICAL;
    } else if (upper == "PHYSICAL") {
        level = ExplainLevel::PHYSICAL;
    } else if (upper == "PIPELINE") {
        level = ExplainLevel::PIPELINE;
    } else if (upper == "IR") {
        level = ExplainLevel::IR;
    } else if (upper == "ASM") {
        level = ExplainLevel::ASM;
    } else if (upper == "PROFILE") {
        level = ExplainLevel::PROFILE;
    } else if (upper == "ALL") {
        level = ExplainLevel::ALL;
    } else {
        return false;
    }
    return true;
}

std::string explainLevelToString(ExplainLevel level) {
    switch (level) {
        case ExplainLevel::AST:
            return "AST";
        case ExplainLevel::LOGICAL:
            return "LOGICAL";
        case ExplainLevel::PHYSICAL:
            return "PHYSICAL";
        case ExplainLevel::PIPELINE:
            return "PIPELINE";
        case ExplainLevel::IR:
            return "IR";
        case ExplainLevel::ASM:
            return "ASM";
        case ExplainLevel::PROFILE:
            return "PROFILE";
        case ExplainLevel::ALL:
            return "ALL";
    }
    return "UNKNOWN";
}

bool parseExplainPrefix(const std::string& sql, ExplainLevel& level, std::string& remainingSql) {
    std::string trimmed = trim(sql);
    if (trimmed.size() < 7) {
        return false;
    }

    std::string prefix = toUpper(trimmed.substr(0, 7));
    if (prefix != "EXPLAIN") {
        return false;
    }

    // Skip "EXPLAIN" and whitespace.
    std::string rest = trim(trimmed.substr(7));
    if (rest.empty()) {
        return false;
    }

    // Try to parse a level keyword.
    // Find the first space/tab/newline after the potential level keyword.
    auto spacePos = rest.find_first_of(" \t\n\r");
    if (spacePos != std::string::npos) {
        std::string candidate = rest.substr(0, spacePos);
        if (parseExplainLevel(candidate, level)) {
            remainingSql = trim(rest.substr(spacePos));
            return true;
        }
    }

    // No explicit level — default to ALL, treat rest as SQL.
    level = ExplainLevel::ALL;
    remainingSql = rest;
    return true;
}

std::string runExplain(ExplainLevel level, const std::string& sql, const Catalog& catalog,
                       Table& table, execution::Backend backend) {
    std::ostringstream out;

    bool showAst = (level == ExplainLevel::AST || level == ExplainLevel::ALL ||
                    level == ExplainLevel::PROFILE);
    bool showLogical = (level == ExplainLevel::LOGICAL || level == ExplainLevel::ALL ||
                        level == ExplainLevel::PROFILE);
    bool showPhysical = (level == ExplainLevel::PHYSICAL || level == ExplainLevel::ALL ||
                         level == ExplainLevel::PROFILE);
    bool showPipeline = (level == ExplainLevel::PIPELINE || level == ExplainLevel::ALL ||
                         level == ExplainLevel::PROFILE);
    bool showIr =
        (level == ExplainLevel::IR || level == ExplainLevel::ALL || level == ExplainLevel::PROFILE);

    // For non-ALL/non-PROFILE, only show the requested level.
    if (level != ExplainLevel::ALL && level != ExplainLevel::PROFILE) {
        showAst = (level == ExplainLevel::AST);
        showLogical = (level == ExplainLevel::LOGICAL);
        showPhysical = (level == ExplainLevel::PHYSICAL);
        showPipeline = (level == ExplainLevel::PIPELINE);
        showIr = (level == ExplainLevel::IR);
    }

    auto totalStart = now();

    // === Parse ===
    auto planStart = now();
    auto parseResult = parser::Parser::parse(sql);

    if (showAst) {
        out << "AST:\n";
        out << parser::ASTPrinter::print(*parseResult.stmt);
        out << "\n";
    }

    // For AST-only, stop here.
    if (level == ExplainLevel::AST) {
        return out.str();
    }

    // === Bind ===
    binder::Binder binder(catalog);
    auto bindResult = binder.bind(*parseResult.stmt);

    if (showLogical) {
        out << "Logical Plan:\n";
        out << binder::BoundPlanPrinter::print(bindResult.root);
        out << "\n";
    }

    // For LOGICAL-only, stop here.
    if (level == ExplainLevel::LOGICAL) {
        return out.str();
    }

    // === Optimize ===
    optimizer::Optimizer optimizer;
    auto optResult = optimizer.optimize(bindResult);
    auto planEnd = now();

    if (showPhysical) {
        out << "Physical Plan:\n";
        out << optimizer::PhysicalPlanPrinter::printPlan(optResult.root);
        out << "\n";
    }

    // For PHYSICAL-only, stop here.
    if (level == ExplainLevel::PHYSICAL) {
        return out.str();
    }

    if (showPipeline) {
        out << "Pipelines:\n";
        out << optimizer::PhysicalPlanPrinter::printPipelines(optResult.pipelines);
        out << "\n";
    }

    // For PIPELINE-only, stop here.
    if (level == ExplainLevel::PIPELINE) {
        return out.str();
    }

    // === IR: generate IR via codegen (no execution) ===
    if (showIr && level == ExplainLevel::IR) {
        execution::PipelineExecutor executor(backend);
        out << "IR:\n";
        out << executor.generateIR(optResult);
        out << "\n";
        return out.str();
    }

    // === ASM: generate assembly or LLVM IR via the selected backend ===
    if (level == ExplainLevel::ASM) {
#ifdef UMBRA_LLVM_BACKEND
        if (backend == execution::Backend::LLVM) {
            execution::PipelineExecutor executor(execution::Backend::LLVM);
            out << "LLVM IR (optimized, O3):\n";
            out << executor.generateASM(optResult);
            out << "\n";
        } else
#endif
        {
            if (backend == execution::Backend::LLVM) {
                out << "(LLVM backend not compiled; showing Flying Start ASM instead)\n\n";
            }
            execution::PipelineExecutor executor(execution::Backend::FLYING_START);
            out << "ASM (x86-64, Flying Start):\n";
            out << executor.generateASM(optResult);
            out << "\n";
        }
        return out.str();
    }

    // For ALL level, show IR placeholder (codegen happens at execution time).
    if (showIr && level != ExplainLevel::IR) {
        out << "IR: (use EXPLAIN IR to see generated IR)\n";
        out << "\n";
    }

    // For PROFILE, execute with per-pipeline timing breakdown.
    if (level == ExplainLevel::PROFILE) {
        double planMs = elapsedMs(planStart, planEnd);

        execution::ResultCollector collector;
        execution::PipelineExecutor executor(backend);
        executor.enableProfiling();
        executor.execute(optResult, table, collector);

        const auto& profiles = executor.profiles();
        double totalMs = elapsedMs(totalStart, now());

        // Sum per-pipeline phases.
        double totalCodegen = 0.0;
        double totalCompile = 0.0;
        double totalExecute = 0.0;
        for (const auto& p : profiles) {
            totalCodegen += p.codegenMs;
            totalCompile += p.compileMs;
            totalExecute += p.executeMs;
        }

        out << std::fixed;
        out.precision(2);

        std::string backendName;
        switch (backend) {
            case execution::Backend::INTERPRETER:
                backendName = "interpreter";
                break;
            case execution::Backend::FLYING_START:
                backendName = "flying-start";
                break;
            case execution::Backend::LLVM:
                backendName = "llvm";
                break;
            case execution::Backend::ADAPTIVE:
                backendName = "adaptive";
                break;
        }
        out << "Profile (backend: " << backendName << "):\n";
        if (backend == execution::Backend::ADAPTIVE) {
            out << "                  plan    codegen  compile(FS) compile(LLVM)  execute  total\n";
        } else {
            out << "                  plan    codegen  compile  execute  total\n";
        }
        for (size_t i = 0; i < profiles.size(); ++i) {
            const auto& p = profiles[i];
            double pTotal = p.codegenMs + p.compileMs + p.executeMs;
            out << "Pipeline " << i << ":       -       " << p.codegenMs << "ms   " << p.compileMs
                << "ms   ";
            if (backend == execution::Backend::ADAPTIVE) {
                if (p.llvmCompileMs > 0.0) {
                    out << p.llvmCompileMs << "ms      ";
                } else {
                    out << "(bg, unused)   ";
                }
            }
            out << p.executeMs << "ms   " << pTotal << "ms";
            if (p.totalMorsels > 0) {
                out << "  (" << p.totalMorsels << " morsels)";
            }
            out << "\n";
        }
        out << "Total:           " << planMs << "ms   " << totalCodegen << "ms   " << totalCompile
            << "ms   " << totalExecute << "ms   " << totalMs << "ms\n";

        // Adaptive execution morsel breakdown.
        if (backend == execution::Backend::ADAPTIVE) {
            out << "\n";
            for (size_t i = 0; i < profiles.size(); ++i) {
                const auto& p = profiles[i];
                if (p.totalMorsels > 0) {
                    out << "  Pipeline " << i << ": " << p.fsMorsels << "/" << p.totalMorsels
                        << " morsels via Flying Start, " << p.llvmMorsels << "/" << p.totalMorsels
                        << " via LLVM.";
                    if (p.switchMorsel > 0) {
                        out << " Switch at morsel " << p.switchMorsel << ".";
                    } else {
                        out << " LLVM detached.";
                    }
                    out << "\n";
                }
            }
        }

        out << "\nResult: " << collector.numRows() << " rows\n";
    }

    return out.str();
}

}  // namespace umbra::explain

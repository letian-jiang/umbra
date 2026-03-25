#include "shell/shell.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

#include "binder/binder.h"
#include "catalog/catalog.h"
#include "common/types.h"
#include "execution/pipeline.h"
#include "execution/result_collector.h"
#include "explain/explain.h"
#include "optimizer/optimizer.h"
#include "parser/parser.h"
#include "shell/formatter.h"
#include "storage/table.h"

namespace umbra::shell {

namespace {

/// Trim leading/trailing whitespace.
std::string trim(const std::string& str) {
    auto start = str.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) {
        return "";
    }
    auto end = str.find_last_not_of(" \t\n\r");
    return str.substr(start, end - start + 1);
}

/// Convert a string to lowercase.
std::string toLower(const std::string& str) {
    std::string result = str;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return result;
}

/// Type name for .schema display.
std::string typeToString(SQLType type) {
    switch (type.id) {
        case SQLTypeId::INTEGER:
            return "INTEGER";
        case SQLTypeId::BIGINT:
            return "BIGINT";
        case SQLTypeId::DATE:
            return "DATE";
        case SQLTypeId::DECIMAL:
            return "DECIMAL(" + std::to_string(type.precision) + "," + std::to_string(type.scale) +
                   ")";
        case SQLTypeId::VARCHAR:
            return "VARCHAR(" + std::to_string(type.maxLen) + ")";
        case SQLTypeId::BOOLEAN:
            return "BOOLEAN";
    }
    return "UNKNOWN";
}

}  // namespace

Shell::Shell(Catalog& catalog, Table& table, ShellConfig config)
    : catalog_(catalog), table_(table), config_(std::move(config)) {}

void Shell::printBanner(std::ostream& out) const {
    out << "UmbraDB v0.1 (" << table_.rowCount << " rows loaded)\n";
    out << "Type .help for commands, Ctrl-D to exit.\n";
}

int Shell::run() {
    if (config_.interactive) {
        printBanner(std::cout);
        std::cout << "\n";
    }

    while (true) {
        std::string stmt = readStatement(std::cin, config_.interactive);
        if (stmt.empty()) {
            break;  // EOF
        }

        if (!executeStatement(stmt, std::cout, std::cerr)) {
            return 0;  // .quit/.exit
        }
    }

    if (config_.interactive) {
        std::cout << "\nBye!\n";
    }

    return 0;
}

bool Shell::executeStatement(const std::string& sql, std::ostream& out, std::ostream& err) {
    std::string trimmed = trim(sql);
    if (trimmed.empty()) {
        return true;
    }

    // Remove trailing semicolon.
    if (!trimmed.empty() && trimmed.back() == ';') {
        trimmed.pop_back();
        trimmed = trim(trimmed);
    }

    if (trimmed.empty()) {
        return true;
    }

    if (trimmed[0] == '.') {
        return executeDotCommand(trimmed, out, err);
    }

    executeQuery(trimmed, out, err);
    return true;
}

bool Shell::executeDotCommand(const std::string& cmd, std::ostream& out, std::ostream& err) {
    std::string lower = toLower(cmd);

    if (lower == ".quit" || lower == ".exit") {
        return false;
    }

    if (lower == ".help") {
        out << ".help              Show this help\n";
        out << ".quit              Exit shell\n";
        out << ".tables            List loaded tables\n";
        out << ".schema <table>    Show table schema\n";
        out << ".timer on|off      Toggle query timing\n";
        return true;
    }

    if (lower == ".tables") {
        for (const auto& [name, def] : catalog_.tables) {
            out << name << "\n";
        }
        return true;
    }

    if (lower.starts_with(".schema")) {
        std::string arg = trim(cmd.substr(7));
        if (arg.empty()) {
            // Show all tables.
            for (const auto& [name, def] : catalog_.tables) {
                out << "Table: " << name << "\n";
                for (const auto& col : def.columns) {
                    out << "  " << col.columnIndex << "  " << col.name << "  "
                        << typeToString(col.type) << "\n";
                }
            }
        } else {
            const TableDef* tableDef = catalog_.lookupTable(arg);
            if (tableDef == nullptr) {
                err << "Error: table \"" << arg << "\" not found\n";
            } else {
                out << "Table: " << tableDef->name << " (" << table_.rowCount << " rows)\n";
                for (const auto& col : tableDef->columns) {
                    out << "  " << col.columnIndex << "  " << col.name << "  "
                        << typeToString(col.type) << "\n";
                }
            }
        }
        return true;
    }

    if (lower.starts_with(".timer")) {
        std::string arg = trim(lower.substr(6));
        if (arg == "on") {
            config_.timerEnabled = true;
            out << "Timer enabled.\n";
        } else if (arg == "off") {
            config_.timerEnabled = false;
            out << "Timer disabled.\n";
        } else {
            err << "Usage: .timer on|off\n";
        }
        return true;
    }

    err << "Error: unknown command \"" << cmd << "\". Use .help for available commands.\n";
    return true;
}

void Shell::executeQuery(const std::string& sql, std::ostream& out, std::ostream& err) {
    try {
        // Check for EXPLAIN prefix.
        explain::ExplainLevel level{};
        std::string remainingSql;
        if (explain::parseExplainPrefix(sql, level, remainingSql)) {
            std::string result = explain::runExplain(level, remainingSql, catalog_, table_);
            out << result;
            return;
        }

        // Normal query execution.
        auto start = std::chrono::high_resolution_clock::now();
        auto parseResult = parser::Parser::parse(sql);
        binder::Binder binder(catalog_);
        auto bindResult = binder.bind(*parseResult.stmt);
        optimizer::Optimizer opt;
        auto optResult = opt.optimize(bindResult);
        execution::ResultCollector collector;
        execution::PipelineExecutor executor;
        executor.execute(optResult, table_, collector);
        double elapsedMs = std::chrono::duration<double, std::milli>(
                               std::chrono::high_resolution_clock::now() - start)
                               .count();
        out << (config_.interactive ? formatTable(collector) : formatTsv(collector));
        if (config_.timerEnabled || config_.interactive) {
            out << collector.numRows() << " row" << (collector.numRows() != 1 ? "s" : "") << " ("
                << std::fixed << std::setprecision(2) << elapsedMs << " ms)\n";
        }

    } catch (const parser::ParseError& e) {
        err << "Parse error: " << e.what() << "\n";
    } catch (const binder::BindError& e) {
        err << "Bind error: " << e.what() << "\n";
    } catch (const std::exception& e) {
        err << "Error: " << e.what() << "\n";
    }
}

std::string Shell::readStatement(std::istream& in, bool interactive) {
    std::string buffer;
    std::string line;
    bool firstLine = true;

    while (true) {
        if (interactive) {
            if (firstLine) {
                std::cout << "umbra> ";
            } else {
                std::cout << "   ...> ";
            }
            std::cout.flush();
        }

        if (!std::getline(in, line)) {
            // EOF
            if (!buffer.empty()) {
                return buffer;
            }
            return "";
        }

        if (firstLine) {
            buffer = line;
        } else {
            buffer += "\n" + line;
        }
        firstLine = false;

        // Check if the buffer contains a semicolon.
        if (buffer.find(';') != std::string::npos) {
            return buffer;
        }

        // Check for dot commands (single line, no semicolon needed).
        std::string trimmed = trim(buffer);
        if (!trimmed.empty() && trimmed[0] == '.') {
            return buffer;
        }
    }
}

}  // namespace umbra::shell

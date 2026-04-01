#pragma once

#include <iostream>
#include <optional>
#include <string>

#include "catalog/catalog.h"
#include "execution/pipeline.h"
#include "storage/table.h"

namespace umbra::shell {

/// Shell configuration.
struct ShellConfig {
    bool interactive{true};                                    // Interactive REPL mode.
    bool timerEnabled{false};                                  // Show timing after each query.
    std::string dataDir{"."};                                  // Directory for .tbl files.
    execution::Backend backend{execution::Backend::ADAPTIVE};  // Execution backend.
    uint64_t morselSize{execution::DEFAULT_MORSEL_SIZE};       // Morsel size for adaptive.
};

/// Interactive REPL for UmbraDB.
///
/// Supports:
/// - SQL query execution
/// - EXPLAIN at multiple levels
/// - Dot commands (.help, .quit, .tables, .schema, etc.)
/// - Multi-line input (accumulate until ';')
/// - Non-interactive pipe mode
class Shell {
   public:
    Shell(Catalog& catalog, Table& table, ShellConfig config);
    ~Shell();

    Shell(const Shell&) = delete;
    Shell& operator=(const Shell&) = delete;
    Shell(Shell&&) = delete;
    Shell& operator=(Shell&&) = delete;

    /// Run the REPL loop. Returns the exit code.
    int run();

    /// Execute a single SQL statement (for non-interactive use or testing).
    /// @param sql The SQL string.
    /// @param out Output stream for results.
    /// @param err Output stream for errors.
    /// @return true if execution succeeded (false only for .quit/.exit).
    bool executeStatement(const std::string& sql, std::ostream& out, std::ostream& err);

    /// Read a complete statement (possibly multi-line) from a stream.
    /// Returns empty string on EOF.
    static std::string readStatement(std::istream& in, bool interactive);

    /// Print the welcome banner.
    void printBanner(std::ostream& out) const;

   private:
    Catalog& catalog_;
    Table& table_;
    ShellConfig config_;
    std::string historyFile_;

    /// Execute a dot command. Returns true if the shell should continue.
    bool executeDotCommand(const std::string& cmd, std::ostream& out, std::ostream& err);

    /// Execute a SQL query and print results.
    void executeQuery(const std::string& sql, std::ostream& out, std::ostream& err);

    /// Read a single line using readline (interactive) or getline (pipe).
    /// Returns std::nullopt on EOF.
    std::optional<std::string> readLine(std::istream& in, const char* prompt);

    /// Read a complete statement using readline for interactive mode.
    std::string readStatementInteractive(std::istream& in);
};

}  // namespace umbra::shell

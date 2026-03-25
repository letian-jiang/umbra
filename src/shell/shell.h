#pragma once

#include <iostream>
#include <string>

#include "catalog/catalog.h"
#include "storage/table.h"

namespace umbra::shell {

/// Shell configuration.
struct ShellConfig {
    bool interactive{true};    // Interactive REPL mode.
    bool timerEnabled{false};  // Show timing after each query.
    std::string dataDir{"."};  // Directory for .tbl files.
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

    /// Execute a dot command. Returns true if the shell should continue.
    bool executeDotCommand(const std::string& cmd, std::ostream& out, std::ostream& err);

    /// Execute a SQL query and print results.
    void executeQuery(const std::string& sql, std::ostream& out, std::ostream& err);
};

}  // namespace umbra::shell

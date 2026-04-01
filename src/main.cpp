#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>

#include "catalog/catalog.h"
#include "shell/shell.h"
#include "storage/table.h"
#include "storage/tbl_loader.h"

namespace {

/// Check if stdin is connected to a terminal.
bool isInteractive() {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    return isatty(fileno(stdin)) != 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    // Parse command-line arguments.
    std::string dataDir = "data/tpch";
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        if ((arg == "--data" || arg == "-d") && i + 1 < argc) {
            dataDir = argv[i + 1];  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
            ++i;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: umbra [options]\n";
            std::cout << "  --data <dir>   Directory containing .tbl files (default: .)\n";
            std::cout << "  --help         Show this help\n";
            return 0;
        }
    }

    // Build catalog.
    umbra::Catalog catalog;
    auto lineitemDef = umbra::lineitemTableDef();
    catalog.addTable(lineitemDef);

    // Try to load data.
    umbra::Table table;
    table.name = "lineitem";
    std::string tblPath = dataDir + "/lineitem.tbl";
    if (std::filesystem::exists(tblPath)) {
        table = umbra::loadTblFile(tblPath, lineitemDef);
    }

    // Configure shell.
    umbra::shell::ShellConfig config;
    config.interactive = isInteractive();
    config.dataDir = dataDir;

    // Run shell.
    umbra::shell::Shell shell(catalog, table, config);
    return shell.run();
}

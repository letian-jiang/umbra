# CLAUDE.md

## Project

UmbraDB MVP — a JIT-compiling OLAP database inspired by Umbra, targeting TPC-H Q1.

- C++20, x86-64 Linux, CMake build system
- Design docs in `doc/`, overview in `DESIGN.md`

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

Debug build:

```bash
cmake -B build-debug -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="--coverage"
cmake --build build-debug -j$(nproc)
```

Sanitizer build (ASan + UBSan):

```bash
cmake -B build-san -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
cmake --build build-san -j$(nproc)
cd build-san && ctest --output-on-failure
```

## Test

```bash
cd build && ctest --output-on-failure
```

## Lint

```bash
# clang-tidy (uses .clang-tidy config)
find src -name '*.cpp' -o -name '*.h' | xargs clang-tidy -p build

# clang-format check (uses .clang-format config)
find src test -name '*.cpp' -o -name '*.h' | xargs clang-format --dry-run --Werror
```

## Coverage

```bash
# 需要 Debug + --coverage build
cd build-debug && ctest --output-on-failure
lcov --capture --directory . --output-file coverage.info --ignore-errors mismatch
lcov --remove coverage.info '/usr/*' '*/third_party/*' '*/test/*' --output-file coverage.info
genhtml coverage.info --output-directory coverage_report
# 查看总覆盖率
lcov --summary coverage.info
```

## Commit Requirements

Every commit must satisfy ALL of the following before being created:

1. **Lint clean**: `clang-tidy` and `clang-format` report zero errors on all changed files
2. **Compiles**: both Release and Debug builds succeed with zero warnings (`-Wall -Wextra -Werror`)
3. **All tests pass**: `ctest` exits 0 — unit tests, IR tests, and SQL tests
4. **Sanitizers clean**: all tests pass under ASan + UBSan with zero errors
5. **Coverage thresholds**:
   - New/changed code: **≥ 90% line coverage**
   - Full project: **≥ 90% line coverage**

If any check fails, fix the issue before committing. Do not use `--no-verify` or skip any check.

### Verification workflow

```bash
# 1. Format
find src test -name '*.cpp' -o -name '*.h' | xargs clang-format -i

# 2. Build (Release, warnings as errors)
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS="-Wall -Wextra -Werror"
cmake --build build -j$(nproc)

# 3. Lint
find src -name '*.cpp' -o -name '*.h' | xargs clang-tidy -p build --warnings-as-errors='*'

# 4. Test
cd build && ctest --output-on-failure

# 5. Sanitizers (ASan + UBSan)
cmake -B build-san -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all -Wall -Wextra"
cmake --build build-san -j$(nproc)
cd build-san && ctest --output-on-failure

# 6. Coverage (Debug build with --coverage)
cmake -B build-debug -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="--coverage -Wall -Wextra"
cmake --build build-debug -j$(nproc)
cd build-debug && ctest --output-on-failure
lcov --capture --directory . --output-file coverage.info --ignore-errors mismatch
lcov --remove coverage.info '/usr/*' '*/third_party/*' '*/test/*' --output-file coverage.info
# Check thresholds
COVERAGE=$(lcov --summary coverage.info 2>&1 | grep 'lines' | grep -oP '[\d.]+(?=%)')
echo "Line coverage: ${COVERAGE}%"
# Must be >= 90.0
```

## Code Style

- Follow existing code conventions in each file
- No trailing whitespace
- Include guards: `#pragma once`
- Naming: `PascalCase` for types, `camelCase` for functions/variables, `UPPER_SNAKE` for constants/macros
- Prefer `std::` over raw pointers where ownership is involved
- No `using namespace std;` in headers

## Design–Code Consistency

Design documents in `doc/` are the source of truth for architecture and data structures. **Any code change that alters design (data layouts, interfaces, algorithms, IR encoding, pipeline structure, etc.) must include corresponding updates to the relevant `doc/*.md` files in the same commit.**

Specifically:

- **Adding/removing/renaming** an IR opcode, type, or instruction encoding → update `doc/ir.md`
- **Changing** Tidy Tuples layer interfaces, operator translator logic, or codegen patterns → update `doc/tidy-tuples.md`
- **Changing** Flying Start register allocation, optimization passes, or translation rules → update `doc/flying-start.md`
- **Changing** storage layout, type representations, or column format → update `doc/storage.md`
- **Changing** AST node types, parser conversion logic → update `doc/parser.md`
- **Changing** binding rules, type inference, aggregate analysis → update `doc/binder.md`
- **Changing** optimization rules, physical plan mapping, pipeline decomposition → update `doc/optimizer.md`
- **Changing** interpreter execution model, value storage, instruction dispatch → update `doc/interpreter.md`
- **Changing** EXPLAIN levels, output format → update `doc/explain.md`
- **Changing** shell commands, REPL behavior, key bindings → update `doc/shell.md`
- **Changing** test strategy, test categories, coverage approach → update `doc/testing.md`
- **Changing** overall architecture, dependencies, file structure, scope → update `DESIGN.md`

If a code change contradicts what a design doc says, the commit must fix the doc — do not leave known inconsistencies. When in doubt, update the doc.

## Architecture

See `DESIGN.md` for overview and links to all design documents in `doc/`.

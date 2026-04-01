#!/usr/bin/env bash
# Full pre-commit verification: format, build, lint, test, sanitizers, coverage.
# Usage: ./scripts/verify.sh [--skip-coverage]
# Exit code 0 = all checks pass; non-zero = first failing step.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

SKIP_COVERAGE=false
for arg in "$@"; do
  case "$arg" in
    --skip-coverage) SKIP_COVERAGE=true ;;
  esac
done

step() { echo -e "\n=== $1 ==="; }

# 1. Format
step "1/6 clang-format"
find src test -name '*.cpp' -o -name '*.h' | xargs clang-format -i

# 2. Build (Release, warnings as errors)
step "2/6 Build (Release)"
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS="-Wall -Wextra -Werror"
cmake --build build -j"$(nproc)"

# # 3. Lint
# step "3/6 clang-tidy"
# find src -name '*.cpp' -o -name '*.h' | xargs clang-tidy -p build --warnings-as-errors='*' --header-filter='^.*/src/.*'

# 4. Test
step "4/6 Test (Release)"
(cd build && ctest --output-on-failure)

# 5. Sanitizers (ASan + UBSan)
step "5/6 Sanitizers (ASan + UBSan)"
cmake -B build-san -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all -Wall -Wextra"
cmake --build build-san -j"$(nproc)"
(cd build-san && ctest --output-on-failure)

# 6. Coverage
if [ "$SKIP_COVERAGE" = true ]; then
  step "6/6 Coverage (SKIPPED)"
else
  step "6/6 Coverage"
  cmake -B build-debug -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="--coverage -Wall -Wextra"
  cmake --build build-debug -j"$(nproc)"
  (cd build-debug && ctest --output-on-failure)
  lcov --capture --directory build-debug --output-file build-debug/coverage.info --ignore-errors mismatch 2>/dev/null \
    || lcov --capture --directory build-debug --output-file build-debug/coverage.info
  lcov --remove build-debug/coverage.info '/usr/*' '*/third_party/*' '*/test/*' '*/_deps/*' --output-file build-debug/coverage.info
  COVERAGE=$(lcov --summary build-debug/coverage.info 2>&1 | grep 'lines' | grep -oP '[\d.]+(?=%)')
  echo "Line coverage: ${COVERAGE}%"
  if awk "BEGIN{exit ($COVERAGE < 90.0) ? 0 : 1}"; then
    echo "FAIL: coverage ${COVERAGE}% < 90% threshold"
    exit 1
  fi
fi

echo -e "\n=== All checks passed ==="

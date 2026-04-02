#!/usr/bin/env bash
# cmake-format-all.sh — format every CMake file in the repository,
# skipping submodules/ and all build_* directories.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR/.."

ERRORS=0
COUNT=0

while IFS= read -r -d '' file; do
    if cmake-format --in-place "$file"; then
        COUNT=$((COUNT + 1))
    else
        echo "FAILED: $file" >&2
        ERRORS=$((ERRORS + 1))
    fi
done < <(find . \
    -type d \( -name "submodules" -o -name "build_*" \) -prune \
    -o -type f \( -name "CMakeLists.txt" -o -name "*.cmake" \) -print0)

echo "cmake-format: formatted $COUNT file(s), $ERRORS error(s)."
exit $ERRORS

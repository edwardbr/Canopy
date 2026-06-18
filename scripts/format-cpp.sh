#!/usr/bin/env sh
#
# format-cpp.sh - Run clang-format across project C/C++ files
#
# Formats all C/C++ source and header files in-place, excluding submodules
# and build output directories.

set -e

REPO_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
export REPO_ROOT

if [ -n "${CLANG_FORMAT:-}" ]; then
    CLANG_FORMAT_BIN=$CLANG_FORMAT
else
    CLANG_FORMAT_BIN=
    USER_BASE=$(python3 -m site --user-base 2>/dev/null || true)

    for candidate in \
        clang-format-23 \
        clang-format-22 \
        "${USER_BASE}/bin/clang-format" \
        clang-format
    do
        if command -v "$candidate" >/dev/null 2>&1; then
            version=$("$candidate" --version | sed -n 's/.*version \([0-9][0-9]*\).*/\1/p')
            if [ "$version" -ge 22 ] 2>/dev/null; then
                CLANG_FORMAT_BIN=$candidate
                break
            fi
        fi
    done

    if [ -z "$CLANG_FORMAT_BIN" ]; then
        echo "error: clang-format 22 or newer is required by .clang-format" >&2
        echo "hint: install clang-format 22+, or set CLANG_FORMAT=/path/to/clang-format" >&2
        exit 1
    fi
fi

export CLANG_FORMAT_BIN

python3 - <<'PY'
import os
import subprocess
from pathlib import Path

root = Path(os.environ['REPO_ROOT'])
clang_format = os.environ['CLANG_FORMAT_BIN']
extensions = {'.c', '.cc', '.cpp', '.cxx', '.h', '.hh', '.hpp', '.hxx', '.ipp', '.tpp'}
excluded = {'.git', 'submodules'}

files = []
for path in root.rglob('*'):
    if not path.is_file():
        continue

    parts = path.relative_to(root).parts
    if any(part in excluded for part in parts):
        continue
    if any(part.startswith('build_') for part in parts):
        continue
    if path.suffix.lower() in extensions:
        files.append(str(path))

files.sort()
if files:
    subprocess.run([clang_format, '-i', *files], check=True)
PY

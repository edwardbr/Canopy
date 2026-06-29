#!/usr/bin/env sh
#
# format-cpp.sh - Run clang-format across project C/C++ files
#
# Formats all C/C++ source and header files in-place, excluding submodules
# and build output directories.  Files are processed in parallel across all
# available CPU cores.

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
import multiprocessing
import os
import subprocess
from concurrent.futures import ThreadPoolExecutor

root      = os.environ['REPO_ROOT']
clang_fmt = os.environ['CLANG_FORMAT_BIN']

EXTENSIONS = frozenset({
    '.c', '.cc', '.cpp', '.cxx',
    '.h', '.hh', '.hpp', '.hxx',
    '.ipp', '.tpp',
})

# Read every submodule path from .gitmodules so we never descend into one.
submodule_dirs = set()
gitmodules = os.path.join(root, '.gitmodules')
if os.path.isfile(gitmodules):
    with open(gitmodules) as fh:
        for line in fh:
            line = line.strip()
            if line.startswith('path ='):
                submodule_dirs.add(line.split('=', 1)[1].strip())

files = []
for dirpath, dirnames, filenames in os.walk(root, topdown=True):
    rel = os.path.relpath(dirpath, root)

    # Prune directories in-place so os.walk never descends into them.
    keep = []
    for d in dirnames:
        if d.startswith('.') or d.startswith('build_'):
            continue
        child_rel = d if rel == '.' else os.path.join(rel, d)
        if child_rel in submodule_dirs:
            continue
        keep.append(d)
    dirnames[:] = keep

    for name in filenames:
        if os.path.splitext(name)[1].lower() in EXTENSIONS:
            files.append(os.path.join(dirpath, name))

files.sort()

BATCH   = 8
workers = multiprocessing.cpu_count()

def fmt(batch):
    subprocess.run([clang_fmt, '-i', *batch], check=True)

with ThreadPoolExecutor(max_workers=workers) as pool:
    batches  = [files[i:i + BATCH] for i in range(0, len(files), BATCH)]
    futures  = [pool.submit(fmt, b) for b in batches]
    for fut in futures:
        fut.result()   # re-raises on non-zero clang-format exit
PY

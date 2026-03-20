#!/usr/bin/env sh
#
# format-cpp.sh - Run clang-format across project C/C++ files
#
# Formats all C/C++ source and header files in-place, excluding submodules
# and build output directories.

set -e

REPO_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
export REPO_ROOT

python3 - <<'PY'
import os
import subprocess
from pathlib import Path

root = Path(os.environ['REPO_ROOT'])
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
    subprocess.run(['clang-format', '-i', *files], check=True)
PY

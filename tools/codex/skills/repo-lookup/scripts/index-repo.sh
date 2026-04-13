#!/usr/bin/env bash
set -euo pipefail

ROOT="${1:-.}"
OUTDIR="${2:-.codex-index}"

if ! command -v rg >/dev/null 2>&1; then
    echo "error: rg is required" >&2
    exit 1
fi

if ! command -v ctags >/dev/null 2>&1; then
    echo "error: ctags is required" >&2
    exit 1
fi

cd "$ROOT"
mkdir -p "$OUTDIR"

FILES_LIST="$OUTDIR/files.txt"
TAGS_FILE="$OUTDIR/tags"

rg --files --hidden \
    -g '!.git' \
    -g '!build*/**' \
    -g '!node_modules/**' \
    -g '!dist/**' \
    -g '!coverage/**' \
    -g '!Testing/**' \
    -g '!submodules/**' \
    -g '!c++/submodules/**' \
    > "$FILES_LIST"

ctags -f "$TAGS_FILE" -L "$FILES_LIST"

echo "indexed $(wc -l < "$FILES_LIST") files"
echo "file list: $FILES_LIST"
echo "tags: $TAGS_FILE"

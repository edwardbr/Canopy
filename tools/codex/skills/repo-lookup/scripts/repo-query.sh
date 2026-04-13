#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
usage:
  repo-query.sh files <pattern> [root]
  repo-query.sh text <pattern> [root] [paths...]
  repo-query.sh defs <symbol> [root]
  repo-query.sh refs <pattern> [root] [paths...]
  repo-query.sh target <target-name> [root]
  repo-query.sh compile-db [root]
EOF
}

if ! command -v rg >/dev/null 2>&1; then
    echo "error: rg is required" >&2
    exit 1
fi

COMMAND="${1:-}"

if [[ -z "$COMMAND" ]]; then
    usage
    exit 1
fi

case "$COMMAND" in
    files)
        PATTERN="${2:-}"
        ROOT="${3:-.}"
        [[ -n "$PATTERN" ]] || { usage; exit 1; }
        cd "$ROOT"
        rg --files --hidden -g '!.git' | rg -n "$PATTERN"
        ;;
    text)
        PATTERN="${2:-}"
        ROOT="${3:-.}"
        shift 3 || true
        [[ -n "$PATTERN" ]] || { usage; exit 1; }
        cd "$ROOT"
        if [[ "$#" -gt 0 ]]; then
            rg -n -S "$PATTERN" "$@"
        else
            rg -n -S "$PATTERN" .
        fi
        ;;
    defs)
        SYMBOL="${2:-}"
        ROOT="${3:-.}"
        [[ -n "$SYMBOL" ]] || { usage; exit 1; }
        cd "$ROOT"
        TAGS_FILE=".codex-index/tags"
        if [[ ! -f "$TAGS_FILE" ]]; then
            echo "error: $TAGS_FILE not found. Run scripts/index-repo.sh first." >&2
            exit 1
        fi
        rg -n "^${SYMBOL}[[:space:]]" "$TAGS_FILE" || true
        ;;
    refs)
        PATTERN="${2:-}"
        ROOT="${3:-.}"
        shift 3 || true
        [[ -n "$PATTERN" ]] || { usage; exit 1; }
        cd "$ROOT"
        if [[ "$#" -gt 0 ]]; then
            rg -n -S "$PATTERN" "$@"
        else
            rg -n -S "$PATTERN" .
        fi
        ;;
    target)
        TARGET="${2:-}"
        ROOT="${3:-.}"
        [[ -n "$TARGET" ]] || { usage; exit 1; }
        cd "$ROOT"
        rg -n -S "$TARGET" --glob 'CMakeLists.txt' --glob '*.cmake' .
        ;;
    compile-db)
        ROOT="${2:-.}"
        cd "$ROOT"
        find . -name compile_commands.json \
            -not -path '*/submodules/*' \
            -not -path '*/c++/submodules/*' \
            | sort
        ;;
    *)
        usage
        exit 1
        ;;
esac

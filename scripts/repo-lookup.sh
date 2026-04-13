#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

QUERY_SCRIPT="$REPO_ROOT/tools/codex/skills/repo-lookup/scripts/repo-query.sh"
COMMAND="${1:-}"
PATTERN="${2:-}"

case "$COMMAND" in
    files|defs|target)
        exec "$QUERY_SCRIPT" "$COMMAND" "$PATTERN" "$REPO_ROOT"
        ;;
    text|refs)
        if [[ "$#" -ge 2 ]]; then
            shift 2
            exec "$QUERY_SCRIPT" "$COMMAND" "$PATTERN" "$REPO_ROOT" "$@"
        fi
        ;;
    compile-db)
        exec "$QUERY_SCRIPT" compile-db "$REPO_ROOT"
        ;;
esac

exec "$QUERY_SCRIPT" "$@"

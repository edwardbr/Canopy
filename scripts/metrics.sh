#!/usr/bin/env bash

set -euo pipefail

if ! command -v scc >/dev/null 2>&1; then
    echo "error: 'scc' is not installed or not on PATH" >&2
    exit 1
fi

mode="${1:-owned}"

case "$mode" in
    owned)
        exec scc . \
            --exclude-dir .git,submodules,c++/submodules,documents,.opencode,build_debug,build_release,build_debug_coroutine,build_release_coroutine
        ;;
    core)
        exec scc \
            c++/rpc \
            c++/transports \
            c++/tests \
            c++/streaming \
            c++/subcomponents \
            generator \
            rust \
            interfaces \
            scripts \
            cmake \
            --no-cocomo
        ;;
    full)
        exec scc .
        ;;
    help|-h|--help)
        cat <<'EOF'
Usage: scripts/metrics.sh [owned|core|full]

Modes:
  owned  Repo-owned code, excluding vendor trees, docs, and common build outputs.
  core   Core implementation areas, excluding telemetry web assets.
  full   Raw repo-wide scc output.
EOF
        ;;
    *)
        echo "error: unknown mode '$mode'" >&2
        echo "run 'scripts/metrics.sh --help' for usage" >&2
        exit 1
        ;;
esac

#!/usr/bin/env sh
#
# format-staged.sh - Run clang-format and cmake-format on staged files
#
# Formats all staged C/C++ and CMake files in-place, then re-stages them
# so the commit contains the formatted versions.
#
# Exit codes:
#   0 - success (files formatted and re-staged, or nothing to format)
#   1 - a formatting tool failed

set -e

REPO_ROOT=$(git rev-parse --show-toplevel)

# Get staged files (Added, Copied, Modified) — exclude deleted files
STAGED=$(git diff --cached --name-only --diff-filter=ACM)

if [ -z "$STAGED" ]; then
    exit 0
fi

# ── C / C++ formatting ────────────────────────────────────────────────────────

if command -v clang-format >/dev/null 2>&1; then
    CPP_FILES=""
    for f in $STAGED; do
        case "$f" in
            # Skip generated output and submodules
            build/*|build_*|submodules/*) continue ;;
        esac
        case "$f" in
            *.c|*.cc|*.cpp|*.cxx|*.h|*.hh|*.hpp|*.hxx)
                CPP_FILES="$CPP_FILES $REPO_ROOT/$f"
                ;;
        esac
    done

    if [ -n "$CPP_FILES" ]; then
        # shellcheck disable=SC2086
        clang-format -i $CPP_FILES
        for f in $STAGED; do
            case "$f" in
                build/*|build_*|submodules/*) continue ;;
            esac
            case "$f" in
                *.c|*.cc|*.cpp|*.cxx|*.h|*.hh|*.hpp|*.hxx)
                    git add "$REPO_ROOT/$f"
                    ;;
            esac
        done
    fi
else
    echo "Warning: clang-format not found, skipping C/C++ formatting" >&2
fi

# ── CMake formatting ──────────────────────────────────────────────────────────

if command -v cmake-format >/dev/null 2>&1; then
    CMAKE_FILES=""
    for f in $STAGED; do
        case "$f" in
            build/*|build_*|submodules/*) continue ;;
        esac
        case "$f" in
            CMakeLists.txt|*.cmake)
                CMAKE_FILES="$CMAKE_FILES $REPO_ROOT/$f"
                ;;
        esac
    done

    if [ -n "$CMAKE_FILES" ]; then
        # shellcheck disable=SC2086
        cmake-format -i $CMAKE_FILES
        for f in $STAGED; do
            case "$f" in
                build/*|build_*|submodules/*) continue ;;
            esac
            case "$f" in
                CMakeLists.txt|*.cmake)
                    git add "$REPO_ROOT/$f"
                    ;;
            esac
        done
    fi
else
    echo "Warning: cmake-format not found, skipping CMake formatting" >&2
fi

exit 0

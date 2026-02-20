#!/usr/bin/env sh
#
# install-hooks.sh - Install the composite pre-commit hook
#
# Run this once after cloning, and again after 'bd hooks install'
# regenerates the standard shim.
#
# The composite hook runs clang-format + cmake-format on staged files
# before delegating to bd's pre-commit logic.

set -e

REPO_ROOT=$(git rev-parse --show-toplevel)
HOOK="$REPO_ROOT/.git/hooks/pre-commit"

cat > "$HOOK" << 'EOF'
#!/usr/bin/env sh
#
# Composite pre-commit hook: formatting + bd shim
#

# ── Format staged files ───────────────────────────────────────────────────────
REPO_ROOT=$(git rev-parse --show-toplevel)
if [ -x "$REPO_ROOT/scripts/format-staged.sh" ]; then
    "$REPO_ROOT/scripts/format-staged.sh" || exit 1
fi

# ── BD shim (beads issue tracking) ───────────────────────────────────────────
if ! command -v bd >/dev/null 2>&1; then
    echo "Warning: bd command not found in PATH, skipping bd pre-commit hook" >&2
    exit 0
fi

exec bd hooks run pre-commit "$@"
EOF

chmod +x "$HOOK"
echo "Installed composite pre-commit hook at $HOOK"

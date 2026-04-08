#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

cd "${repo_root}/rust"
cargo fmt --all

cd "${repo_root}"
cargo fmt --manifest-path integration_tests/fuzz_test/rust_child/Cargo.toml

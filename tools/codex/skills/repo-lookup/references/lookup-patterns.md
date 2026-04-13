# Lookup Patterns

Use these patterns to keep repository lookup fast and predictable.

## First Pass

- Find the project map: `rg --files | rg 'AGENTS.md|llms.txt|README.md|CMakeLists.txt|CMakePresets.json'`
- Find a filename: `scripts/repo-query.sh files 'transport|child_service|rpc_test'`
- Find text: `scripts/repo-query.sh text 'CanopyGenerate|rpc::shared_ptr|CANOPY_BUILD_COROUTINE'`

## Build And CMake

- Find where a target is mentioned: `scripts/repo-query.sh target rpc_test`
- Find compile databases: `scripts/repo-query.sh compile-db`
- If a C++ repo has no compile database yet, configure an existing build with `CMAKE_EXPORT_COMPILE_COMMANDS=ON` before relying on `clangd`.

## Definitions And References

- Build the local index once per repo checkout: `scripts/index-repo.sh`
- Look up likely definitions from ctags: `scripts/repo-query.sh defs child_service`
- Find textual references: `scripts/repo-query.sh refs 'child_service'`

## Heuristics

- Prefer `rg` for discovery.
- Prefer `ctags` for quick symbol entry points.
- Prefer `clangd` for semantic C and C++ navigation when compile commands exist.
- Prefer build files and source code over docs when the two disagree.

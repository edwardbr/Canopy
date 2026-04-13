---
name: repo-lookup
description: Efficient repository discovery and codebase lookup using fast local tools. Use when Codex needs to find authoritative files, search text, locate symbol definitions, find references, inspect CMake targets, or build a lightweight local index for an unfamiliar repo.
---

# Repo Lookup

Use this skill to answer repository questions with local tools before falling back to broader exploration.

## Workflow

1. Identify the repository root and any obvious source-of-truth files such as `AGENTS.md`, `llms.txt`, `README.md`, `CMakeLists.txt`, and `CMakePresets.json`.
2. Use `scripts/repo-query.sh` for immediate lookup:
   - `files <pattern>` for filenames
   - `text <pattern> [paths...]` for content search
   - `defs <symbol>` for indexed definitions from ctags
   - `refs <symbol>` for textual references
   - `target <name>` for CMake target discovery
   - `compile-db` to locate `compile_commands.json`
3. If symbol lookup is needed and the repo has no index yet, run `scripts/index-repo.sh`.
4. For C and C++ repositories, prefer `clangd` once a valid `compile_commands.json` exists. Use `repo-query.sh compile-db` to find it.
5. Prefer authoritative code and build files over prose docs when they disagree.

## Decision Guide

- Start with `files` when you do not know where something lives.
- Use `text` for strings, macros, error text, flags, comments, and config keys.
- Use `target` for build questions such as "where is this executable defined?"
- Use `defs` after indexing when the question is about declarations or symbol entry points.
- Use `refs` when the question is "who uses this?" and a semantic index is unavailable.
- Use `clangd` in an editor or client that supports it when compile commands are present and semantic navigation matters.

## Notes

- `rg` is the default discovery tool because it is fast and respects ignore files.
- The bundled index excludes common generated and vendor-like directories by default so lookups stay focused.
- `ctags` is approximate but useful for quick jumps.
- `clangd` is more accurate for C and C++, but only after compile flags are available.

## Resources

- `scripts/index-repo.sh`: build a lightweight local file list and ctags index under `.codex-index/`
- `scripts/repo-query.sh`: query files, text, definitions, references, CMake targets, and compile databases
- `references/lookup-patterns.md`: concise lookup patterns and escalation guidance

<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# Canopy Development Guide

## Git Policy

**Do not run git commands in this repository unless the user explicitly asks for them.**

- Do not fetch, pull, push, rebase, commit, stash, reset, or inspect git state without authorization.
- If a task would normally end with git operations, stop after local verification and report what remains.

## Source Of Truth

The source of truth is the live repository state:

- `CMakeLists.txt`
- `CMakePresets.json`
- files under `rpc/`, `generator/`, `transports/`, `streaming/`, `tests/`, `types/`, and `telemetry/`

Do not rely on `documents/` for correctness. It may be useful for background, but it is not authoritative.

## Overview

Canopy is a modern C++ RPC library with generated proxy/stub code from IDL files. It supports multiple transport layers, optional coroutine builds, JSON schema metadata, enclave-related builds, demos, and benchmarks.

## Repository Structure

### Main Directories

- `rpc/` - core RPC library
  - public headers: `rpc/include/rpc`
  - implementation: `rpc/src`
  - generated/public interfaces: `rpc/interfaces`
- `generator/` - IDL code generator
- `transports/` - transport implementations
  - current transport subdirectories include `direct`, `local`, `mock_test`, `sgx`, `streaming`
- `streaming/` - coroutine-only streaming stack and tests
- `types/` - additional types, including JSON support
- `telemetry/` - telemetry/logging support
- `tests/` - host tests, fixtures, fuzz tests, unit tests, schema tests, serializer tests
- `subcomponents/` - reusable support components such as `network_config`, `spsc_queue`, and `http_server`
- `submodules/` - third-party dependencies and parser components, including `idlparser`
- `demos/` - example programs
- `benchmarking/` - benchmark targets
- `cmake/` - CMake modules

### Important Notes

- Build outputs are preset-specific. Do not assume a single `build/` directory.
- Generated files appear under the active binary directory, typically in `<binaryDir>/generated/`.

## Build System

### Baseline

- CMake minimum version: `3.24`
- Generator: `Ninja`
- Default compilers in presets: `clang` / `clang++`
- Language standard:
  - C++17 by default
  - C++20 when `CANOPY_BUILD_COROUTINE=ON`

### Configure Presets

Current top-level configure presets are defined in `CMakePresets.json`. The commonly useful ones are:

- `Debug` -> binary dir `build_debug`
- `Debug_Agressive`
- `Debug_ASAN`
- `Debug-clang-18`
- `Debug_GCC`
- `Debug_Coverage`
- `Debug_Hang_On_Assert`
- `Debug_Coroutine` -> binary dir `build_debug_coroutine`
- `Debug_Coroutine_ASAN`
- `Debug_Coroutine-GCC`
- `Debug_Coroutines_With_No_Logging`
- `Debug_Coroutine_With_Full_Logging`
- `Debug_Coroutine_Coverage`
- `Debug_Coroutine_Tidy`
- `Release` -> binary dir `build_release`
- `Release-clang-18`
- `Release_Coroutines` -> binary dir `build_release_coroutine`
- `Release_Coroutine_with_No_logging`
- `Release_with_coroutines_GCC`
- `Debug_SGX`
- `Debug_SGX_Sim`
- `Release_SGX`
- `Release_SGX_Sim`

Use the exact preset names from `CMakePresets.json`. Do not normalize or rename them in instructions.

### Build Behaviour

- `CANOPY_BUILD_TEST` defaults to `ON` in the base preset.
- `CANOPY_BUILD_DEMOS` defaults to `ON`.
- `CANOPY_BUILD_BENCHMARKING` defaults to `ON`.
- `CANOPY_BUILD_COROUTINE` defaults to `OFF`.
- `streaming/` is only added when coroutine builds are enabled.
- `tests/test_enclave` is only added when `CANOPY_BUILD_ENCLAVE=ON`.
- `tests/json_schema_test` is only added when `NLOHMANN_JSON_CONFIG_INSTALL_DIR` is defined.

## Common Commands

### Configure

```bash
cmake --preset Debug
cmake --preset Debug_Coroutine
cmake --preset Release
```

### Build

```bash
cmake --build build_debug
cmake --build build_debug_coroutine
cmake --build build_release
```

To build a specific target:

```bash
cmake --build build_debug --target rpc_test
cmake --build build_debug --target fuzz_test_main
cmake --build build_debug_coroutine --target io_uring_stream_test
```

### IDL Regeneration

After editing IDL, rebuild the relevant target or rebuild the active binary directory.

```bash
cmake --build build_debug --target generator
cmake --build build_debug --target <interface_name>_idl
cmake --build build_debug
```

Do not assume generated code lands in source directories. Check the active binary directory first.

## Testing

### Primary Test Targets

Current directly named test executables include:

- `rpc_test`
- `serialiser_test`
- `fuzz_test_main`
- `json_schema_metadata_test` when JSON schema test support is enabled
- `member_ptr_test`
- `passthrough_test`
- `zone_address_test`
- multiple targets under `tests/std_test`
- `io_uring_stream_test` in coroutine builds

### Running Tests

Examples:

```bash
./build_debug/output/rpc_test --telemetry-console
./build_debug/output/fuzz_test_main 3
./build_debug/output/json_schema_metadata_test
./build_debug_coroutine/output/io_uring_stream_test
```

Notes:

- `rpc_test` supports `--telemetry-console`.
- `fuzz_test_main` is registered with CTest to run multiple iterations.
- `memory_tests` exists in the tree but is currently not added by `tests/CMakeLists.txt`.
- VS Code test discovery is configured to match `build*/output/*`.

Prefer running the smallest relevant target first, then broaden if needed.

## Style And Editing Expectations

- Follow `.clang-format`.
- The repository uses WebKit-derived formatting with:
  - `IndentWidth: 4`
  - `BreakBeforeBraces: Allman`
  - `PointerAlignment: Left`
  - `SortIncludes: false`
- Preserve existing comments unless they are wrong or actively misleading.
- Keep naming and surrounding style consistent with the local file.
- Baseline code should remain valid in non-coroutine builds unless the change is explicitly coroutine-only.

## Coroutine And Blocking Builds

- Canopy supports both blocking and coroutine builds.
- `CORO_TASK`, `CO_RETURN`, and `CO_AWAIT` are compatibility macros used across the codebase.
- Coroutine-specific code paths are guarded by `CANOPY_BUILD_COROUTINE`.
- If you change shared logic, consider both:
  - `Debug`
  - `Debug_Coroutine`

## Pointer And Ownership Rules

- `rpc::shared_ptr` and `std::shared_ptr` are not interchangeable.
- Do not cast between `rpc::shared_ptr` and `std::shared_ptr`.
- Do not use raw-pointer conversions to bridge them.
- Marshalled IDL interfaces use `rpc::shared_ptr` or `rpc::optimistic_ptr`.
- Core ownership outside the marshalled interface layer is usually `std::shared_ptr`.

## Architecture Notes That Still Matter In Code

- Stub and proxy lifetime management is central to correctness.
- Hierarchical transports intentionally maintain parent/child lifetime links.
- `child_service`, passthroughs, and service proxies all participate in transport lifetime management.
- Changes touching transport shutdown, status propagation, service ownership, or cross-zone references need extra scrutiny.

Verify behaviour in code before restating architectural claims.

## Logging And Telemetry

- Prefer structured logging macros such as `RPC_INFO` and `RPC_WARNING`.
- Telemetry is enabled in the `Debug` preset and disabled in several reduced-logging presets.
- `CANOPY_USE_TELEMETRY_RAII_LOGGING` should only be enabled deliberately for investigation because it is expensive.

## Working Practices

- Validate repository facts from code and CMake, not from old prose.
- When changing build, generator, IDL, transport, or lifetime behaviour, inspect the nearest `CMakeLists.txt` and implementation files first.
- If code changes affect both coroutine and non-coroutine paths, verify both builds when practical.
- If a test or target is conditionally compiled, mention that condition explicitly in your handoff.

## Session Completion

When ending a session:

1. Run the relevant local verification for the files you changed.
2. State clearly what you verified and what you did not verify.
3. Do not perform git or remote issue-tracker actions unless the user explicitly requested them.
4. Note any follow-up work that remains.

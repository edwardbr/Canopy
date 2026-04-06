<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# c_abi

Low-level C++ support for the shared `c_abi/` transport boundary.

## Purpose

This directory is the C++ home for cross-language transport integration work.
It should not change the existing `dynamic_library` transport behaviour.

The first responsibility here is simple:

- load a child shared library that exports the shared `canopy_dll_*` symbols
- resolve those symbols safely
- provide a small C++ wrapper that higher-level integration work can build on

## Relationship To `dynamic_library`

- `../dynamic_library/` remains the existing C++ transport
- `../c_abi/` is for the new shared ABI path
- this directory is where C++ integration code for that ABI should live

## Current Scope

- shared-library loading
- `canopy_dll_*` symbol resolution
- support for low-level C++ <-> Rust smoke tests

## Not Yet Here

- a full `rpc::transport` implementation
- generated interface integration
- coroutine support

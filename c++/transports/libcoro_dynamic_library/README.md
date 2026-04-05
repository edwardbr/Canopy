<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# libcoro_dynamic_library

Coroutine DLL transport for in-process child zones.

## Purpose

Loads a child zone from a shared object into the current process, but uses a
coroutine-oriented ABI rather than the blocking `canopy_dll_*` API.

## Main pieces

- `transport_libcoro_dynamic_library`
  - host-side transport linked into the executable
- `transport_libcoro_dynamic_library_dll`
  - static library linked into the shared object being loaded

## User extension point

The DLL author provides:

- `canopy_libcoro_dll_init`
- `rpc_log`

The transport library provides `canopy_libcoro_dll_create` and the rest of the
cross-boundary plumbing.

## Difference from `dynamic_library`

- coroutine build only
- coroutine function-pointer ABI
- still in-process, so it is not a process-isolation transport

## Related transports

- `../dynamic_library/`
  - blocking in-process DLL transport
- `../libcoro_spsc_dynamic_dll/`
  - coroutine DLL runtime used when the DLL is hosted behind an SPSC stream
- `../ipc_transport/`
  - process-owning transport used when the child must live in another process

<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# libcoro_dll_scheduled_dynamic_library

Coroutine DLL transport for in-process child zones that run on a DLL-owned
scheduler.

## Purpose

Loads a child zone from a shared object into the current process, but uses a
coroutine-oriented ABI rather than the blocking `canopy_dll_*` API. The loaded
shared object creates its own scheduler and keeps a blocking entry thread alive
until the DLL-side transport dies.

## Main pieces

- `transport_libcoro_dll_scheduled_dynamic_library`
  - host-side transport linked into the executable
- `transport_libcoro_dll_scheduled_dynamic_library_dll`
  - static library linked into the shared object being loaded

## User extension point

The DLL author provides:

- `canopy_libcoro_dll_scheduled_dll_init`
- `rpc_log`

The transport library provides `canopy_libcoro_dll_scheduled_dll_start` and the rest of the
cross-boundary begin/complete callback plumbing.

## Difference from `dynamic_library`

- coroutine build only
- non-blocking begin/complete callback ABI
- the loaded shared object owns its scheduler and keeps its entry thread blocked
  until the DLL-side transport dies
- ready and shutdown handshakes use mutex-backed one-shot state, not scheduler
  task counts
- still in-process, so it is not a process-isolation transport

## Difference from `libcoro_host_scheduled_dynamic_library`

`libcoro_host_scheduled_dynamic_library` passes the host scheduler into the DLL
runtime. This transport deliberately owns the scheduler inside the DLL, making it
closer to an isolated runtime lifecycle and a stronger clean unload/reload test.

## Unload Contract

The loader uses `RTLD_NOW | RTLD_LOCAL` on ELF platforms. It deliberately does
not use `RTLD_NODELETE`: a transport shutdown must let the DLL entry point
return, join the entry thread, and then `dlclose` the shared object. A later
load of the same DLL must start with clean DLL-local state rather than seeing
static storage left behind by a previous load.

The CMake helper for libcoro DLLs also requests `-fno-gnu-unique` for GCC ELF
builds. GNU unique C++ symbols can cause the dynamic loader to keep a shared
object resident even when the caller did not pass `RTLD_NODELETE`.

The `libcoro_dll_scheduled_dynamic_library_lifecycle.dll_static_state_resets_after_transport_unload`
test exercises this contract with a test-only DLL static probe.

## Related transports

- `../dynamic_library/`
  - blocking in-process DLL transport
- `../libcoro_spsc_dynamic_dll/`
  - coroutine DLL runtime used when the DLL is hosted behind an SPSC stream
- `../ipc_transport/`
  - process-owning transport used when the child must live in another process

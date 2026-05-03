<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# libcoro_host_scheduled_dynamic_library

Coroutine DLL transport for in-process child zones that run on the host
scheduler.

## Purpose

Loads a child zone from a shared object into the current process, but uses a
coroutine-oriented ABI rather than the blocking `canopy_dll_*` API. The host
passes its scheduler into the DLL runtime, so both sides schedule coroutine work
through the same scheduler instance.

## Main pieces

- `transport_libcoro_host_scheduled_dynamic_library`
  - host-side transport linked into the executable
- `transport_libcoro_host_scheduled_dynamic_library_dll`
  - static library linked into the shared object being loaded

## User extension point

The DLL author provides:

- `canopy_libcoro_host_scheduled_dll_init`
- `rpc_log`

The transport library provides `canopy_libcoro_host_scheduled_dll_create`.
That entry point is called synchronously during connection to exchange raw
`coro::task` function pointers for host-to-DLL and DLL-to-host calls.

## Difference from `dynamic_library`

- coroutine build only
- direct coroutine function-pointer ABI
- the loaded shared object shares the host scheduler; it does not own an entry
  thread or a scheduler
- still in-process, so it is not a process-isolation transport

## Difference from `libcoro_dll_scheduled_dynamic_library`

`libcoro_dll_scheduled_dynamic_library` creates and owns a scheduler inside the
loaded shared object. This transport instead uses the host scheduler. It is a
useful lower-overhead plugin-style test, while the DLL-scheduled transport is
the stronger lifecycle-isolation test.

## Unload Contract

The loader uses `RTLD_NOW | RTLD_LOCAL` on ELF platforms. It deliberately does
not use `RTLD_NODELETE`.

Because this transport runs DLL coroutine code on host scheduler worker
threads, clean `dlclose` is deferred until that host scheduler has stopped. A
worker thread can hold DLL-owned thread-local destructors; unloading the shared
object before the worker thread exits can leave glibc with destructor pointers
to unmapped code. This is the core lifecycle difference from
`libcoro_dll_scheduled_dynamic_library`, whose scheduler is owned by the loaded
DLL runtime and can be stopped as part of transport shutdown.

This means repeated clean reloads on the same still-running host scheduler are
not the purpose of this variant. Use the DLL-scheduled transport when each
transport teardown must fully unload the shared object and reset DLL-local
static state immediately.

The CMake helper for libcoro DLLs also requests `-fno-gnu-unique` for GCC ELF
builds. GNU unique C++ symbols can cause the dynamic loader to keep a shared
object resident even when the caller did not pass `RTLD_NODELETE`.

The `libcoro_host_scheduled_dynamic_library_lifecycle.dll_static_state_resets_after_transport_unload`
test exercises this contract with a test-only DLL static probe.

## Related transports

- `../dynamic_library/`
  - blocking in-process DLL transport
- `../libcoro_spsc_dynamic_dll/`
  - coroutine DLL runtime used when the DLL is hosted behind an SPSC stream
- `../ipc_transport/`
  - process-owning transport used when the child must live in another process

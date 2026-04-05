<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# libcoro_spsc_dynamic_dll

Coroutine DLL runtime hosted behind an SPSC stream.

## Purpose

Hosts a child zone from a shared object using a dedicated coroutine scheduler
and an SPSC queue pair. This is the DLL-side runtime used when the host is not
loading the DLL directly into its own process.

## What it owns

- the DLL-side scheduler thread
- the `streaming::spsc_queue::stream`
- the `rpc::stream_transport` used inside the DLL runtime
- parent-expired callback when the host-side transport goes away

## What it does not own

- creation of the shared-memory queue pair
- spawning the child process
- reaping the child process

Those responsibilities belong to `../ipc_transport/`.

## User extension point

The DLL author provides:

- `canopy_libcoro_spsc_dll_init`
- `rpc_log`

The library exports `canopy_libcoro_spsc_dll_start` and the runtime plumbing.

## Typical composition

- host process uses `rpc::ipc_transport`
- child process executable is `ipc_child_host_process`
- that executable loads a DLL linked against `transport_libcoro_spsc_dll_host`

## Related transports

- `../libcoro_dynamic_library/`
  - coroutine DLL loading in the current process
- `../ipc_transport/`
  - process-owning transport and child-process bootstrap

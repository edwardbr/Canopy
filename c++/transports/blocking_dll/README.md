<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# blocking_dll

Blocking / non-coroutine DLL transport.

## Purpose

Loads a child zone from a shared object into the current process and communicates
with it through a C ABI. This gives a zone boundary and symbol isolation without
adding process isolation.

## Main pieces

- `transport_blocking_dll`
  - host-side transport linked into the executable
- `transport_blocking_dll_runtime`
  - static library linked into the shared object being loaded

## User extension point

The DLL author provides:

- `canopy_module_init` by including `<rpc_objects/object_registration.h>`
- `rpc_log`

The transport registration adapter provides `canopy_dll_init`; the transport
library provides the rest of the exported `canopy_dll_*` ABI.

## Use this when

- you need a plugin model
- you want a child zone in the same process
- you do not need coroutine support

## Related transports

- `../shared_scheduler_dll/`
  - coroutine in-process DLL loading using the host scheduler
- `../unshared_scheduler_dll/`
  - coroutine in-process DLL loading with a DLL-owned scheduler
- `../ipc_spsc/`
  - SPSC-backed IPC transport for out-of-process child hosting

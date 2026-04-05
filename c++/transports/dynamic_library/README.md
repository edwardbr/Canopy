<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# dynamic_library

Blocking / non-coroutine DLL transport.

## Purpose

Loads a child zone from a shared object into the current process and communicates
with it through a C ABI. This gives a zone boundary and symbol isolation without
adding process isolation.

## Main pieces

- `transport_dynamic_library`
  - host-side transport linked into the executable
- `transport_dynamic_library_dll`
  - static library linked into the shared object being loaded

## User extension point

The DLL author provides:

- `canopy_dll_init`
- `rpc_log`

The transport library provides the rest of the exported `canopy_dll_*` ABI.

## Use this when

- you need a plugin model
- you want a child zone in the same process
- you do not need coroutine support

## Related transports

- `../libcoro_dynamic_library/`
  - coroutine equivalent for in-process DLL loading
- `../libcoro_spsc_dynamic_dll/`
  - coroutine DLL runtime for use behind an SPSC stream
- `../ipc_transport/`
  - process-owning transport for out-of-process child hosting

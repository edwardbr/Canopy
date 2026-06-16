<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# ipc_spsc

Coroutine SPSC transport built on top of `rpc::stream_transport`.

## Purpose

Encapsulates:

- creation of a shared-memory SPSC queue pair
- spawning a child process executable
- connecting two independently launched processes through a named shared-memory file
- optional OS-enforced child termination when the parent dies unexpectedly
- cleanup and reaping of the child process when the transport disconnects

## Key point

`ipc_spsc` is the transport. `sidecar_process/` contains bootstrap
helpers, not a separate transport.

## Runtime options

- `sidecar_process/`
  - builds the `canopy_ipc_spsc_sidecar_process` executable
  - maps the queue pair and loads a DLL which uses
    `../ipc_spsc/`
  - exposes header-only helpers for application-owned direct process
    executables, where concrete service interfaces belong to the application or
    test that owns them
- shared-file peer mode
  - one independently launched process creates the shared-memory file and
    accepts
  - another independently launched process opens that file and connects

## Options

`rpc::ipc_spsc::options` selects:

- `process_executable`
- `dll_path` when using the DLL-host child
- `dll_zone`
- `process_kind`
  - `host_dll`
  - `direct_service` (API present, but the in-tree direct-service executable is
    currently disabled)
- `kill_child_on_parent_death`

On Linux, `kill_child_on_parent_death` uses `PR_SET_PDEATHSIG(SIGKILL)`.

## Use this when

- you need process isolation
- you want the transport to own the child process lifetime
- you want two existing processes to pair over a unique shared-memory file
- you want either direct child hosting or DLL hosting behind the same SPSC IPC
  mechanism

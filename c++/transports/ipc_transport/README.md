<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# ipc_transport

Process-owning coroutine transport built on top of `rpc::stream_transport`.

## Purpose

Encapsulates:

- creation of a shared-memory SPSC queue pair
- spawning a child process executable
- optional OS-enforced child termination when the parent dies unexpectedly
- cleanup and reaping of the child process when the transport disconnects

## Key point

`ipc_transport` is the transport. The child executables under this directory are
bootstrap helpers, not transports.

## Child runtime options

- `ipc_child_host_process/`
  - maps the queue pair and loads a DLL which uses
    `../libcoro_spsc_dynamic_dll/`
- `ipc_child_process/`
  - maps the queue pair and hosts a `rpc::stream_transport` directly in the
    child process executable

## Options

`rpc::ipc_transport::options` selects:

- `process_executable`
- `dll_path` when using the DLL-host child
- `dll_zone`
- `process_kind`
  - `host_dll`
  - `direct_service`
- `kill_child_on_parent_death`

On Linux, `kill_child_on_parent_death` uses `PR_SET_PDEATHSIG(SIGKILL)`.

## Use this when

- you need process isolation
- you want the transport to own the child process lifetime
- you want either direct child hosting or DLL hosting behind the same SPSC IPC
  mechanism

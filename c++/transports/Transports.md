<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# Transport Overview

This directory contains the built-in Canopy transport implementations and a few
small child-process runtimes that exist to support those transports.

## Main transports

- `direct/`
  - Test and fixture transport used for in-memory connectivity.
- `local/`
  - Parent/child transport for in-process zone hierarchies without DLL loading.
- `dynamic_library/`
  - Blocking in-process DLL transport. The host loads a shared object and talks
    to it through a C ABI.
- `libcoro_dynamic_library/`
  - Coroutine in-process DLL transport. Similar purpose to `dynamic_library/`
    but the ABI is coroutine-oriented.
- `streaming/`
  - Stream-based transport for coroutine builds.
- `ipc_transport/`
  - Process-owning coroutine transport built on top of `stream_transport`. It
    creates a shared-memory SPSC queue pair, spawns a child process, and reaps
    it when the connection terminates.
- `libcoro_spsc_dynamic_dll/`
  - Coroutine DLL runtime that exposes a child zone behind an SPSC-backed
    `stream_transport`. It does not spawn processes itself.
- `sgx/`
  - SGX-specific transport support.

## How the newer transports relate

There are now three distinct concerns which used to be more tightly coupled:

1. Loading and running a child zone from a DLL in the current process
2. Moving bytes across an SPSC queue pair
3. Spawning and owning a child process

Those are implemented as:

- `dynamic_library/` and `libcoro_dynamic_library/`
  - DLL loading in the current process
- `libcoro_spsc_dynamic_dll/`
  - DLL runtime behind an SPSC stream
- `ipc_transport/`
  - Child-process ownership and SPSC queue creation

## Child process runtimes under `ipc_transport/`

- `ipc_transport/ipc_child_host_process/`
  - Maps the shared SPSC queue pair, loads a DLL, and forwards the queues into
    `libcoro_spsc_dynamic_dll`.
- `ipc_transport/ipc_child_process/`
  - Maps the shared SPSC queue pair and hosts a `rpc::stream_transport`
    directly in the child process executable.

These are helper executables, not transports in their own right.

## Practical combinations

- In-process DLL zone:
  - `dynamic_library/` or `libcoro_dynamic_library/`
- Out-of-process DLL zone:
  - `ipc_transport/` + `ipc_child_host_process/` + `libcoro_spsc_dynamic_dll/`
- Out-of-process direct child service:
  - `ipc_transport/` + `ipc_child_process/`

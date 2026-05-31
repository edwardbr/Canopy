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
- `blocking_dll/`
  - Blocking in-process DLL transport. The host loads a shared object and talks
    to it through a C ABI.
- `shared_scheduler_dll/`
  - Coroutine in-process DLL transport using the host scheduler and direct
    coroutine function pointers.
- `unshared_scheduler_dll/`
  - Coroutine in-process DLL transport with a DLL-owned scheduler and
    begin/complete dispatch ABI.
- `streaming/`
  - Stream-backed RPC transport used by TCP, TLS, WebSocket, SPSC, IPC, and
    io_uring compositions. The core stream transport, TCP, OpenSSL TLS, and
    WebSocket paths are dual-mode; SPSC, IPC, io_uring, and SGX stream
    compositions remain coroutine-only or conditionally built.
- `untrusted_web/`
  - Browser-facing RPC bridge for WebSocket clients. It uses the
    `websocket_protocol` wire messages but is named for the trust boundary:
    public web clients are untrusted and subject to size, handshake, decode,
    and inactivity limits.
- `ipc_spsc/`
  - Process-owning coroutine transport built on top of `stream_transport`. It
    creates a shared-memory SPSC queue pair, can spawn a child-host sidecar, and
    can load a DLL runtime behind that SPSC stream.
- `sgx/`
  - SGX-specific transport support.

## How the newer transports relate

There are now three distinct concerns which used to be more tightly coupled:

1. Loading and running a child zone from a DLL in the current process
2. Moving bytes across an SPSC queue pair
3. Optionally spawning and owning a child process

Those are implemented as:

- `blocking_dll/`, `shared_scheduler_dll/`, and
  `unshared_scheduler_dll/`
  - DLL loading in the current process
- `ipc_spsc/`
  - SPSC queue creation, optional child-process ownership, and the DLL runtime
    used by the generic sidecar process

## Sidecar and peer process support under `ipc_spsc/`

- `ipc_spsc/sidecar_process/`
  - Builds the `canopy_ipc_spsc_sidecar_process` executable. It maps the shared
    SPSC queue pair, loads a DLL, and forwards the queues into
    `ipc_spsc`.
- `ipc_spsc/sidecar_process/include/`
  - Header-only helpers for application-owned direct process executables.
  - Concrete service interfaces stay in the application or test executable.
- Shared-file peer mode:
  - One independently launched process creates a unique shared-memory file and
    accepts.
  - The other process opens the same file and connects.

These are helper executables and peer bootstrap APIs, not transports in their own right.

## Practical combinations

- In-process DLL zone:
  - `blocking_dll/`, `shared_scheduler_dll/`, or
    `unshared_scheduler_dll/`
- Out-of-process DLL zone:
  - `ipc_spsc/` + `canopy_ipc_spsc_sidecar_process`
- Out-of-process direct child service:
  - `ipc_spsc/` + an application-owned child executable using the IPC SPSC bootstrap helpers
- Independently launched process pair:
  - `ipc_spsc/` + a unique shared-memory file path known by both processes
